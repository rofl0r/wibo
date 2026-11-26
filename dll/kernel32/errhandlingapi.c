#include "errhandlingapi.h"

#include "common.h"
#include "context.h"
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

// The stack protector reads the canary through %gs on i386, which is not reliable in a signal frame.
// SA_NODEFER is not set, so a fault while this handler runs is fatal (the kernel resets to SIG_DFL).
__attribute__((no_stack_protector)) static _Noreturn void reportHostFault(int sig, siginfo_t *info, void *uctx) {
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
	record.ExceptionAddress = toGuestPtr(__builtin_return_address(0));
	if (lpArguments) {
		DWORD count = nNumberOfArguments < (DWORD)EXCEPTION_MAXIMUM_PARAMETERS ? nNumberOfArguments
																			   : (DWORD)EXCEPTION_MAXIMUM_PARAMETERS;
		record.NumberParameters = count;
		memcpy(record.ExceptionInformation, lpArguments, count * sizeof(ULONG_PTR));
	}
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
