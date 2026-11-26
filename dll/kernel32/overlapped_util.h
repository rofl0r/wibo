#pragma once

#include "handles.h"
#include "internal.h"
#include "minwinbase.h"

static inline HANDLE kernel32_detail_normalizedOverlappedEventHandle(const OVERLAPPED *ov) {
	if (!ov || (ov->hEvent & 1U) != 0) {
		return NO_HANDLE;
	}
	return ov->hEvent;
}

// GetOverlappedResult checks Internal under file->base.m before waiting on overlappedCv, so publish the
// status and broadcast under that lock; otherwise a wakeup can land between its check and its wait.
static inline void kernel32_detail_signalOverlappedEvent(FileObject *file, OVERLAPPED *ov, NTSTATUS status,
														 size_t bytesTransferred) {
	if (file) {
		pthread_mutex_lock(&file->base.m);
	}
	if (ov) {
		ov->Internal = status;
		ov->InternalHigh = (ULONG_PTR)bytesTransferred;
	}
	if (file) {
		pthread_cond_broadcast(&file->overlappedCv);
		pthread_mutex_unlock(&file->base.m);
	}
	HANDLE handle = kernel32_detail_normalizedOverlappedEventHandle(ov);
	if (handle) {
		ObjectBase *obj = Handles_getAs(wibo_handles(), handle, ObjectType_Event, NULL);
		if (obj) {
			EventObject_set((EventObject *)obj);
			ObjectBase_unref(obj);
		}
	}
}

static inline void kernel32_detail_resetOverlappedEvent(OVERLAPPED *ov) {
	HANDLE handle = kernel32_detail_normalizedOverlappedEventHandle(ov);
	if (handle) {
		ObjectBase *obj = Handles_getAs(wibo_handles(), handle, ObjectType_Event, NULL);
		if (obj) {
			EventObject_reset((EventObject *)obj);
			ObjectBase_unref(obj);
		}
	}
}
