#pragma once

#include "types.h"

#define EXCEPTION_MAXIMUM_PARAMETERS 15

typedef struct {
	DWORD ExceptionCode;
	DWORD ExceptionFlags;
	GUEST_PTR ExceptionRecord;
	GUEST_PTR ExceptionAddress;
	DWORD NumberParameters;
	ULONG_PTR ExceptionInformation[EXCEPTION_MAXIMUM_PARAMETERS];
} EXCEPTION_RECORD;

typedef GUEST_PTR PEXCEPTION_RECORD;
typedef GUEST_PTR PCONTEXT;

typedef struct {
	PEXCEPTION_RECORD ExceptionRecord;
	PCONTEXT ContextRecord;
} EXCEPTION_POINTERS;

typedef EXCEPTION_POINTERS *PEXCEPTION_POINTERS;
typedef LONG(_CC_STDCALL *PVECTORED_EXCEPTION_HANDLER)(PEXCEPTION_POINTERS ExceptionInfo);
typedef LONG(_CC_STDCALL *LPTOP_LEVEL_EXCEPTION_FILTER)(PEXCEPTION_POINTERS ExceptionInfo);

#define EXCEPTION_CONTINUE_EXECUTION ((LONG)-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_EXECUTE_HANDLER 1

DWORD WINAPI GetLastError(void);
void WINAPI SetLastError(DWORD dwErrCode);
void WINAPI RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags, DWORD nNumberOfArguments,
						   const ULONG_PTR *lpArguments);
PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetUnhandledExceptionFilter(LPTOP_LEVEL_EXCEPTION_FILTER lpTopLevelExceptionFilter);
LONG WINAPI UnhandledExceptionFilter(PEXCEPTION_POINTERS ExceptionInfo);
UINT WINAPI SetErrorMode(UINT uMode);
