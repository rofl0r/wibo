#include "async_io.h"

#include "errors.h"
#include "kernel32/overlapped_util.h"

#include <errno.h>
#include <liburing.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/uio.h>

static const unsigned kQueueDepth = 64;

typedef enum {
	AsyncRequestRead,
	AsyncRequestWrite,
	AsyncRequestShutdown,
} AsyncRequestKind;

typedef struct {
	AsyncRequestKind kind;
	FileObject *file; // Owns one reference when non-NULL.
	OVERLAPPED *overlapped;
	bool isPipe;
	struct iovec vec;
} AsyncRequest;

typedef struct {
	AsyncIOBackend base;
	struct io_uring mRing;
	pthread_mutex_t mSubmitMutex;
	pthread_cond_t mQueueCv;
	atomic_bool mRunning;
	atomic_uint mPending;
	pthread_t mThread;
	bool mThreadStarted;
} IoUringBackend;

static void freeRequest(AsyncRequest *req) {
	if (req->file) {
		ObjectBase_unref(&req->file->base.base);
	}
	free(req);
}

static void notifySpace(IoUringBackend *self) {
	pthread_mutex_lock(&self->mSubmitMutex);
	pthread_cond_broadcast(&self->mQueueCv);
	pthread_mutex_unlock(&self->mSubmitMutex);
}

static bool enqueueRequest(IoUringBackend *self, AsyncRequest *req, void *buffer, DWORD length, const off_t *offset,
						   bool isWrite) {
	pthread_mutex_lock(&self->mSubmitMutex);
	if (!atomic_load(&self->mRunning) && req->kind != AsyncRequestShutdown) {
		pthread_mutex_unlock(&self->mSubmitMutex);
		return false;
	}

	struct io_uring_sqe *sqe;
	while (true) {
		sqe = io_uring_get_sqe(&self->mRing);
		if (!sqe) {
			pthread_cond_wait(&self->mQueueCv, &self->mSubmitMutex);
			if (!atomic_load(&self->mRunning) && req->kind != AsyncRequestShutdown) {
				pthread_mutex_unlock(&self->mSubmitMutex);
				return false;
			}
			continue;
		}
		io_uring_sqe_set_data(sqe, req);
		if (req->kind == AsyncRequestShutdown) {
			io_uring_prep_nop(sqe);
		} else {
			req->vec.iov_base = buffer;
			req->vec.iov_len = length;
			off_t fileOffset = -1;
			if (!req->isPipe && offset) {
				fileOffset = *offset;
			}
			int fd = req->file ? req->file->base.fd : -1;
			if (isWrite) {
				io_uring_prep_writev(sqe, fd, &req->vec, 1, fileOffset);
			} else {
				io_uring_prep_readv(sqe, fd, &req->vec, 1, fileOffset);
			}
		}
		atomic_fetch_add_explicit(&self->mPending, 1, memory_order_relaxed);
		break;
	}

	while (true) {
		int res = io_uring_submit(&self->mRing);
		if (res >= 0) {
			break;
		} else if (res == -EINTR) {
			continue;
		} else if (res == -EBUSY || res == -EAGAIN) {
			pthread_mutex_unlock(&self->mSubmitMutex);
			sched_yield();
			pthread_mutex_lock(&self->mSubmitMutex);
			continue;
		}
		DEBUG_LOG("io_uring_submit failed (will retry): %d\n", res);
	}

	pthread_mutex_unlock(&self->mSubmitMutex);
	pthread_cond_signal(&self->mQueueCv);
	return true;
}

static void requestStop(IoUringBackend *self) {
	atomic_store(&self->mRunning, false);
	AsyncRequest *req = xcalloc(1, sizeof(*req));
	req->kind = AsyncRequestShutdown;
	if (!enqueueRequest(self, req, NULL, 0, NULL, false)) {
		free(req);
	}
}

static void handleCompletion(IoUringBackend *self, struct io_uring_cqe *cqe) {
	AsyncRequest *req = io_uring_cqe_get_data(cqe);
	if (!req) {
		return;
	}

	if (req->kind == AsyncRequestShutdown) {
		free(req);
		atomic_fetch_sub_explicit(&self->mPending, 1, memory_order_acq_rel);
		return;
	}

	NTSTATUS completionStatus = STATUS_SUCCESS;
	size_t bytesTransferred = 0;
	if (cqe->res >= 0) {
		bytesTransferred = (size_t)cqe->res;
		if (req->kind == AsyncRequestRead && cqe->res == 0) {
			completionStatus = req->isPipe ? STATUS_PIPE_BROKEN : STATUS_END_OF_FILE;
		}
	} else {
		int err = -cqe->res;
		if (err == EPIPE) {
			completionStatus = STATUS_PIPE_BROKEN;
		} else {
			completionStatus = wibo_statusFromErrno(err);
			if (completionStatus == STATUS_SUCCESS) {
				completionStatus = STATUS_UNEXPECTED_IO_ERROR;
			}
		}
	}
	kernel32_detail_signalOverlappedEvent(req->file, req->overlapped, completionStatus, bytesTransferred);

	freeRequest(req);
	atomic_fetch_sub_explicit(&self->mPending, 1, memory_order_acq_rel);
}

static void *workerLoop(void *arg) {
	IoUringBackend *self = arg;
	while (atomic_load(&self->mRunning) || atomic_load(&self->mPending) > 0) {
		struct io_uring_cqe *cqe = NULL;
		int ret = io_uring_wait_cqe(&self->mRing, &cqe);
		if (ret == -EINTR) {
			continue;
		}
		if (ret < 0) {
			DEBUG_LOG("io_uring_wait_cqe failed: %d\n", ret);
			continue;
		}
		handleCompletion(self, cqe);
		io_uring_cqe_seen(&self->mRing, cqe);
		notifySpace(self);
	}

	while (atomic_load(&self->mPending) > 0) {
		struct io_uring_cqe *cqe = NULL;
		int ret = io_uring_peek_cqe(&self->mRing, &cqe);
		if (ret != 0 || !cqe) {
			break;
		}
		handleCompletion(self, cqe);
		io_uring_cqe_seen(&self->mRing, cqe);
		notifySpace(self);
	}
	return NULL;
}

static bool uringInit(AsyncIOBackend *base) {
	IoUringBackend *self = (IoUringBackend *)base;
	if (atomic_load(&self->mRunning)) {
		return true;
	}
	int rc = io_uring_queue_init(kQueueDepth, &self->mRing, 0);
	if (rc < 0) {
		DEBUG_LOG("io_uring_queue_init failed: %d\n", rc);
		return false;
	}
	atomic_store(&self->mRunning, true);
	if (pthread_create(&self->mThread, NULL, workerLoop, self) != 0) {
		DEBUG_LOG("io_uring: failed to start worker thread\n");
		atomic_store(&self->mRunning, false);
		io_uring_queue_exit(&self->mRing);
		return false;
	}
	self->mThreadStarted = true;
	DEBUG_LOG("io_uring backend initialized (depth=%u)\n", kQueueDepth);
	return true;
}

static void uringShutdown(AsyncIOBackend *base) {
	IoUringBackend *self = (IoUringBackend *)base;
	if (!atomic_exchange(&self->mRunning, false)) {
		return;
	}
	requestStop(self);
	if (self->mThreadStarted) {
		pthread_join(self->mThread, NULL);
		self->mThreadStarted = false;
	}
	io_uring_queue_exit(&self->mRing);
}

static bool uringRunning(const AsyncIOBackend *base) {
	const IoUringBackend *self = (const IoUringBackend *)base;
	return atomic_load(&self->mRunning);
}

static bool uringQueueRead(AsyncIOBackend *base, FileObject *file, OVERLAPPED *ov, void *buffer, DWORD length,
						   const off_t *offset, bool isPipe) {
	IoUringBackend *self = (IoUringBackend *)base;
	AsyncRequest *req = xcalloc(1, sizeof(*req));
	req->kind = AsyncRequestRead;
	req->file = file;
	req->overlapped = ov;
	req->isPipe = isPipe;
	if (!enqueueRequest(self, req, buffer, length, offset, false)) {
		freeRequest(req);
		return false;
	}
	return true;
}

static bool uringQueueWrite(AsyncIOBackend *base, FileObject *file, OVERLAPPED *ov, const void *buffer, DWORD length,
							const off_t *offset, bool isPipe) {
	IoUringBackend *self = (IoUringBackend *)base;
	AsyncRequest *req = xcalloc(1, sizeof(*req));
	req->kind = AsyncRequestWrite;
	req->file = file;
	req->overlapped = ov;
	req->isPipe = isPipe;
	if (!enqueueRequest(self, req, (void *)buffer, length, offset, true)) {
		freeRequest(req);
		return false;
	}
	return true;
}

static void uringDestroy(AsyncIOBackend *base) {
	IoUringBackend *self = (IoUringBackend *)base;
	uringShutdown(base);
	pthread_mutex_destroy(&self->mSubmitMutex);
	pthread_cond_destroy(&self->mQueueCv);
	free(self);
}

AsyncIOBackend *wibo_detail_createIoUringBackend(void) {
	IoUringBackend *self = xcalloc(1, sizeof(*self));
	self->base.init = uringInit;
	self->base.shutdown = uringShutdown;
	self->base.running = uringRunning;
	self->base.queueRead = uringQueueRead;
	self->base.queueWrite = uringQueueWrite;
	self->base.destroy = uringDestroy;
	pthread_mutex_init(&self->mSubmitMutex, NULL);
	pthread_cond_init(&self->mQueueCv, NULL);
	return &self->base;
}
