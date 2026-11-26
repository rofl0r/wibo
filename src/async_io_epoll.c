#include "async_io.h"

#include "errors.h"
#include "files.h"
#include "kernel32/overlapped_util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

static const NTSTATUS kStatusCancelled = 0xC0000120;

typedef enum {
	AsyncRequestRead,
	AsyncRequestWrite,
} AsyncRequestKind;

typedef struct AsyncRequest AsyncRequest;

typedef struct {
	AsyncRequest *head;
	AsyncRequest *tail;
} RequestQueue;

struct AsyncRequest {
	AsyncRequest *next;
	AsyncRequestKind kind;
	FileObject *file; // Owns one reference.
	OVERLAPPED *overlapped;
	off_t offset;
	bool hasOffset;
	bool isPipe;
	bool updateFilePointer;
	void *readBuffer;
	const uint8_t *writeBuffer;
	size_t length;
	size_t progress;
	NTSTATUS status;
	size_t bytesTransferred;
};

typedef struct {
	int fd;
	bool registered;
	uint32_t events;
	int originalFlags;
	RequestQueue readQueue;
	RequestQueue writeQueue;
} FileState;

typedef struct {
	bool completed;
	bool requeue;
	NTSTATUS status;
	size_t bytesTransferred;
} ProcessResult;

typedef struct {
	AsyncIOBackend base;
	atomic_bool mRunning;
	atomic_uint mPending;
	int mEpollFd;
	int mEventFd;
	pthread_t mThread;
	bool mThreadStarted;
	pthread_mutex_t mMutex;
	hashmap *mFileStates; // int fd -> FileState *
	pthread_mutex_t mFileQueueMutex;
	pthread_cond_t mFileQueueCv;
	RequestQueue mFileQueue;
	bool mFileStopping;
	pthread_t *mFileWorkers;
	size_t mFileWorkerCount;
	pthread_mutex_t mCompletionMutex;
	RequestQueue mCompletions;
} EpollBackend;

static bool queueEmpty(const RequestQueue *q) { return q->head == NULL; }

static void queuePushBack(RequestQueue *q, AsyncRequest *req) {
	req->next = NULL;
	if (q->tail) {
		q->tail->next = req;
	} else {
		q->head = req;
	}
	q->tail = req;
}

static void queuePushFront(RequestQueue *q, AsyncRequest *req) {
	req->next = q->head;
	q->head = req;
	if (!q->tail) {
		q->tail = req;
	}
}

static AsyncRequest *queuePopFront(RequestQueue *q) {
	AsyncRequest *req = q->head;
	if (req) {
		q->head = req->next;
		if (!q->head) {
			q->tail = NULL;
		}
		req->next = NULL;
	}
	return req;
}

// Moves all requests from src to the back of dst.
static void queueAppend(RequestQueue *dst, RequestQueue *src) {
	if (!src->head) {
		return;
	}
	if (dst->tail) {
		dst->tail->next = src->head;
	} else {
		dst->head = src->head;
	}
	dst->tail = src->tail;
	src->head = NULL;
	src->tail = NULL;
}

static void freeRequest(AsyncRequest *req) {
	if (req->file) {
		ObjectBase_unref(&req->file->base.base);
	}
	free(req);
}

static void dropQueue(RequestQueue *q) {
	AsyncRequest *req;
	while ((req = queuePopFront(q))) {
		freeRequest(req);
	}
}

static void completeRequest(EpollBackend *self, AsyncRequest *req, NTSTATUS status, size_t bytesTransferred) {
	kernel32_detail_signalOverlappedEvent(req->file, req->overlapped, status, bytesTransferred);
	atomic_fetch_sub(&self->mPending, 1);
	freeRequest(req);
}

static void notifyWorker(const EpollBackend *self) {
	if (self->mEventFd < 0) {
		return;
	}
	uint64_t value = 1;
	ssize_t rc;
	do {
		rc = write(self->mEventFd, &value, sizeof(value));
	} while (rc == -1 && errno == EINTR);
}

static void drainEventFd(const EpollBackend *self) {
	uint64_t value;
	while (true) {
		ssize_t rc = read(self->mEventFd, &value, sizeof(value));
		if (rc == -1) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN) {
				break;
			}
		}
		if (rc == sizeof(value)) {
			continue;
		}
		break;
	}
}

static void restoreOriginalFlags(FileState *state) {
	if (state->originalFlags < 0) {
		return;
	}

	if (fcntl(state->fd, F_SETFL, state->originalFlags) != 0) {
		DEBUG_LOG("AsyncIO(epoll): restoring flags for fd %d failed: %d\n", state->fd, errno);
	}

	state->originalFlags = -1;
}

static void ensureNonBlocking(FileState *state) {
	if (state->originalFlags >= 0) {
		return;
	}

	int flags = fcntl(state->fd, F_GETFL, 0);
	if (flags < 0) {
		DEBUG_LOG("AsyncIO(epoll): fcntl(F_GETFL) failed for fd %d: %d\n", state->fd, errno);
		return;
	}

	if ((flags & O_NONBLOCK) != 0) {
		return;
	}

	state->originalFlags = flags;
	if (fcntl(state->fd, F_SETFL, flags | O_NONBLOCK) != 0) {
		DEBUG_LOG("AsyncIO(epoll): fcntl(F_SETFL) failed for fd %d: %d\n", state->fd, errno);
		state->originalFlags = -1;
	}
}

static void updateRegistrationLocked(EpollBackend *self, FileState *state) {
	uint32_t desired = 0;
	if (!queueEmpty(&state->readQueue)) {
		desired |= EPOLLIN;
	}
	if (!queueEmpty(&state->writeQueue)) {
		desired |= EPOLLOUT;
	}

	if (desired == state->events && state->registered) {
		return;
	}

	if (desired == 0) {
		if (state->registered) {
			if (epoll_ctl(self->mEpollFd, EPOLL_CTL_DEL, state->fd, NULL) != 0) {
				DEBUG_LOG("AsyncIO(epoll): epoll_ctl del fd %d failed: %d\n", state->fd, errno);
			}
			state->registered = false;
			state->events = 0;
		}
		restoreOriginalFlags(state);
		return;
	}

	struct epoll_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.events = desired;
	ev.data.ptr = state;
	int op = state->registered ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
	if (epoll_ctl(self->mEpollFd, op, state->fd, &ev) != 0) {
		DEBUG_LOG("AsyncIO(epoll): epoll_ctl op=%d fd=%d failed: %d\n", op, state->fd, errno);
		return;
	}
	state->registered = true;
	state->events = desired;
}

static FileState *findFileState(EpollBackend *self, int fd) {
	void *value = NULL;
	return hashmap_find(self->mFileStates, &fd, sizeof(fd), &value) ? value : NULL;
}

static FileState *getOrCreateFileState(EpollBackend *self, int fd) {
	FileState *state = findFileState(self, fd);
	if (!state) {
		state = xcalloc(1, sizeof(*state));
		state->fd = fd;
		state->originalFlags = -1;
		hashmap_put(self->mFileStates, &fd, sizeof(fd), state);
	}
	return state;
}

static void removeFileState(EpollBackend *self, FileState *state) {
	hashmap_remove(self->mFileStates, &state->fd, sizeof(state->fd), NULL);
	free(state);
}

static bool enqueueFileRequest(EpollBackend *self, AsyncRequest *req) {
	pthread_mutex_lock(&self->mFileQueueMutex);
	if (self->mFileStopping) {
		pthread_mutex_unlock(&self->mFileQueueMutex);
		return false;
	}
	queuePushBack(&self->mFileQueue, req);
	pthread_cond_signal(&self->mFileQueueCv);
	pthread_mutex_unlock(&self->mFileQueueMutex);
	return true;
}

// Takes ownership of req; it is released on failure.
static bool enqueueRequest(EpollBackend *self, AsyncRequest *req) {
	if (!req->file || !FsObject_valid(&req->file->base) || !atomic_load(&self->mRunning)) {
		freeRequest(req);
		return false;
	}

	if (req->isPipe) {
		pthread_mutex_lock(&self->mMutex);
		if (!atomic_load(&self->mRunning)) {
			pthread_mutex_unlock(&self->mMutex);
			freeRequest(req);
			return false;
		}
		atomic_fetch_add(&self->mPending, 1);
		FileState *state = getOrCreateFileState(self, req->file->base.fd);
		ensureNonBlocking(state);
		if (req->kind == AsyncRequestRead) {
			queuePushBack(&state->readQueue, req);
		} else {
			queuePushBack(&state->writeQueue, req);
		}
		updateRegistrationLocked(self, state);
		notifyWorker(self);
		pthread_mutex_unlock(&self->mMutex);
		return true;
	}

	atomic_fetch_add(&self->mPending, 1);
	if (enqueueFileRequest(self, req)) {
		return true;
	}
	atomic_fetch_sub(&self->mPending, 1);
	freeRequest(req);
	return false;
}

static ProcessResult tryProcessPipeRead(AsyncRequest *req) {
	ProcessResult result = {0};
	if (!req->file || !FsObject_valid(&req->file->base)) {
		result.completed = true;
		result.status = STATUS_INVALID_HANDLE;
		return result;
	}
	const int fd = req->file->base.fd;
	if (req->length == 0) {
		result.completed = true;
		result.status = STATUS_SUCCESS;
		return result;
	}

	uint8_t *buffer = req->readBuffer;
	size_t toRead = req->length;
	while (true) {
		size_t chunk = toRead < (size_t)SSIZE_MAX ? toRead : (size_t)SSIZE_MAX;
		ssize_t rc = read(fd, buffer, chunk);
		if (rc > 0) {
			result.completed = true;
			result.status = STATUS_SUCCESS;
			result.bytesTransferred = (size_t)rc;
			return result;
		}
		if (rc == 0) {
			result.completed = true;
			result.status = req->isPipe ? STATUS_PIPE_BROKEN : STATUS_END_OF_FILE;
			result.bytesTransferred = 0;
			return result;
		}
		if (rc == -1) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				result.requeue = true;
				return result;
			}
			int err = errno ? errno : EIO;
			result.completed = true;
			if (err == EPIPE || err == ECONNRESET) {
				result.status = STATUS_PIPE_BROKEN;
			} else {
				result.status = wibo_statusFromErrno(err);
				if (result.status == STATUS_SUCCESS) {
					result.status = STATUS_UNEXPECTED_IO_ERROR;
				}
			}
			result.bytesTransferred = 0;
			return result;
		}
	}
}

static ProcessResult tryProcessPipeWrite(AsyncRequest *req) {
	ProcessResult result = {0};
	if (!req->file || !FsObject_valid(&req->file->base)) {
		result.completed = true;
		result.status = STATUS_INVALID_HANDLE;
		return result;
	}

	const int fd = req->file->base.fd;
	size_t remaining = req->length - req->progress;
	const uint8_t *buffer = req->writeBuffer ? req->writeBuffer + req->progress : NULL;

	while (remaining > 0) {
		size_t chunk = remaining < (size_t)SSIZE_MAX ? remaining : (size_t)SSIZE_MAX;
		ssize_t rc = write(fd, buffer, chunk);
		if (rc > 0) {
			size_t written = (size_t)rc;
			req->progress += written;
			remaining -= written;
			buffer += written;
			if (req->hasOffset) {
				req->offset += (off_t)written;
			}
			continue;
		}
		if (rc == 0) {
			break;
		}
		if (errno == EINTR) {
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			result.requeue = true;
			return result;
		}
		int err = errno ? errno : EIO;
		result.completed = true;
		if (err == EPIPE || err == ECONNRESET) {
			result.status = STATUS_PIPE_BROKEN;
		} else {
			result.status = wibo_statusFromErrno(err);
			if (result.status == STATUS_SUCCESS) {
				result.status = STATUS_UNEXPECTED_IO_ERROR;
			}
		}
		result.bytesTransferred = req->progress;
		return result;
	}

	if (remaining == 0) {
		result.completed = true;
		result.status = STATUS_SUCCESS;
		result.bytesTransferred = req->progress;
	} else {
		result.requeue = true;
	}
	return result;
}

static void processBlockingRequest(AsyncRequest *req) {
	req->bytesTransferred = 0;
	if (!req->file || !FsObject_valid(&req->file->base)) {
		req->status = STATUS_INVALID_HANDLE;
		return;
	}

	const off_t *offset = req->hasOffset ? &req->offset : NULL;
	IOResult io;
	if (req->kind == AsyncRequestRead) {
		io = files_read(req->file, req->readBuffer, req->length, offset, req->updateFilePointer);
	} else {
		io = files_write(req->file, req->writeBuffer, req->length, offset, req->updateFilePointer);
	}

	req->bytesTransferred = io.bytesTransferred;

	if (io.unixError != 0) {
		req->status = wibo_statusFromErrno(io.unixError);
		if (req->status == STATUS_SUCCESS) {
			req->status = STATUS_UNEXPECTED_IO_ERROR;
		}
	} else if (req->kind == AsyncRequestRead && io.bytesTransferred == 0 && io.reachedEnd) {
		req->status = req->isPipe ? STATUS_PIPE_BROKEN : STATUS_END_OF_FILE;
	} else if (req->kind == AsyncRequestWrite && io.bytesTransferred == 0 && io.reachedEnd) {
		req->status = STATUS_END_OF_FILE;
	} else {
		req->status = STATUS_SUCCESS;
	}
}

static void processCompletions(EpollBackend *self) {
	RequestQueue pending = {NULL, NULL};
	pthread_mutex_lock(&self->mCompletionMutex);
	queueAppend(&pending, &self->mCompletions);
	pthread_mutex_unlock(&self->mCompletionMutex);

	AsyncRequest *req;
	while ((req = queuePopFront(&pending))) {
		completeRequest(self, req, req->status, req->bytesTransferred);
	}
}

static void handleFileEvents(EpollBackend *self, FileState *state, uint32_t events) {
	const bool canRead = (events & (EPOLLIN | EPOLLERR | EPOLLHUP)) != 0;
	const bool canWrite = (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0;

	if (canRead) {
		while (true) {
			pthread_mutex_lock(&self->mMutex);
			AsyncRequest *req = queuePopFront(&state->readQueue);
			pthread_mutex_unlock(&self->mMutex);
			if (!req) {
				break;
			}

			ProcessResult result = tryProcessPipeRead(req);
			if (result.requeue) {
				pthread_mutex_lock(&self->mMutex);
				queuePushFront(&state->readQueue, req);
				updateRegistrationLocked(self, state);
				pthread_mutex_unlock(&self->mMutex);
				break;
			}
			if (result.completed) {
				completeRequest(self, req, result.status, result.bytesTransferred);
			} else {
				freeRequest(req);
			}
			pthread_mutex_lock(&self->mMutex);
			updateRegistrationLocked(self, state);
			pthread_mutex_unlock(&self->mMutex);
		}
	}

	if (canWrite) {
		while (true) {
			pthread_mutex_lock(&self->mMutex);
			AsyncRequest *req = queuePopFront(&state->writeQueue);
			pthread_mutex_unlock(&self->mMutex);
			if (!req) {
				break;
			}

			ProcessResult result = tryProcessPipeWrite(req);
			if (result.requeue) {
				pthread_mutex_lock(&self->mMutex);
				queuePushFront(&state->writeQueue, req);
				updateRegistrationLocked(self, state);
				pthread_mutex_unlock(&self->mMutex);
				break;
			}
			if (result.completed) {
				completeRequest(self, req, result.status, result.bytesTransferred);
			} else {
				freeRequest(req);
			}
			pthread_mutex_lock(&self->mMutex);
			updateRegistrationLocked(self, state);
			pthread_mutex_unlock(&self->mMutex);
		}
	}

	pthread_mutex_lock(&self->mMutex);
	if (findFileState(self, state->fd) == state && !state->registered && queueEmpty(&state->readQueue) &&
		queueEmpty(&state->writeQueue)) {
		restoreOriginalFlags(state);
		removeFileState(self, state);
	}
	pthread_mutex_unlock(&self->mMutex);
}

static void *fileWorkerMain(void *arg) {
	EpollBackend *self = arg;
	while (true) {
		pthread_mutex_lock(&self->mFileQueueMutex);
		while (!self->mFileStopping && queueEmpty(&self->mFileQueue)) {
			pthread_cond_wait(&self->mFileQueueCv, &self->mFileQueueMutex);
		}
		if (self->mFileStopping && queueEmpty(&self->mFileQueue)) {
			pthread_mutex_unlock(&self->mFileQueueMutex);
			break;
		}
		AsyncRequest *req = queuePopFront(&self->mFileQueue);
		pthread_mutex_unlock(&self->mFileQueueMutex);

		processBlockingRequest(req);
		pthread_mutex_lock(&self->mCompletionMutex);
		queuePushBack(&self->mCompletions, req);
		pthread_mutex_unlock(&self->mCompletionMutex);
		notifyWorker(self);
	}
	return NULL;
}

static void workerMain(EpollBackend *self) {
	struct epoll_event events[128];

	while (true) {
		processCompletions(self);

		if (!atomic_load(&self->mRunning) && atomic_load(&self->mPending) == 0) {
			break;
		}

		int timeout = atomic_load(&self->mRunning) ? -1 : 10;
		int count = epoll_wait(self->mEpollFd, events, (int)(sizeof(events) / sizeof(events[0])), timeout);
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			DEBUG_LOG("AsyncIO(epoll): epoll_wait failed: %d\n", errno);
			continue;
		}
		if (count == 0) {
			continue;
		}

		for (int i = 0; i < count; ++i) {
			struct epoll_event *ev = &events[i];
			if (ev->data.fd == self->mEventFd) {
				drainEventFd(self);
				processCompletions(self);
				continue;
			}
			if (ev->data.ptr) {
				handleFileEvents(self, ev->data.ptr, ev->events);
			}
		}
	}
}

static void stopFileStateCb(const void *key, size_t keylen, void *value, void *ctx) {
	(void)key;
	(void)keylen;
	RequestQueue *pending = ctx;
	FileState *state = value;
	queueAppend(pending, &state->readQueue);
	queueAppend(pending, &state->writeQueue);
	state->registered = false;
	state->events = 0;
	restoreOriginalFlags(state);
}

static void failAllPending(EpollBackend *self) {
	processCompletions(self);

	RequestQueue pending = {NULL, NULL};
	pthread_mutex_lock(&self->mMutex);
	hashmap_foreach(self->mFileStates, stopFileStateCb, &pending);
	pthread_mutex_unlock(&self->mMutex);

	pthread_mutex_lock(&self->mFileQueueMutex);
	queueAppend(&pending, &self->mFileQueue);
	pthread_mutex_unlock(&self->mFileQueueMutex);

	RequestQueue completions = {NULL, NULL};
	pthread_mutex_lock(&self->mCompletionMutex);
	queueAppend(&completions, &self->mCompletions);
	pthread_mutex_unlock(&self->mCompletionMutex);

	AsyncRequest *req;
	while ((req = queuePopFront(&completions))) {
		completeRequest(self, req, req->status, req->bytesTransferred);
	}
	while ((req = queuePopFront(&pending))) {
		completeRequest(self, req, kStatusCancelled, 0);
	}
}

static void *workerThreadMain(void *arg) {
	EpollBackend *self = arg;
	workerMain(self);
	failAllPending(self);
	return NULL;
}

static void freeFileStateCb(const void *key, size_t keylen, void *value, void *ctx) {
	(void)key;
	(void)keylen;
	(void)ctx;
	FileState *state = value;
	restoreOriginalFlags(state);
	dropQueue(&state->readQueue);
	dropQueue(&state->writeQueue);
	free(state);
}

static void epollShutdown(AsyncIOBackend *base);

static bool epollInit(AsyncIOBackend *base) {
	EpollBackend *self = (EpollBackend *)base;
	if (atomic_load(&self->mRunning)) {
		return true;
	}

	self->mEpollFd = epoll_create1(EPOLL_CLOEXEC);
	if (self->mEpollFd < 0) {
		DEBUG_LOG("AsyncIO(epoll): epoll_create1 failed: %d\n", errno);
		return false;
	}

	self->mEventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (self->mEventFd < 0) {
		DEBUG_LOG("AsyncIO(epoll): eventfd failed: %d\n", errno);
		close(self->mEpollFd);
		self->mEpollFd = -1;
		return false;
	}

	struct epoll_event event;
	memset(&event, 0, sizeof(event));
	event.events = EPOLLIN;
	event.data.fd = self->mEventFd;
	if (epoll_ctl(self->mEpollFd, EPOLL_CTL_ADD, self->mEventFd, &event) != 0) {
		DEBUG_LOG("AsyncIO(epoll): epoll_ctl add eventfd failed: %d\n", errno);
		close(self->mEventFd);
		close(self->mEpollFd);
		self->mEventFd = -1;
		self->mEpollFd = -1;
		return false;
	}

	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	size_t workerCount = cpus > 0 ? (size_t)cpus : 1;
	if (workerCount > 4) {
		workerCount = 4;
	}

	pthread_mutex_lock(&self->mFileQueueMutex);
	self->mFileStopping = false;
	pthread_mutex_unlock(&self->mFileQueueMutex);

	self->mFileWorkers = xcalloc(workerCount, sizeof(*self->mFileWorkers));
	self->mFileWorkerCount = 0;
	atomic_store(&self->mRunning, true);
	while (self->mFileWorkerCount < workerCount) {
		if (pthread_create(&self->mFileWorkers[self->mFileWorkerCount], NULL, fileWorkerMain, self) != 0) {
			DEBUG_LOG("AsyncIO(epoll): failed to start file worker thread\n");
			epollShutdown(base);
			return false;
		}
		++self->mFileWorkerCount;
	}

	if (pthread_create(&self->mThread, NULL, workerThreadMain, self) != 0) {
		DEBUG_LOG("AsyncIO(epoll): failed to start event worker thread\n");
		epollShutdown(base);
		return false;
	}
	self->mThreadStarted = true;
	DEBUG_LOG("AsyncIO: epoll backend initialized\n");
	return true;
}

static void epollShutdown(AsyncIOBackend *base) {
	EpollBackend *self = (EpollBackend *)base;
	if (!atomic_exchange(&self->mRunning, false)) {
		return;
	}

	pthread_mutex_lock(&self->mFileQueueMutex);
	self->mFileStopping = true;
	pthread_mutex_unlock(&self->mFileQueueMutex);
	pthread_cond_broadcast(&self->mFileQueueCv);
	notifyWorker(self);

	if (self->mThreadStarted) {
		pthread_join(self->mThread, NULL);
		self->mThreadStarted = false;
	}
	for (size_t i = 0; i < self->mFileWorkerCount; ++i) {
		pthread_join(self->mFileWorkers[i], NULL);
	}
	self->mFileWorkerCount = 0;
	free(self->mFileWorkers);
	self->mFileWorkers = NULL;

	if (self->mEventFd >= 0) {
		close(self->mEventFd);
		self->mEventFd = -1;
	}
	if (self->mEpollFd >= 0) {
		close(self->mEpollFd);
		self->mEpollFd = -1;
	}

	pthread_mutex_lock(&self->mMutex);
	hashmap_foreach(self->mFileStates, freeFileStateCb, NULL);
	hashmap_free(self->mFileStates);
	self->mFileStates = hashmap_create();
	pthread_mutex_unlock(&self->mMutex);

	pthread_mutex_lock(&self->mFileQueueMutex);
	dropQueue(&self->mFileQueue);
	pthread_mutex_unlock(&self->mFileQueueMutex);

	pthread_mutex_lock(&self->mCompletionMutex);
	dropQueue(&self->mCompletions);
	pthread_mutex_unlock(&self->mCompletionMutex);

	atomic_store(&self->mPending, 0);
}

static bool epollRunning(const AsyncIOBackend *base) {
	return atomic_load(&((const EpollBackend *)base)->mRunning);
}

static bool epollQueueRead(AsyncIOBackend *base, FileObject *file, OVERLAPPED *ov, void *buffer, DWORD length,
						   const off_t *offset, bool isPipe) {
	EpollBackend *self = (EpollBackend *)base;
	AsyncRequest *req = xcalloc(1, sizeof(*req));
	req->kind = AsyncRequestRead;
	req->file = file;
	req->overlapped = ov;
	if (offset) {
		req->offset = *offset;
		req->hasOffset = true;
	}
	req->isPipe = isPipe;
	req->updateFilePointer = file ? !file->overlapped : true;
	req->readBuffer = buffer;
	req->length = length;
	return enqueueRequest(self, req);
}

static bool epollQueueWrite(AsyncIOBackend *base, FileObject *file, OVERLAPPED *ov, const void *buffer, DWORD length,
							const off_t *offset, bool isPipe) {
	EpollBackend *self = (EpollBackend *)base;
	AsyncRequest *req = xcalloc(1, sizeof(*req));
	req->kind = AsyncRequestWrite;
	req->file = file;
	req->overlapped = ov;
	if (offset) {
		req->offset = *offset;
		req->hasOffset = true;
	}
	req->isPipe = isPipe;
	req->updateFilePointer = file ? !file->overlapped : true;
	req->writeBuffer = buffer;
	req->length = length;
	return enqueueRequest(self, req);
}

static void epollDestroy(AsyncIOBackend *base) {
	EpollBackend *self = (EpollBackend *)base;
	epollShutdown(base);
	hashmap_free(self->mFileStates);
	pthread_mutex_destroy(&self->mMutex);
	pthread_mutex_destroy(&self->mFileQueueMutex);
	pthread_cond_destroy(&self->mFileQueueCv);
	pthread_mutex_destroy(&self->mCompletionMutex);
	free(self);
}

AsyncIOBackend *wibo_detail_createEpollBackend(void) {
	EpollBackend *self = xcalloc(1, sizeof(*self));
	self->base.init = epollInit;
	self->base.shutdown = epollShutdown;
	self->base.running = epollRunning;
	self->base.queueRead = epollQueueRead;
	self->base.queueWrite = epollQueueWrite;
	self->base.destroy = epollDestroy;
	self->mEpollFd = -1;
	self->mEventFd = -1;
	self->mFileStates = hashmap_create();
	pthread_mutex_init(&self->mMutex, NULL);
	pthread_mutex_init(&self->mFileQueueMutex, NULL);
	pthread_cond_init(&self->mFileQueueCv, NULL);
	pthread_mutex_init(&self->mCompletionMutex, NULL);
	return &self->base;
}
