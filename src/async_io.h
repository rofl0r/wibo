#pragma once

#include "kernel32/internal.h"
#include "kernel32/minwinbase.h"

#include <stdbool.h>
#include <sys/types.h>

typedef struct AsyncIOBackend AsyncIOBackend;

// Backends embed this as their first member `base` and receive it as `self`.
struct AsyncIOBackend {
	bool (*init)(AsyncIOBackend *self);
	void (*shutdown)(AsyncIOBackend *self);
	bool (*running)(const AsyncIOBackend *self);
	// Takes ownership of one reference to file. offset is NULL to use the current file position.
	bool (*queueRead)(AsyncIOBackend *self, FileObject *file, OVERLAPPED *ov, void *buffer, DWORD length,
					  const off_t *offset, bool isPipe);
	bool (*queueWrite)(AsyncIOBackend *self, FileObject *file, OVERLAPPED *ov, const void *buffer, DWORD length,
					   const off_t *offset, bool isPipe);
	void (*destroy)(AsyncIOBackend *self);
};

#if WIBO_ENABLE_LIBURING
AsyncIOBackend *wibo_detail_createIoUringBackend(void);
#endif
#ifdef __linux__
AsyncIOBackend *wibo_detail_createEpollBackend(void);
#endif
#ifdef __APPLE__
AsyncIOBackend *wibo_detail_createKqueueBackend(void);
#endif

AsyncIOBackend *wibo_asyncIO(void);
