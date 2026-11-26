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

// i386 CONTEXT layout (winnt.h). Only the 32-bit guest path reads or writes it.
#define CONTEXT_i386 0x00010000
#define CONTEXT_CONTROL (CONTEXT_i386 | 0x1)
#define CONTEXT_INTEGER (CONTEXT_i386 | 0x2)
#define CONTEXT_SEGMENTS (CONTEXT_i386 | 0x4)
#define CONTEXT_FULL (CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS)

typedef struct {
	DWORD ContextFlags;
	DWORD Dr0;
	DWORD Dr1;
	DWORD Dr2;
	DWORD Dr3;
	DWORD Dr6;
	DWORD Dr7;
	BYTE FloatSave[112];
	DWORD SegGs;
	DWORD SegFs;
	DWORD SegEs;
	DWORD SegDs;
	DWORD Edi;
	DWORD Esi;
	DWORD Ebx;
	DWORD Edx;
	DWORD Ecx;
	DWORD Eax;
	DWORD Ebp;
	DWORD Eip;
	DWORD SegCs;
	DWORD EFlags;
	DWORD Esp;
	DWORD SegSs;
	BYTE ExtendedRegisters[512];
} CONTEXT;

typedef GUEST_PTR PCONTEXT;

// A raw __try/__except frame as mingw's __try1 links it into fs:[0].
typedef struct {
	GUEST_PTR Next;
	GUEST_PTR Handler;
} EXCEPTION_REGISTRATION;

#define EXCEPTION_NONCONTINUABLE 0x1
#define ExceptionContinueExecution 0
#define ExceptionContinueSearch 1
#define ExceptionNestedException 2
#define ExceptionCollidedUnwind 3

typedef LONG(_CC_CDECL *PEXCEPTION_ROUTINE_CDECL)(PEXCEPTION_RECORD ExceptionRecord, GUEST_PTR EstablisherFrame,
												  PCONTEXT ContextRecord, GUEST_PTR DispatcherContext);

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
