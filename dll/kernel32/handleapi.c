#include "handleapi.h"

#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"

#include <pthread.h>
#include <unistd.h>

static bool validateProcessHandle(HANDLE handle) {
	if (kernel32_isPseudoCurrentProcessHandle(handle)) {
		return true;
	}
	ObjectBase *obj = Handles_getAs(wibo_handles(), handle, ObjectType_Process, NULL);
	if (!obj) {
		return false;
	}
	bool valid = ((ProcessObject *)obj)->pid == getpid();
	ObjectBase_unref(obj);
	return valid;
}

BOOL WINAPI DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
							LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DuplicateHandle(%p, %p, %p, %p, %x, %d, %x)\n", hSourceProcessHandle, hSourceHandle,
			  hTargetProcessHandle, lpTargetHandle, dwDesiredAccess, bInheritHandle, dwOptions);
	(void)dwDesiredAccess;
	(void)dwOptions;
	if (!lpTargetHandle) {
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (!validateProcessHandle(hSourceProcessHandle) || !validateProcessHandle(hTargetProcessHandle)) {
		DEBUG_LOG("DuplicateHandle: unsupported process handle combination (source=%p target=%p)\n",
				  hSourceProcessHandle, hTargetProcessHandle);
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	Handles *handles = wibo_handles();
	if (kernel32_isPseudoCurrentProcessHandle(hSourceHandle)) {
		ProcessObject *po = ProcessObject_alloc(getpid(), -1, false);
		HANDLE handle = Handles_alloc(handles, &po->base.base, 0, 0);
		DEBUG_LOG("DuplicateHandle: created process handle for current process -> %p\n", handle);
		*lpTargetHandle = handle;
		return TRUE;
	} else if (kernel32_isPseudoCurrentThreadHandle(hSourceHandle)) {
		ThreadObject *th = ThreadObject_alloc(pthread_self());
		HANDLE handle = Handles_alloc(handles, &th->base.base, 0, 0);
		DEBUG_LOG("DuplicateHandle: created thread handle for current thread -> %p\n", handle);
		*lpTargetHandle = handle;
		return TRUE;
	}

	if (!Handles_duplicateTo(handles, hSourceHandle, handles, lpTargetHandle, dwDesiredAccess, bInheritHandle,
							 dwOptions)) {
		DEBUG_LOG("-> ERROR_INVALID_HANDLE\n");
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	DEBUG_LOG("-> %p\n", *lpTargetHandle);
	return TRUE;
}

BOOL WINAPI CloseHandle(HANDLE hObject) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CloseHandle(%p)\n", hObject);
	if (!Handles_release(wibo_handles(), hObject)) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetHandleInformation(%p, 0x%x, 0x%x)\n", hObject, dwMask, dwFlags);
	if (!Handles_setInformation(wibo_handles(), hObject, dwMask, dwFlags)) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}
