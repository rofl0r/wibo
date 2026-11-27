#include "debugapi.h"

#include "common.h"
#include "context.h"

BOOL WINAPI IsDebuggerPresent(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: IsDebuggerPresent()\n");
	return FALSE;
}

void WINAPI OutputDebugStringA(LPCSTR lpOutputString) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("OutputDebugStringA(%s)\n", lpOutputString ? lpOutputString : "(null)");
}
