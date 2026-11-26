#include "winnt.h"

#include "common.h"
#include "context.h"

#include <stdio.h>
#include <unistd.h>

// Landing on a target frame would discard host frames and the TEB boundary state, which is not implemented.
// Returning would resume the handler at the wrong place, so the process is stopped instead.
void WINAPI RtlUnwind(PVOID TargetFrame, PVOID TargetIp, PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlUnwind(%p, %p, %p, %p)\n", TargetFrame, TargetIp, ExceptionRecord, ReturnValue);
	fprintf(stderr, "wibo: RtlUnwind is not supported; cannot unwind to %p\n", TargetFrame);
	_exit(1);
}
