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
__attribute__((no_stack_protector)) static _Noreturn void reportUnhandledException(EXCEPTION_RECORD *record) {
	if (g_topLevelExceptionFilter) {
		EXCEPTION_POINTERS pointers = {toGuestPtr(record), 0};
		call_LPTOP_LEVEL_EXCEPTION_FILTER(g_topLevelExceptionFilter, &pointers);
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

#if defined(__i386__)
_Static_assert(sizeof(CONTEXT) == 716, "CONTEXT must match the i386 Windows layout");

// Walks the guest's fs:[0] chain. A frame is only considered when it lies between the faulting or raising guest code
// and the innermost host-to-guest boundary. Frames past the boundary belong to guest callers of host code, and calling
// their handlers would skip the host frames in between. Returns true only if a handler returned
// EXCEPTION_CONTINUE_EXECUTION; the handler may have changed ctx.
__attribute__((no_stack_protector)) static bool guestDispatchRegistrations(TEB *teb, EXCEPTION_RECORD *record,
																		  CONTEXT *ctx, uintptr_t lowerBound) {
	uintptr_t upperBound = (uintptr_t)teb->GuestBoundary;
	GUEST_PTR frame = teb->Tib.ExceptionList;
	while (frame != (GUEST_PTR)-1) {
		if (frame < lowerBound || frame >= upperBound || (frame & 3)) {
			return false;
		}
		const EXCEPTION_REGISTRATION *registration = (const EXCEPTION_REGISTRATION *)(uintptr_t)frame;
		GUEST_PTR next = registration->Next;
		LONG disposition = call_PEXCEPTION_ROUTINE_CDECL((PEXCEPTION_ROUTINE_CDECL)(uintptr_t)registration->Handler,
														 toGuestPtr(record), frame, toGuestPtr(ctx), 0);
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

// A continuable RaiseException from guest code returns normally when a handler continues without changing the
// context. Redirecting it would require discarding the RaiseException host frame and its thunk, which is not
// supported, so such a handler falls through to the unhandled path.
__attribute__((no_stack_protector)) static bool resumeRaisedException(EXCEPTION_RECORD *record) {
	TEB *teb = currentThreadTeb;
	// Without a host-to-guest boundary, TEB.CurrentStackPointer is not a guest return slot.
	if (!teb->GuestBoundary || (record->ExceptionFlags & EXCEPTION_NONCONTINUABLE)) {
		return false;
	}
	// The g2h thunk stores its guest stack slot here: [slot] holds the guest ebp and [slot + 4] the return address.
	uintptr_t slot = (uintptr_t)teb->CurrentStackPointer;
	CONTEXT original;
	memset(&original, 0, sizeof(original));
	original.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	original.Ebp = *(const uint32_t *)slot;
	original.Eip = *(const uint32_t *)(slot + 4);
	original.Esp = (uint32_t)(slot + 8);
	record->ExceptionAddress = original.Eip;
	CONTEXT ctx = original;
	if (!guestDispatchRegistrations(teb, record, &ctx, slot + 8)) {
		return false;
	}
	return memcmp(&ctx, &original, sizeof(ctx)) == 0;
}
#endif

// Guest code runs on the host, so a bad guest access arrives as a host signal. The alternate stack lets a
// stack overflow be reported too. Only the main thread gets one; other threads report on their own stack.
static char g_faultAltStack[64 * 1024];

static void faultRegisters(void *uctx, uintptr_t *ip, uintptr_t *err) {
	*ip = 0;
	*err = 0;
#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
	ucontext_t *uc = uctx;
#if defined(__x86_64__)
	*ip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
	*ip = (uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
#endif
	*err = (uintptr_t)uc->uc_mcontext.gregs[REG_ERR];
#else
	(void)uctx;
#endif
}

#if defined(__linux__) && defined(__x86_64__)
// A 32-bit guest keeps its TEB in fs, so the host's TLS is unreachable until the host fs base is back. The
// host-to-guest thunks save that base in TEB.HostFsBase. A fault from the 64-bit host (cs unchanged) leaves fs alone.
__attribute__((no_stack_protector)) static void restoreHostFsForGuestFault(void *uctx) {
	const ucontext_t *uc = uctx;
	uint16_t handlerCs;
	__asm__("mov %%cs, %0" : "=r"(handlerCs));
	if ((uint16_t)(uc->uc_mcontext.gregs[REG_CSGSFS] & 0xffff) == handlerCs) {
		return;
	}
	unsigned long guestFs = 0;
	if (syscall(SYS_arch_prctl, ARCH_GET_FS, &guestFs) != 0 || guestFs == 0) {
		return;
	}
	TEB *teb = (TEB *)(uintptr_t)guestFs;
	// The thunks take the guest stack from TEB.CurrentStackPointer. While the guest runs, that field holds the host
	// stack, so it has to be set to the faulting guest esp before the filter is called.
	teb->CurrentStackPointer = (void *)(uintptr_t)(uint32_t)uc->uc_mcontext.gregs[REG_RSP];
	syscall(SYS_arch_prctl, ARCH_SET_FS, (unsigned long)(uintptr_t)teb->HostFsBase);
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

// The guest-running state that a fault dispatch has to put back before a handler can resume the guest.
typedef struct {
	void *guestSp;
	uint16_t hostFs;
	uint16_t hostGs;
} GuestFaultState;

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
	state->guestSp = teb->CurrentStackPointer;
	state->hostFs = hostFs;
	state->hostGs = hostGs;
	teb->CurrentStackPointer = (void *)(uintptr_t)(uint32_t)uc->uc_mcontext.gregs[REG_ESP];
	teb->CurrentFsSelector = guestFs;
	teb->CurrentGsSelector = guestGs;
	__asm__ volatile("mov %0, %%fs\n\tmov %1, %%gs" : : "r"(hostFs), "r"(hostGs));
	return teb;
}

// Resumes the guest at the context a handler left behind. Returns false if no handler continued execution. The TEB
// is then left as the filter and report expect it: host segments, with the guest stack pointer for the filter.
__attribute__((no_stack_protector)) static bool resumeGuestFault(TEB *teb, const GuestFaultState *state,
																EXCEPTION_RECORD *record, void *uctx) {
	ucontext_t *uc = uctx;
	greg_t *g = uc->uc_mcontext.gregs;
	CONTEXT ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	ctx.Eip = (uint32_t)g[REG_EIP];
	ctx.Esp = (uint32_t)g[REG_ESP];
	ctx.Ebp = (uint32_t)g[REG_EBP];
	ctx.Eax = (uint32_t)g[REG_EAX];
	ctx.Ebx = (uint32_t)g[REG_EBX];
	ctx.Ecx = (uint32_t)g[REG_ECX];
	ctx.Edx = (uint32_t)g[REG_EDX];
	ctx.Esi = (uint32_t)g[REG_ESI];
	ctx.Edi = (uint32_t)g[REG_EDI];
	ctx.EFlags = (uint32_t)g[REG_EFL];
	if (!guestDispatchRegistrations(teb, record, &ctx, (uintptr_t)ctx.Esp)) {
		return false;
	}
	// Guest-running state again. The sigreturn restores the guest fs and gs from the ucontext.
	teb->CurrentFsSelector = state->hostFs;
	teb->CurrentGsSelector = state->hostGs;
	teb->CurrentStackPointer = state->guestSp;
	g[REG_EIP] = ctx.Eip;
	g[REG_ESP] = ctx.Esp;
	g[REG_EBP] = ctx.Ebp;
	g[REG_EAX] = ctx.Eax;
	g[REG_EBX] = ctx.Ebx;
	g[REG_ECX] = ctx.Ecx;
	g[REG_EDX] = ctx.Edx;
	g[REG_ESI] = ctx.Esi;
	g[REG_EDI] = ctx.Edi;
	g[REG_EFL] = ctx.EFlags;
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
#if defined(__linux__) && defined(__i386__)
	GuestFaultState state = {0};
	TEB *guestTeb = restoreHostSegmentsForGuestFault(uctx, &state);
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
		record.ExceptionInformation[1] = (ULONG_PTR)info->si_addr;
	} else if (sig == SIGILL) {
		record.ExceptionCode = STATUS_ILLEGAL_INSTRUCTION;
	} else if (info->si_code == FPE_INTDIV) {
		record.ExceptionCode = STATUS_INTEGER_DIVIDE_BY_ZERO;
	} else if (info->si_code == FPE_INTOVF) {
		record.ExceptionCode = STATUS_INTEGER_OVERFLOW;
	} else {
		record.ExceptionCode = STATUS_FLOAT_DIVIDE_BY_ZERO;
	}
#if defined(__linux__) && defined(__x86_64__)
	restoreHostFsForGuestFault(uctx);
#endif
#if defined(__linux__) && defined(__i386__)
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
#if defined(__i386__)
	if (resumeRaisedException(&record)) {
		return;
	}
#endif
#if defined(WIBO_GUEST64)
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
