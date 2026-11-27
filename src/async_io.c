#include "async_io.h"

#include "common.h"
#include "cutil.h"

#include <stdlib.h>

static AsyncIOBackend *g_backend;

static bool noOpInit(AsyncIOBackend *self) {
	(void)self;
	return true;
}

static void noOpShutdown(AsyncIOBackend *self) { (void)self; }

static bool noOpRunning(const AsyncIOBackend *self) {
	(void)self;
	return true;
}

static bool noOpQueueRead(AsyncIOBackend *self, FileObject *file, OVERLAPPED *ov, void *buffer, DWORD length,
						  const off_t *offset, bool isPipe) {
	(void)self;
	(void)ov;
	(void)buffer;
	(void)length;
	(void)offset;
	(void)isPipe;
	ObjectBase_unref(&file->base.base);
	return false; // Force synchronous fallback
}

static bool noOpQueueWrite(AsyncIOBackend *self, FileObject *file, OVERLAPPED *ov, const void *buffer, DWORD length,
						   const off_t *offset, bool isPipe) {
	(void)self;
	(void)ov;
	(void)buffer;
	(void)length;
	(void)offset;
	(void)isPipe;
	ObjectBase_unref(&file->base.base);
	return false; // Force synchronous fallback
}

static void noOpDestroy(AsyncIOBackend *self) { free(self); }

static AsyncIOBackend *createNoOpBackend(void) {
	AsyncIOBackend *backend = xcalloc(1, sizeof *backend);
	backend->init = noOpInit;
	backend->shutdown = noOpShutdown;
	backend->running = noOpRunning;
	backend->queueRead = noOpQueueRead;
	backend->queueWrite = noOpQueueWrite;
	backend->destroy = noOpDestroy;
	return backend;
}

typedef struct {
	const char *name;
	AsyncIOBackend *(*factory)(void);
} BackendEntry;

static const BackendEntry kBackends[] = {
#if WIBO_ENABLE_LIBURING
	{"io_uring", wibo_detail_createIoUringBackend},
#endif
#ifdef __linux__
	{"epoll", wibo_detail_createEpollBackend},
#endif
#ifdef __APPLE__
	{"kqueue", wibo_detail_createKqueueBackend},
#endif
};

AsyncIOBackend *wibo_asyncIO(void) {
	if (!g_backend) {
		for (size_t i = 0; i < sizeof kBackends / sizeof kBackends[0]; ++i) {
			const BackendEntry *entry = &kBackends[i];
			DEBUG_LOG("AsyncIO: initializing %s backend\n", entry->name);
			AsyncIOBackend *backend = entry->factory();
			if (backend && backend->init(backend)) {
				g_backend = backend;
				break;
			}
			DEBUG_LOG("AsyncIO: %s backend unavailable\n", entry->name);
			if (backend) {
				backend->shutdown(backend);
				backend->destroy(backend);
			}
		}
	}

	if (!g_backend) {
		DEBUG_LOG("AsyncIO: no backend available; using no-op backend\n");
		g_backend = createNoOpBackend();
		g_backend->init(g_backend);
	}

	return g_backend;
}
