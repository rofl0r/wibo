#include "errhandlingapi.h"

#include "common.h"
#include "context.h"
#include "context64.h"
#include "errors.h"
#include "internal.h"
#include "kernel32.h"
#include "kernel32_trampolines.h"
#include "types.h"

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#if defined(__linux__) && defined(__x86_64__)
#include <asm/prctl.h>
#endif
#if defined(__linux__) && defined(__i386__)
#include <asm/ldt.h>
#endif

static LPTOP_LEVEL_EXCEPTION_FILTER g_topLevelExceptionFilter = NULL;
static UINT g_processErrorMode = 0;

DWORD kernel32_getLastError(void) { return currentThreadTeb->LastErrorValue; }

void kernel32_setLastError(DWORD error) { currentThreadTeb->LastErrorValue = error; }

void kernel32_setLastErrorFromErrno(void) { kernel32_setLastError(wibo_winErrorFromErrno(errno)); }

DWORD WINAPI GetLastError(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLastError() -> %u\n", kernel32_getLastError());
	return currentThreadTeb->LastErrorValue;
}

void WINAPI SetLastError(DWORD dwErrCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetLastError(%u)\n", dwErrCode);
	currentThreadTeb->LastErrorValue = dwErrCode;
}

// The report is formatted into a fixed buffer and written with a raw syscall. Host fault handlers run in a
// signal frame where glibc's TLS (%gs on i386) may be unusable, so stdio and errno-setting wrappers are avoided.
typedef struct {
	char text[256];
	size_t len;
} FaultReport;

static void faultReportAppend(FaultReport *report, const char *s) {
	while (*s && report->len < sizeof(report->text) - 1) {
		report->text[report->len++] = *s++;
	}
}

__attribute__((no_stack_protector)) static void faultReportAppendHex(FaultReport *report, unsigned long long value) {
	char digits[16];
	int count = 0;
	do {
		digits[count++] = "0123456789abcdef"[value & 0xf];
		value >>= 4;
	} while (value);
	faultReportAppend(report, "0x");
	while (count) {
		char c[2] = {digits[--count], 0};
		faultReportAppend(report, c);
	}
}

// Unhandled exceptions skip the VEH and __try/__except chains (not implemented). The top-level filter runs,
// then the process exits with the low byte of the exception code, as Wine does on Linux.
// A host fault has no resumable context here, so the process exits even if the filter returns
// EXCEPTION_CONTINUE_EXECUTION. ContextRecord is passed as NULL.
#if !defined(WIBO_GUEST64)
// Guest handlers take 32-bit pointers, but host stacks lie above 4 GB on x86_64. The exception state is therefore
// copied to the guest stack, where the guest's own 32-bit code can address it.
typedef struct {
	EXCEPTION_RECORD record;
	CONTEXT context;
	EXCEPTION_POINTERS pointers;
} GuestExceptionFrame;

static GuestExceptionFrame *stageGuestExceptionFrame(TEB *teb, const EXCEPTION_RECORD *record, const CONTEXT *ctx);
#endif

__attribute__((no_stack_protector)) static _Noreturn void reportUnhandledException(EXCEPTION_RECORD *record) {
	if (g_topLevelExceptionFilter) {
#if defined(__x86_64__) && !defined(WIBO_GUEST64)
		GuestExceptionFrame *frame = stageGuestExceptionFrame(currentThreadTeb, record, NULL);
		if (frame) {
			call_LPTOP_LEVEL_EXCEPTION_FILTER(g_topLevelExceptionFilter, &frame->pointers);
		}
#else
		EXCEPTION_POINTERS pointers = {toGuestPtr(record), 0};
		call_LPTOP_LEVEL_EXCEPTION_FILTER(g_topLevelExceptionFilter, &pointers);
#endif
	}
	FaultReport report;
	report.len = 0;
	faultReportAppend(&report, "wibo: unhandled exception ");
	faultReportAppendHex(&report, record->ExceptionCode);
	if (record->ExceptionCode == (DWORD)STATUS_ACCESS_VIOLATION && record->NumberParameters >= 2) {
		faultReportAppend(&report, " (access violation: ");
		faultReportAppend(&report, record->ExceptionInformation[0] ? "write" : "read");
		faultReportAppend(&report, " of ");
		faultReportAppendHex(&report, record->ExceptionInformation[1]);
		faultReportAppend(&report, ")");
	}
	faultReportAppend(&report, " at ");
	faultReportAppendHex(&report, record->ExceptionAddress);
	faultReportAppend(&report, "\n");
	syscall(SYS_write, 2, report.text, report.len);
	// Skips kernel32_exitInternal: the handle table may be locked by the code that faulted.
	_exit((int)record->ExceptionCode);
}

#if !defined(WIBO_GUEST64)
_Static_assert(sizeof(CONTEXT) == 716, "CONTEXT must match the i386 Windows layout");

// Copies the exception state below the guest stack pointer (TEB.CurrentStackPointer). Returns NULL outside a
// host-to-guest call, when that pointer is not a guest stack. A NULL ctx gives the handlers a NULL ContextRecord.
__attribute__((no_stack_protector)) static GuestExceptionFrame *stageGuestExceptionFrame(TEB *teb,
																						 const EXCEPTION_RECORD *record,
																						 const CONTEXT *ctx) {
	if (!teb->GuestBoundary) {
		return NULL;
	}
	uintptr_t sp = (uintptr_t)teb->CurrentStackPointer;
	GuestExceptionFrame *frame = (GuestExceptionFrame *)((sp - sizeof(*frame)) & ~(uintptr_t)15);
	frame->record = *record;
	if (ctx) {
		frame->context = *ctx;
	} else {
		memset(&frame->context, 0, sizeof(frame->context));
	}
	frame->pointers.ExceptionRecord = toGuestPtr(&frame->record);
	frame->pointers.ContextRecord = ctx ? toGuestPtr(&frame->context) : 0;
	return frame;
}

// Walks the guest's fs:[0] chain. A frame is only considered when it lies between the faulting or raising guest code
// and the innermost host-to-guest boundary. Frames past the boundary belong to guest callers of host code, and calling
// their handlers would skip the host frames in between. Returns true only if a handler returned
// EXCEPTION_CONTINUE_EXECUTION; the handler may have changed the staged context.
__attribute__((no_stack_protector)) static bool walkGuestRegistrations(TEB *teb, GuestExceptionFrame *staged,
																	   uintptr_t lowerBound) {
	uintptr_t upperBound = (uintptr_t)teb->GuestBoundary;
	GUEST_PTR frame = teb->Tib.ExceptionList;
	while (frame != (GUEST_PTR)-1) {
		if (frame < lowerBound || frame >= upperBound || (frame & 3)) {
			return false;
		}
		const EXCEPTION_REGISTRATION *registration = (const EXCEPTION_REGISTRATION *)(uintptr_t)frame;
		GUEST_PTR next = registration->Next;
		LONG disposition = call_PEXCEPTION_ROUTINE_CDECL((PEXCEPTION_ROUTINE_CDECL)(uintptr_t)registration->Handler,
														 staged->pointers.ExceptionRecord, frame,
														 staged->pointers.ContextRecord, 0);
		if (disposition == ExceptionContinueExecution) {
			return true;
		}
		if (disposition != ExceptionContinueSearch) {
			return false;
		}
		frame = next;
	}
	return false;
}

// Runs the guest handlers for record with ctx as their context. The guest stack pointer is moved below the staged
// frame while they run. On success, ctx receives the context the handler left.
__attribute__((no_stack_protector)) static bool dispatchGuestRegistrations(TEB *teb, EXCEPTION_RECORD *record,
																		   CONTEXT *ctx, uintptr_t lowerBound) {
	void *savedSp = teb->CurrentStackPointer;
	GuestExceptionFrame *staged = stageGuestExceptionFrame(teb, record, ctx);
	if (!staged) {
		return false;
	}
	teb->CurrentStackPointer = staged;
	bool continued = walkGuestRegistrations(teb, staged, lowerBound);
	teb->CurrentStackPointer = savedSp;
	if (continued) {
		*ctx = staged->context;
	}
	return continued;
}

// The g2h thunk records the guest's stack at its entry, as the offsets below from TEB.CurrentStackPointer. Its
// pushes differ by host: the i386 host pushes only EBP, while the x86_64 host pushes EBP, ESI, EDI, and EBX.
#if defined(__x86_64__)
enum { kRaiseSlotEbp = 12, kRaiseSlotEip = 16, kRaiseSlotEsp = 20 };
#else
enum { kRaiseSlotEbp = 0, kRaiseSlotEip = 4, kRaiseSlotEsp = 8 };
#endif

// A continuable RaiseException from guest code returns normally when a handler continues without changing the
// context. Redirecting it would require discarding the RaiseException host frame and its thunk, which is not
// supported, so such a handler falls through to the unhandled path.
__attribute__((no_stack_protector)) static bool resumeRaisedException(EXCEPTION_RECORD *record) {
	TEB *teb = currentThreadTeb;
	// Without a host-to-guest boundary, TEB.CurrentStackPointer is not a guest return slot.
	if (!teb->GuestBoundary || (record->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) {
		return false;
	}
	uintptr_t slot = (uintptr_t)teb->CurrentStackPointer;
	CONTEXT original;
	memset(&original, 0, sizeof(original));
	original.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	original.Ebp = *(const uint32_t *)(slot + kRaiseSlotEbp);
	original.Eip = *(const uint32_t *)(slot + kRaiseSlotEip);
	original.Esp = (uint32_t)(slot + kRaiseSlotEsp);
	record->ExceptionAddress = original.Eip;
	CONTEXT ctx = original;
	if (!dispatchGuestRegistrations(teb, record, &ctx, slot + kRaiseSlotEsp)) {
		return false;
	}
	return memcmp(&ctx, &original, sizeof(ctx)) == 0;
}
#endif

// Guest code runs on the host, so a bad guest access arrives as a host signal. The alternate stack lets a
// stack overflow be reported too. Only the main thread gets one; other threads report on their own stack.
static char g_faultAltStack[64 * 1024];

// The guest's 32-bit registers as the ucontext stores them. On x86_64 hosts they occupy the low halves of the 64-bit
// slots, and the high halves are dropped when the guest context is rebuilt.
#if defined(__linux__) && defined(__x86_64__)
#define GREG_EAX REG_RAX
#define GREG_EBX REG_RBX
#define GREG_ECX REG_RCX
#define GREG_EDX REG_RDX
#define GREG_ESI REG_RSI
#define GREG_EDI REG_RDI
#define GREG_EBP REG_RBP
#define GREG_ESP REG_RSP
#define GREG_EIP REG_RIP
#elif defined(__linux__) && defined(__i386__)
#define GREG_EAX REG_EAX
#define GREG_EBX REG_EBX
#define GREG_ECX REG_ECX
#define GREG_EDX REG_EDX
#define GREG_ESI REG_ESI
#define GREG_EDI REG_EDI
#define GREG_EBP REG_EBP
#define GREG_ESP REG_ESP
#define GREG_EIP REG_EIP
#endif

// The guest-running state that a fault dispatch has to put back before a handler can resume the guest.
typedef struct {
	void *hostSp;
	uint16_t hostFs;
	uint16_t hostGs;
} GuestFaultState;

static void faultRegisters(void *uctx, uintptr_t *ip, uintptr_t *err) {
	*ip = 0;
	*err = 0;
#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
	ucontext_t *uc = uctx;
	*ip = (uintptr_t)uc->uc_mcontext.gregs[GREG_EIP];
	*err = (uintptr_t)uc->uc_mcontext.gregs[REG_ERR];
#else
	(void)uctx;
#endif
}

#if defined(__linux__) && defined(__x86_64__) && !defined(WIBO_GUEST64)
// A 32-bit guest keeps its TEB in fs, so the host's TLS is unreachable until the host fs base is back. The
// host-to-guest thunks save that base in TEB.HostFsBase. A fault from the 64-bit host (cs unchanged) leaves fs alone
// and returns NULL. Otherwise the TEB is returned with TEB.CurrentStackPointer set to the faulting guest esp.
__attribute__((no_stack_protector)) static TEB *restoreHostFsForGuestFault(void *uctx, GuestFaultState *state) {
	const ucontext_t *uc = uctx;
	uint16_t handlerCs;
	__asm__("mov %%cs, %0" : "=r"(handlerCs));
	if ((uint16_t)(uc->uc_mcontext.gregs[REG_CSGSFS] & 0xffff) == handlerCs) {
		return NULL;
	}
	unsigned long guestFs = 0;
	if (syscall(SYS_arch_prctl, ARCH_GET_FS, &guestFs) != 0 || guestFs == 0) {
		return NULL;
	}
	TEB *teb = (TEB *)(uintptr_t)guestFs;
	if (teb->Tib.Self != toGuestPtr(teb) || !teb->HostFsBase) {
		return NULL;
	}
	// While guest code runs, TEB.CurrentStackPointer holds the host stack. The guest stack is set for the handlers
	// and restored on resume.
	state->hostSp = teb->CurrentStackPointer;
	teb->CurrentStackPointer = (void *)(uintptr_t)(uint32_t)uc->uc_mcontext.gregs[GREG_ESP];
	syscall(SYS_arch_prctl, ARCH_SET_FS, (unsigned long)(uintptr_t)teb->HostFsBase);
	return teb;
}
#endif

#if defined(__linux__) && defined(__i386__)
// glibc's syscall() reads TLS through %gs, which is 0 in the faulting guest context, so issue the syscall directly.
// %ebx is reserved for PIC, so the first argument is passed in %esi and swapped into %ebx around the trap.
__attribute__((no_stack_protector)) static long rawSyscall1(long nr, long a1) {
	long ret;
	__asm__ volatile("xchgl %%ebx, %%esi\n\tint $0x80\n\txchgl %%ebx, %%esi"
	                 : "=a"(ret)
	                 : "a"(nr), "S"(a1)
	                 : "memory");
	return ret;
}

// A 32-bit guest runs with the TEB in fs and gs = 0. The thunks swap fs and gs with TEB.CurrentFsSelector and
// TEB.CurrentGsSelector on every transition, so those fields hold the host selectors while guest code runs.
// Swap the host selectors back before any host code runs, and store the guest selectors so the filter can
// call back into the guest. Returns the TEB, or NULL if the fault did not come from guest code.
__attribute__((no_stack_protector)) static TEB *restoreHostSegmentsForGuestFault(void *uctx, GuestFaultState *state) {
	const ucontext_t *uc = uctx;
	uint16_t guestFs = (uint16_t)uc->uc_mcontext.gregs[REG_FS];
	uint16_t guestGs = (uint16_t)uc->uc_mcontext.gregs[REG_GS];
	// Only GDT thread-area entries are handled. The LDT fallback is not.
	if ((guestFs & 4) || guestFs < 8) {
		return NULL;
	}
	struct user_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.entry_number = guestFs >> 3;
	if (rawSyscall1(SYS_get_thread_area, (long)(uintptr_t)&desc) != 0) {
		return NULL;
	}
	TEB *teb = (TEB *)(uintptr_t)desc.base_addr;
	if (!teb || teb->Tib.Self != toGuestPtr(teb)) {
		return NULL;
	}
	uint16_t hostFs = teb->CurrentFsSelector;
	uint16_t hostGs = teb->CurrentGsSelector;
	state->hostSp = teb->CurrentStackPointer;
	state->hostFs = hostFs;
	state->hostGs = hostGs;
	teb->CurrentStackPointer = (void *)(uintptr_t)(uint32_t)uc->uc_mcontext.gregs[REG_ESP];
	teb->CurrentFsSelector = guestFs;
	teb->CurrentGsSelector = guestGs;
	__asm__ volatile("mov %0, %%fs\n\tmov %1, %%gs" : : "r"(hostFs), "r"(hostGs));
	return teb;
}

#endif

#if defined(__linux__) && !defined(WIBO_GUEST64)
#if defined(__linux__) && defined(__x86_64__)
// Entered by sigreturn with the guest context in host registers: rax/rbx/rcx/rdx/rsi/rdi/rbp hold the guest's
// general registers, r8 eip, r9 esp, r11 fs selector, r12 gs selector, and rflags the guest's EFLAGS. Nothing is read
// from memory, so the context does not depend on a stack frame surviving sigreturn. A null selector is not loaded,
// because that would clear the fs base that resumeGuestFault set. The far return switches to the 32-bit code segment
// (0x23) and sets the guest's esp last.
__asm__(".text\n"
		".globl wiboResumeGuest32\n"
		".type wiboResumeGuest32, @function\n"
		"wiboResumeGuest32:\n"
		".intel_syntax noprefix\n"
		"\ttest r11w, r11w\n"
		"\tjz 1f\n"
		"\tmov fs, r11w\n"
		"1:\n"
		"\ttest r12w, r12w\n"
		"\tjz 2f\n"
		"\tmov gs, r12w\n"
		"2:\n"
		"\tmov rsp, r9\n"
		"\tpush 0x23\n"
		"\tpush r8\n"
		"\tretfq\n"
		".att_syntax prefix\n"
		".size wiboResumeGuest32, .-wiboResumeGuest32\n"
		".previous\n");
extern char wiboResumeGuest32[];
#endif

// Resumes the guest at the context a handler left behind. Returns false if no handler continued execution. The TEB
// is then left as the filter and report expect it: host segments, with the guest stack pointer for the filter.
__attribute__((no_stack_protector)) static bool resumeGuestFault(TEB *teb, GuestFaultState *state,
																EXCEPTION_RECORD *record, void *uctx) {
	ucontext_t *uc = uctx;
	greg_t *g = uc->uc_mcontext.gregs;
	CONTEXT ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	ctx.Eip = (uint32_t)g[GREG_EIP];
	ctx.Esp = (uint32_t)g[GREG_ESP];
	ctx.Ebp = (uint32_t)g[GREG_EBP];
	ctx.Eax = (uint32_t)g[GREG_EAX];
	ctx.Ebx = (uint32_t)g[GREG_EBX];
	ctx.Ecx = (uint32_t)g[GREG_ECX];
	ctx.Edx = (uint32_t)g[GREG_EDX];
	ctx.Esi = (uint32_t)g[GREG_ESI];
	ctx.Edi = (uint32_t)g[GREG_EDI];
	ctx.EFlags = (uint32_t)g[REG_EFL];
	if (!dispatchGuestRegistrations(teb, record, &ctx, (uintptr_t)ctx.Esp)) {
		return false;
	}
	teb->CurrentStackPointer = state->hostSp;
#if defined(__i386__)
	// Guest-running state again. The sigreturn restores the guest's segments from the ucontext.
	teb->CurrentFsSelector = state->hostFs;
	teb->CurrentGsSelector = state->hostGs;
	g[GREG_EIP] = ctx.Eip;
	g[GREG_ESP] = ctx.Esp;
	g[GREG_EBP] = ctx.Ebp;
	g[GREG_EAX] = ctx.Eax;
	g[GREG_EBX] = ctx.Ebx;
	g[GREG_ECX] = ctx.Ecx;
	g[GREG_EDX] = ctx.Edx;
	g[GREG_ESI] = ctx.Esi;
	g[GREG_EDI] = ctx.Edi;
	g[REG_EFL] = ctx.EFlags;
#else
	// The guest's selectors come from the TEB, since the signal frame does not reliably carry them. The fs base is
	// set to the TEB here, because sigreturn keeps the base for a null selector and the stub never loads one.
	uint16_t guestFs = teb->CurrentFsSelector;
	uint16_t guestGs = teb->CurrentGsSelector;
	syscall(SYS_arch_prctl, ARCH_SET_FS, (unsigned long)(uintptr_t)teb);
	uint16_t hostCs;
	__asm__("mov %%cs, %0" : "=r"(hostCs));
	// Host code selector with null fs and gs, so the sigreturn keeps the fs base set above.
	g[REG_CSGSFS] = (greg_t)(((uint64_t)g[REG_CSGSFS] & 0xffff000000000000ULL) | hostCs);
	g[REG_RIP] = (greg_t)(uintptr_t)wiboResumeGuest32;
	g[REG_EFL] = ctx.EFlags;
	g[REG_RAX] = ctx.Eax;
	g[REG_RBX] = ctx.Ebx;
	g[REG_RCX] = ctx.Ecx;
	g[REG_RDX] = ctx.Edx;
	g[REG_RSI] = ctx.Esi;
	g[REG_RDI] = ctx.Edi;
	g[REG_RBP] = ctx.Ebp;
	g[REG_R8] = ctx.Eip;
	g[REG_R9] = ctx.Esp;
	g[REG_R11] = guestFs;
	g[REG_R12] = guestGs;
#endif
	return true;
}
#endif

#if defined(WIBO_GUEST64)
// One record per host-to-guest call, pushed by emitThunkWin64 (tools/gentramp.c). Records form a chain through
// TEB.GuestCallFrame, innermost first, and live on the native stack. The layout must match the thunk's pushes. The
// guest's stack pointer at the call is the address just past the record, where the thunk's shadow space begins.
typedef struct GuestCallRecord {
	struct GuestCallRecord *prev;
	uint64_t r15, r14, r13, r12, rbx, rsi, rdi, rbp;
	uint64_t returnAddress;
} GuestCallRecord;

_Static_assert(sizeof(GuestCallRecord) == 80, "GuestCallRecord must match the emitThunkWin64 frame layout");

typedef WiboExceptionDisposition(__attribute__((ms_abi)) * WiboLanguageHandler)(EXCEPTION_RECORD *, uint64_t,
																		  WiboWin64Context *,
																		  WiboDispatcherContext *);

static const int kMaxGuestFrames = 4096;
static const int kGuestGprRegs[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
									  REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};

// The context of the guest function that called the innermost thunk, as seen from its return address.
static void guestCallContext(const GuestCallRecord *rec, WiboWin64Context *ctx) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->ContextFlags = CONTEXT_AMD64_CONTROL | CONTEXT_AMD64_INTEGER;
	ctx->Rip = rec->returnAddress;
	ctx->Rsp = (uint64_t)(uintptr_t)rec + sizeof(*rec);
	ctx->Rbp = rec->rbp;
	ctx->Rbx = rec->rbx;
	ctx->Rsi = rec->rsi;
	ctx->Rdi = rec->rdi;
	ctx->R12 = rec->r12;
	ctx->R13 = rec->r13;
	ctx->R14 = rec->r14;
	ctx->R15 = rec->r15;
}

static void contextFromUcontext(const ucontext_t *uc, WiboWin64Context *ctx) {
	const greg_t *g = uc->uc_mcontext.gregs;
	WiboContext64 regs;
	memset(&regs, 0, sizeof(regs));
	for (int i = 0; i < 16; i++) {
		regs.gpr[i] = (uint64_t)g[kGuestGprRegs[i]];
	}
	regs.rip = (uint64_t)g[REG_RIP];
	memset(ctx, 0, sizeof(*ctx));
	ctx->ContextFlags = CONTEXT_AMD64_CONTROL | CONTEXT_AMD64_INTEGER;
	wibo_contextFromUnwind(&regs, ctx);
	ctx->EFlags = (uint32_t)g[REG_EFL];
}

static void contextToUcontext(const WiboWin64Context *ctx, ucontext_t *uc) {
	greg_t *g = uc->uc_mcontext.gregs;
	WiboContext64 regs;
	wibo_contextToUnwind(ctx, &regs);
	for (int i = 0; i < 16; i++) {
		g[kGuestGprRegs[i]] = (greg_t)regs.gpr[i];
	}
	g[REG_RIP] = (greg_t)regs.rip;
	g[REG_EFL] = (greg_t)ctx->EFlags;
}

// Records of frames that the resumed code has abandoned lie below its stack pointer.
static void dropAbandonedGuestCallRecords(uint64_t rsp) {
	GuestCallRecord *rec = currentThreadTeb->GuestCallFrame;
	while (rec && (uint64_t)(uintptr_t)rec < rsp) {
		rec = rec->prev;
	}
	currentThreadTeb->GuestCallFrame = rec;
}

// Walks the stack outward from start and runs the language handler of each guest frame that has one. ContinueSearch
// moves to the caller frame. When the caller is host code, the next guest frame is the one recorded by the nearest
// outer guest call record whose return address is guest code, so host frames such as qsort are skipped. nextRec is
// the first record not yet consumed. Returns true if a handler returned ContinueExecution, leaving exCtx as the
// context to resume. Handlers receive exCtx as their ContextRecord so they can change it.
static bool guestDispatchFrames(EXCEPTION_RECORD *record, WiboWin64Context *exCtx, const WiboContext64 *start,
								bool startIsCaller, const GuestCallRecord *nextRec) {
	WiboContext64 frame = *start;
	bool isCaller = startIsCaller;
	for (int depth = 0; depth < kMaxGuestFrames; depth++) {
		uint64_t controlPc = isCaller ? frame.rip - 1 : frame.rip;
		if (!wibo_isRegisteredCode(controlPc)) {
			while (nextRec && !wibo_isRegisteredCode(nextRec->returnAddress)) {
				nextRec = nextRec->prev;
			}
			if (!nextRec) {
				return false;
			}
			WiboWin64Context callerCtx;
			guestCallContext(nextRec, &callerCtx);
			wibo_contextToUnwind(&callerCtx, &frame);
			nextRec = nextRec->prev;
			isCaller = true;
			continue;
		}
		uint64_t imageBase = 0;
		WiboRuntimeFunction function;
		bool hasFunction = wibo_lookupRuntimeFunction(controlPc, &imageBase, &function);
		WiboWin64Context frameContext;
		memset(&frameContext, 0, sizeof(frameContext));
		frameContext.ContextFlags = CONTEXT_AMD64_CONTROL | CONTEXT_AMD64_INTEGER;
		wibo_contextFromUnwind(&frame, &frameContext);

		WiboContext64 caller = frame;
		WiboFrameInfo info;
		if (!wibo_unwindWithFunction(&caller, controlPc, hasFunction ? imageBase : 0,
									 hasFunction ? &function : NULL, &info)) {
			return false;
		}
		if ((info.handlerFlags & UNW_FLAG_EHANDLER) && info.languageHandler) {
			WiboDispatcherContext dispatch;
			memset(&dispatch, 0, sizeof(dispatch));
			dispatch.ControlPc = controlPc;
			dispatch.ImageBase = imageBase;
			dispatch.FunctionEntry = hasFunction ? &function : NULL;
			dispatch.EstablisherFrame = info.establisherFrame;
			dispatch.ContextRecord = &frameContext;
			dispatch.LanguageHandler = info.languageHandler;
			dispatch.HandlerData = (void *)(uintptr_t)info.handlerData;
			WiboLanguageHandler handler = (WiboLanguageHandler)(uintptr_t)info.languageHandler;
			WiboExceptionDisposition disposition = handler(record, info.establisherFrame, exCtx, &dispatch);
			if (disposition == WiboExceptionContinueExecution) {
				return true;
			}
			if (disposition != WiboExceptionContinueSearch) {
				return false;
			}
		}
		frame = caller;
		isCaller = true;
	}
	return false;
}

// A continuable RaiseException from guest code returns normally when a handler continues without changing the
// context. Changing it would need the RaiseException host frame discarded, so such a handler reports the exception
// as unhandled, as the i386 path does.
static bool resumeRaisedException64(EXCEPTION_RECORD *record) {
	const GuestCallRecord *rec = currentThreadTeb->GuestCallFrame;
	if (!rec || (record->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) {
		return false;
	}
	WiboWin64Context original;
	guestCallContext(rec, &original);
	WiboContext64 start;
	wibo_contextToUnwind(&original, &start);
	WiboWin64Context ctx = original;
	if (!guestDispatchFrames(record, &ctx, &start, true, rec->prev)) {
		return false;
	}
	return memcmp(&ctx, &original, sizeof(ctx)) == 0;
}

// A fault in guest code dispatches from the faulting context. A fault inside host code (for example a host API given
// a bad guest pointer) is attributed to the innermost guest call, and ContinueExecution resumes at that call's return.
static bool dispatchHostFault(void *uctx, EXCEPTION_RECORD *record) {
	ucontext_t *uc = uctx;
	WiboWin64Context exCtx;
	contextFromUcontext(uc, &exCtx);
	WiboContext64 start;
	wibo_contextToUnwind(&exCtx, &start);
	const GuestCallRecord *rec = currentThreadTeb->GuestCallFrame;
	bool startIsCaller = false;
	const GuestCallRecord *nextRec = rec;
	if (!wibo_isRegisteredCode(start.rip)) {
		if (!rec) {
			return false;
		}
		guestCallContext(rec, &exCtx);
		wibo_contextToUnwind(&exCtx, &start);
		startIsCaller = true;
		nextRec = rec->prev;
	}
	if (!guestDispatchFrames(record, &exCtx, &start, startIsCaller, nextRec)) {
		return false;
	}
	contextToUcontext(&exCtx, uc);
	dropAbandonedGuestCallRecords(exCtx.Rsp);
	return true;
}
#endif

// The stack protector reads the canary through %gs on i386, which is not reliable in a signal frame.
// SA_NODEFER is not set, so a fault while this handler runs is fatal (the kernel resets to SIG_DFL).
// Returning from this function resumes the guest when a handler continued execution.
__attribute__((no_stack_protector)) static void reportHostFault(int sig, siginfo_t *info, void *uctx) {
#if defined(__linux__) && !defined(WIBO_GUEST64)
	GuestFaultState state = {0};
#if defined(__i386__)
	TEB *guestTeb = restoreHostSegmentsForGuestFault(uctx, &state);
#else
	TEB *guestTeb = restoreHostFsForGuestFault(uctx, &state);
#endif
#endif
	uintptr_t ip = 0;
	uintptr_t err = 0;
	faultRegisters(uctx, &ip, &err);

	EXCEPTION_RECORD record;
	memset(&record, 0, sizeof(record));
	record.ExceptionAddress = toGuestPtr((void *)ip);
		if (sig == SIGSEGV || sig == SIGBUS) {
			record.ExceptionCode = STATUS_ACCESS_VIOLATION;
			record.NumberParameters = 2;
		// The x86 page-fault error code has bit 1 set for writes.
		record.ExceptionInformation[0] = (err & 2) ? 1 : 0;
		record.ExceptionInformation[1] = (ULONG_PTR)(uintptr_t)info->si_addr;
	} else if (sig == SIGILL) {
		record.ExceptionCode = STATUS_ILLEGAL_INSTRUCTION;
	} else if (info->si_code == FPE_INTDIV) {
		record.ExceptionCode = STATUS_INTEGER_DIVIDE_BY_ZERO;
	} else if (info->si_code == FPE_INTOVF) {
		record.ExceptionCode = STATUS_INTEGER_OVERFLOW;
	} else {
		record.ExceptionCode = STATUS_FLOAT_DIVIDE_BY_ZERO;
	}
#if defined(__linux__) && !defined(WIBO_GUEST64)
	if (guestTeb && resumeGuestFault(guestTeb, &state, &record, uctx)) {
		return;
	}
#endif
#if defined(WIBO_GUEST64)
	if (dispatchHostFault(uctx, &record)) {
		return;
	}
#endif
	reportUnhandledException(&record);
}

void kernel32_installFaultHandlers(void) {
	stack_t altStack;
	memset(&altStack, 0, sizeof(altStack));
	altStack.ss_sp = g_faultAltStack;
	altStack.ss_size = sizeof(g_faultAltStack);
	sigaltstack(&altStack, NULL);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = reportHostFault;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
}

void WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
						   const ULONG_PTR *lpArguments) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RaiseException(0x%x, 0x%x, %u, %p)\n", dwExceptionCode, dwExceptionFlags, nNumberOfArguments,
			  lpArguments);
	EXCEPTION_RECORD record;
	memset(&record, 0, sizeof(record));
	record.ExceptionCode = dwExceptionCode;
	record.ExceptionFlags = dwExceptionFlags;
#if defined(WIBO_GUEST64)
	// The return address points into the thunk, so the raise site is the guest return address the thunk recorded.
	const GuestCallRecord *callRecord = currentThreadTeb->GuestCallFrame;
	record.ExceptionAddress = callRecord ? callRecord->returnAddress : (uint64_t)(uintptr_t)__builtin_return_address(0);
#else
	record.ExceptionAddress = toGuestPtr(__builtin_return_address(0));
#endif
	if (lpArguments) {
		DWORD count = nNumberOfArguments < (DWORD)EXCEPTION_MAXIMUM_PARAMETERS ? nNumberOfArguments
																			   : (DWORD)EXCEPTION_MAXIMUM_PARAMETERS;
		record.NumberParameters = count;
		memcpy(record.ExceptionInformation, lpArguments, count * sizeof(ULONG_PTR));
	}
#if !defined(WIBO_GUEST64)
	if (resumeRaisedException(&record)) {
		return;
	}
#else
	if (resumeRaisedException64(&record)) {
		return;
	}
#endif
	reportUnhandledException(&record);
}

PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: AddVectoredExceptionHandler(%u, %p)\n", First, Handler);
	return (PVOID)Handler;
}

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI
SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetUnhandledExceptionFilter(%p)\n", lpTopLevelExceptionFilter);
	LPTOP_LEVEL_EXCEPTION_FILTER previous = g_topLevelExceptionFilter;
	g_topLevelExceptionFilter = lpTopLevelExceptionFilter;
	return previous;
}

LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS ExceptionInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: UnhandledExceptionFilter(%p)\n", ExceptionInfo);
	return EXCEPTION_EXECUTE_HANDLER;
}

UINT WINAPI SetErrorMode(UINT uMode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetErrorMode(%u)\n", uMode);
	UINT previous = g_processErrorMode;
	g_processErrorMode = uMode;
	return previous;
}
