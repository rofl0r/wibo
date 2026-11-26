#include "winnt.h"

#include "common.h"
#include "context.h"
#include "internal.h"

void WINAPI RtlUnwindEx(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue,
						PCONTEXT ContextRecord, PVOID HistoryTable) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlUnwindEx(%p, %p, %p, %p, %p, %p)\n", TargetFrame, TargetIp, ExceptionRecord, ReturnValue,
			  ContextRecord, HistoryTable);
	// ContextRecord and HistoryTable are not filled in or consulted; no caller in the supported set reads them.
	(void)ContextRecord;
	(void)HistoryTable;
	kernel32_unwindTo((uint64_t)(uintptr_t)TargetFrame, (uint64_t)(uintptr_t)TargetIp,
					  (EXCEPTION_RECORD *)fromGuestPtr(ExceptionRecord), (uint64_t)(uintptr_t)ReturnValue);
}

void WINAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue) {
	RtlUnwindEx(TargetFrame, TargetIp, ExceptionRecord, ReturnValue, GUEST_NULL, GUEST_NULL);
}
