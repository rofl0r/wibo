#include "ioapiset.h"

#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "overlapped_util.h"
#include "synchapi.h"

#include <pthread.h>

BOOL WINAPI GetOverlappedResult(HANDLE hFile, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred,
								  BOOL bWait) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetOverlappedResult(%p, %p, %p, %d)\n", hFile, lpOverlapped, lpNumberOfBytesTransferred, bWait);
	if (!lpOverlapped) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (bWait && lpOverlapped->Internal == STATUS_PENDING) {
		HANDLE waitHandle = kernel32_detail_normalizedOverlappedEventHandle(lpOverlapped);
		if (waitHandle) {
			WaitForSingleObject(waitHandle, INFINITE);
		} else {
			FileObject *file = (FileObject *)Handles_getWithFlag(wibo_handles(), hFile, Of_File, NULL);
			if (!file) {
				kernel32_setLastError(ERROR_INVALID_HANDLE);
				return FALSE;
			}
			pthread_mutex_lock(&file->base.m);
			while (lpOverlapped->Internal == STATUS_PENDING) {
				pthread_cond_wait(&file->overlappedCv, &file->base.m);
			}
			pthread_mutex_unlock(&file->base.m);
			ObjectBase_unref(&file->base.base);
		}
	}

	const NTSTATUS status = (NTSTATUS)lpOverlapped->Internal;
	if (status == STATUS_PENDING) {
		kernel32_setLastError(ERROR_IO_INCOMPLETE);
		return FALSE;
	}

	if (lpNumberOfBytesTransferred) {
		*lpNumberOfBytesTransferred = (DWORD)lpOverlapped->InternalHigh;
	}

	DWORD error = wibo_winErrorFromNtStatus(status);
	if (error == ERROR_SUCCESS) {
		return TRUE;
	}
	kernel32_setLastError(error);
	return FALSE;
}
