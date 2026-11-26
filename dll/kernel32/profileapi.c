#include "profileapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("STUB: QueryPerformanceCounter(%p)\n", lpPerformanceCount);
	if (!lpPerformanceCount) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	lpPerformanceCount->QuadPart = 0;
	return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *lpFrequency) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("STUB: QueryPerformanceFrequency(%p)\n", lpFrequency);
	if (!lpFrequency) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	lpFrequency->QuadPart = 1;
	return TRUE;
}
