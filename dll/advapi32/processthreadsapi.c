#include "processthreadsapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "kernel32/internal.h"
#include "processes.h"

BOOL WINAPI OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("OpenProcessToken(%p, %u, %p)\n", ProcessHandle, DesiredAccess, TokenHandle);
	if (!TokenHandle) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ObjectBase *obj;
	if (kernel32_isPseudoCurrentProcessHandle(ProcessHandle)) {
		obj = &ProcessObject_alloc(getpid(), -1, false)->base.base;
	} else {
		obj = Handles_getAs(wibo_handles(), ProcessHandle, ObjectType_Process, NULL);
	}
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	TokenObject *token = TokenObject_alloc(obj, DesiredAccess);
	*TokenHandle = Handles_alloc(wibo_handles(), &token->base, 0, 0);
	return TRUE;
}
