#include "wow64apiset.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "types.h"

BOOL WINAPI Wow64DisableWow64FsRedirection(GUEST_PTR *OldValue) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: Wow64DisableWow64FsRedirection(%p)\n", OldValue);
	if (OldValue) {
		*OldValue = GUEST_NULL;
	}
	return TRUE;
}

BOOL WINAPI Wow64RevertWow64FsRedirection(PVOID OldValue) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: Wow64RevertWow64FsRedirection(%p)\n", OldValue);
	(void)OldValue;
	return TRUE;
}

BOOL WINAPI IsWow64Process(HANDLE hProcess, PBOOL Wow64Process) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsWow64Process(%p, %p)\n", hProcess, Wow64Process);
	if (!Wow64Process) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (!kernel32_isPseudoCurrentProcessHandle(hProcess)) {
		if (!hProcess) {
			kernel32_setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		ObjectBase *obj = Handles_getAs(wibo_handles(), hProcess, ObjectType_Process, NULL);
		if (!obj) {
			kernel32_setLastError(ERROR_INVALID_HANDLE);
			return FALSE;
		}
		ObjectBase_unref(obj);
	}

	*Wow64Process = FALSE;
	return TRUE;
}
