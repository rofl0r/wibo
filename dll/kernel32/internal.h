#pragma once

#include "common.h"
#include "errors.h"
#include "handles.h"
#include "types.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct Heap Heap;

typedef struct FsObject FsObject;
typedef struct FileObject FileObject;
typedef struct DirectoryObject DirectoryObject;
typedef struct ProcessObject ProcessObject;
typedef struct ThreadObject ThreadObject;
typedef struct MutexObject MutexObject;
typedef struct EventObject EventObject;
typedef struct SemaphoreObject SemaphoreObject;
typedef struct HeapObject HeapObject;

struct FsObject {
	ObjectBase base;
	pthread_mutex_t m;
	int fd;
	char *canonicalPath; // malloc'd; NULL when unknown
	uint32_t shareAccess;
	bool deletePending;
	bool closeOnDestroy;
};

static inline void FsObject_init(FsObject *self, ObjectType type, int fd, ObjectBase_DestroyFn destroy) {
	ObjectBase_init(&self->base, type, destroy);
	self->base.flags |= Of_FsObject;
	pthread_mutex_init(&self->m, NULL);
	self->fd = fd;
	self->canonicalPath = NULL;
	self->shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	self->deletePending = false;
	self->closeOnDestroy = true;
}

// Closes the fd and deletes the file if requested. Does not free `self`.
void FsObject_free(FsObject *self);

static inline bool FsObject_valid(const FsObject *self) { return self->fd >= 0; }

struct FileObject {
	FsObject base;
	off_t filePos;
	bool overlapped;
	bool appendOnly;
	bool isPipe;
	// Used to notify overlapped operations without an event handle
	pthread_cond_t overlappedCv;
};

static inline void FileObject_init(FileObject *self, ObjectType type, int fd, ObjectBase_DestroyFn destroy) {
	FsObject_init(&self->base, type, fd, destroy);
	self->base.base.flags |= Of_File;
	self->filePos = 0;
	self->overlapped = false;
	self->appendOnly = false;
	self->isPipe = false;
	pthread_cond_init(&self->overlappedCv, NULL);
	if (fd >= 0) {
		off_t pos = lseek(fd, 0, SEEK_CUR);
		if (pos == -1 && errno == ESPIPE) {
			self->isPipe = true;
		} else if (pos >= 0) {
			self->filePos = pos;
		}
	}
}

// Releases resources held by a FileObject. Does not free `self`.
static inline void FileObject_free(FileObject *self) {
	pthread_cond_destroy(&self->overlappedCv);
	FsObject_free(&self->base);
}

static inline void FileObject_destroy(ObjectBase *base) {
	FileObject *self = (FileObject *)base;
	FileObject_free(self);
	free(self);
}

static inline FileObject *FileObject_alloc(ObjectType type, int fd) {
	FileObject *self = xcalloc(1, sizeof(*self));
	FileObject_init(self, type, fd, FileObject_destroy);
	return self;
}

struct DirectoryObject {
	FsObject base;
	uint64_t enumCookie;
};

static inline void DirectoryObject_destroy(ObjectBase *base) {
	DirectoryObject *self = (DirectoryObject *)base;
	FsObject_free(&self->base);
	free(self);
}

static inline DirectoryObject *DirectoryObject_alloc(int dirfd) {
	DirectoryObject *self = xcalloc(1, sizeof(*self));
	FsObject_init(&self->base, ObjectType_Directory, dirfd, DirectoryObject_destroy);
	return self;
}

struct ProcessObject {
	WaitableObject base;
	pid_t pid;
	int pidfd;
	DWORD exitCode;
	bool forcedExitCode;
	bool waitable;
};

static inline void ProcessObject_free(ProcessObject *self) {
	if (self->pidfd != -1) {
		close(self->pidfd);
		self->pidfd = -1;
	}
	WaitableObject_free(&self->base);
}

static inline void ProcessObject_destroy(ObjectBase *base) {
	ProcessObject *self = (ProcessObject *)base;
	ProcessObject_free(self);
	free(self);
}

static inline ProcessObject *ProcessObject_alloc(pid_t pid, int pidfd, bool waitable) {
	ProcessObject *self = xcalloc(1, sizeof(*self));
	WaitableObject_init(&self->base, ObjectType_Process);
	self->base.base.destroy = ProcessObject_destroy;
	self->pid = pid;
	self->pidfd = pidfd;
	self->exitCode = STILL_ACTIVE;
	self->waitable = waitable;
	return self;
}

#define pthread_null ((pthread_t)0)

struct ThreadObject {
	WaitableObject base;
	pthread_t thread;
	DWORD exitCode;
	unsigned int suspendCount;
	TEB *tib;
};

static inline void ThreadObject_free(ThreadObject *self) {
	// Threads are detached at creation; we can safely drop
	if (self->tib) {
		wibo_destroyTib(self->tib);
		self->tib = NULL;
	}
	WaitableObject_free(&self->base);
}

static inline void ThreadObject_destroy(ObjectBase *base) {
	ThreadObject *self = (ThreadObject *)base;
	ThreadObject_free(self);
	free(self);
}

static inline ThreadObject *ThreadObject_alloc(pthread_t thread) {
	ThreadObject *self = xcalloc(1, sizeof(*self));
	WaitableObject_init(&self->base, ObjectType_Thread);
	self->base.base.destroy = ThreadObject_destroy;
	self->thread = thread;
	self->exitCode = STILL_ACTIVE;
	return self;
}

struct MutexObject {
	WaitableObject base;
	bool ownerValid;
	pthread_t owner;
	unsigned int recursionCount;
	bool abandoned; // Owner exited without releasing
};

static inline void MutexObject_destroy(ObjectBase *base) {
	MutexObject *self = (MutexObject *)base;
	WaitableObject_free(&self->base);
	free(self);
}

static inline MutexObject *MutexObject_alloc(void) {
	MutexObject *self = xcalloc(1, sizeof(*self));
	WaitableObject_init(&self->base, ObjectType_Mutex);
	self->base.base.destroy = MutexObject_destroy;
	self->base.signaled = true;
	return self;
}

struct EventObject {
	WaitableObject base;
	bool manualReset;
};

static inline void EventObject_destroy(ObjectBase *base) {
	EventObject *self = (EventObject *)base;
	WaitableObject_free(&self->base);
	free(self);
}

static inline EventObject *EventObject_alloc(bool manual) {
	EventObject *self = xcalloc(1, sizeof(*self));
	WaitableObject_init(&self->base, ObjectType_Event);
	self->base.base.destroy = EventObject_destroy;
	self->manualReset = manual;
	return self;
}

static inline void EventObject_set(EventObject *self) {
	pthread_mutex_lock(&self->base.m);
	self->base.signaled = true;
	pthread_mutex_unlock(&self->base.m);
	// Broadcast even for auto-reset events: a signal can be absorbed by a waiter that times out concurrently,
	// which would leave another waiter asleep on a signaled event. Each waiter re-checks the predicate.
	pthread_cond_broadcast(&self->base.cv);
	WaitableObject_notifyWaiters(&self->base, false);
}

static inline void EventObject_reset(EventObject *self) {
	pthread_mutex_lock(&self->base.m);
	self->base.signaled = false;
	pthread_mutex_unlock(&self->base.m);
}

struct SemaphoreObject {
	WaitableObject base;
	LONG count;
	LONG maxCount;
};

static inline void SemaphoreObject_destroy(ObjectBase *base) {
	SemaphoreObject *self = (SemaphoreObject *)base;
	WaitableObject_free(&self->base);
	free(self);
}

static inline SemaphoreObject *SemaphoreObject_alloc(LONG initial, LONG maximum) {
	SemaphoreObject *self = xcalloc(1, sizeof(*self));
	WaitableObject_init(&self->base, ObjectType_Semaphore);
	self->base.base.destroy = SemaphoreObject_destroy;
	self->count = initial;
	self->maxCount = maximum;
	return self;
}

struct HeapObject {
	ObjectBase base;
	Heap *heap; // NULL when no heap is attached
	pthread_t owner;
	DWORD createFlags;
	SIZE_T initialSize;
	SIZE_T maximumSize;
	DWORD compatibility;
	bool isProcessHeap;
};

// Releases the heap and process-heap bookkeeping. Implemented in heapapi.c. Does not free `self`.
void HeapObject_free(HeapObject *self);

static inline void HeapObject_destroy(ObjectBase *base) {
	HeapObject *self = (HeapObject *)base;
	HeapObject_free(self);
	free(self);
}

// Takes ownership of `heap`, which may be NULL.
static inline HeapObject *HeapObject_alloc(Heap *heap) {
	HeapObject *self = xcalloc(1, sizeof(*self));
	ObjectBase_init(&self->base, ObjectType_Heap, HeapObject_destroy);
	self->heap = heap;
	self->owner = pthread_self();
	return self;
}

static inline bool HeapObject_isOwner(const HeapObject *self) { return pthread_equal(self->owner, pthread_self()); }

static inline bool HeapObject_canAccess(const HeapObject *self) {
	return self->isProcessHeap || (HeapObject_isOwner(self) && self->heap != NULL);
}

#define kPseudoCurrentProcessHandleValue ((HANDLE)-1)
#define kPseudoCurrentThreadHandleValue ((HANDLE)-2)

static inline bool kernel32_isPseudoCurrentProcessHandle(HANDLE h) {
	return h == kPseudoCurrentProcessHandleValue;
}

static inline bool kernel32_isPseudoCurrentThreadHandle(HANDLE h) { return h == kPseudoCurrentThreadHandleValue; }

// Implemented in winbase.c.
void kernel32_tryMarkExecutable(void *mem);

// Implemented in errhandlingapi.c.
DWORD kernel32_getLastError(void);
void kernel32_setLastError(DWORD error);
void kernel32_setLastErrorFromErrno(void);
// Turns host faults in guest code into unhandled Windows exceptions. Call once, on the main thread.
void kernel32_installFaultHandlers(void);

_Noreturn static inline void kernel32_exitInternal(DWORD exitCode) {
	DEBUG_LOG("exitInternal(%u)\n", exitCode);
	Handles_clear(wibo_handles());
	_exit((int)exitCode);
}
