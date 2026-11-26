#include "errhandlingapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"

#include <errno.h>

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

void WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
						   const ULONG_PTR *lpArguments) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RaiseException(0x%x, 0x%x, %u, %p)\n", dwExceptionCode, dwExceptionFlags, nNumberOfArguments,
			  lpArguments);
	(void)dwExceptionFlags;
	(void)nNumberOfArguments;
	(void)lpArguments;
	kernel32_exitInternal(dwExceptionCode);
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
