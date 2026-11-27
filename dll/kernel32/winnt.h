#pragma once

#include "types.h"
#include "errhandlingapi.h"

void WINAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue);
void WINAPI RtlUnwindEx(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue,
						PCONTEXT ContextRecord, PVOID HistoryTable);
// kernel32 forwards these to ntdll; the host implementations live in dll/ntdll.c.
GUEST_PTR WINAPI RtlLookupFunctionEntry(ULONGLONG ControlPc, GUEST_PTR ImageBase, GUEST_PTR HistoryTable);
GUEST_PTR WINAPI RtlVirtualUnwind(DWORD HandlerType, ULONGLONG ImageBase, ULONGLONG ControlPc, GUEST_PTR FunctionEntry,
								  GUEST_PTR ContextRecord, GUEST_PTR HandlerData, GUEST_PTR EstablisherFrame,
								  GUEST_PTR ContextPointers);
