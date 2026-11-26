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
// Resolve the event before publishing: once a waiter observes Internal it may return and reuse the OVERLAPPED,
// so ov must not be read after the broadcast.
static inline void kernel32_detail_signalOverlappedEvent(FileObject *file, OVERLAPPED *ov, NTSTATUS status,
														 size_t bytesTransferred) {
	ObjectBase *eventObj = NULL;
	HANDLE handle = kernel32_detail_normalizedOverlappedEventHandle(ov);
	if (handle) {
		eventObj = Handles_getAs(wibo_handles(), handle, ObjectType_Event, NULL);
	}
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
	if (eventObj) {
		EventObject_set((EventObject *)eventObj);
		ObjectBase_unref(eventObj);
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
