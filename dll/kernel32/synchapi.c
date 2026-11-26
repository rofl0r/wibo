#include "synchapi.h"

#include "common.h"
#include "context.h"
#include "cutil.h"
#include "errors.h"
#include "handles.h"
#include "heap.h"
#include "interlockedapi.h"
#include "internal.h"
#include "kernel32.h"
#include "kernel32_trampolines.h"
#include "processthreadsapi.h"
#include "strutil.h"
#include "types.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum {
	kSrwLockExclusive = 0x1,
	kSrwLockSharedIncrement = 0x2,
	kInitOnceStateMask = 0x3,
	kInitOnceStateSyncRunning = 0x1,
	kInitOnceStateCompleted = 0x2,
	kInitOnceStateAsyncPending = 0x3,
	kAddressBucketCount = 64,
};

static const GUEST_PTR kInitOnceReservedMask = (1u << INIT_ONCE_CTX_RESERVED_BITS) - 1;

typedef struct {
	pthread_mutex_t m;
	pthread_cond_t cv;
} AddressBucket;

// Addresses hash into a fixed set of buckets. Waiters re-check their address after every wakeup, so sharing a
// bucket only costs spurious wakeups.
static AddressBucket g_addressBuckets[kAddressBucketCount];
static pthread_once_t g_addressBucketsOnce = PTHREAD_ONCE_INIT;

static void initAddressBuckets(void) {
	for (size_t i = 0; i < kAddressBucketCount; ++i) {
		pthread_mutex_init(&g_addressBuckets[i].m, NULL);
		pthread_cond_init(&g_addressBuckets[i].cv, NULL);
	}
}

static AddressBucket *addressBucket(const volatile void *address) {
	pthread_once(&g_addressBucketsOnce, initAddressBuckets);
	return &g_addressBuckets[((uintptr_t)address >> 2) % kAddressBucketCount];
}

static bool isAddressWaitSize(size_t size) { return size == 1 || size == 2 || size == 4 || size == 8; }

static uint64_t loadAddressValue(const volatile void *address, size_t size) {
	switch (size) {
	case 1:
		return __atomic_load_n((volatile uint8_t *)address, __ATOMIC_ACQUIRE);
	case 2:
		return __atomic_load_n((volatile uint16_t *)address, __ATOMIC_ACQUIRE);
	case 4:
		return __atomic_load_n((volatile uint32_t *)address, __ATOMIC_ACQUIRE);
	default:
		return __atomic_load_n((volatile uint64_t *)address, __ATOMIC_ACQUIRE);
	}
}

// Buckets are shared between addresses, so a single wake broadcasts as well.
static void wakeAddressWaiters(const volatile void *address) {
	AddressBucket *bucket = addressBucket(address);
	pthread_mutex_lock(&bucket->m);
	pthread_cond_broadcast(&bucket->cv);
	pthread_mutex_unlock(&bucket->m);
}

typedef struct {
	bool infinite;
	struct timespec deadline;
} WaitDeadline;

static void WaitDeadline_init(WaitDeadline *d, DWORD ms) {
	d->infinite = ms == INFINITE;
	if (d->infinite) {
		return;
	}
	clock_gettime(CLOCK_REALTIME, &d->deadline);
	d->deadline.tv_sec += ms / 1000;
	d->deadline.tv_nsec += (long)(ms % 1000) * 1000000L;
	if (d->deadline.tv_nsec >= 1000000000L) {
		d->deadline.tv_sec += 1;
		d->deadline.tv_nsec -= 1000000000L;
	}
}

// Caller holds m. Returns 0 after a wakeup, or ETIMEDOUT once the deadline has passed.
static int WaitDeadline_wait(const WaitDeadline *d, pthread_cond_t *cv, pthread_mutex_t *m) {
	return d->infinite ? pthread_cond_wait(cv, m) : pthread_cond_timedwait(cv, m, &d->deadline);
}

typedef struct {
	bool waitAll;
	DWORD count;
	bool *satisfied;
	bool completed;
	DWORD result;
	pthread_mutex_t m;
	pthread_cond_t cv;
} WaitBlock;

static void WaitBlock_init(WaitBlock *b, bool waitAll, DWORD count) {
	b->waitAll = waitAll;
	b->count = count;
	b->satisfied = xcalloc(count, sizeof(bool));
	b->completed = false;
	b->result = WAIT_TIMEOUT;
	pthread_mutex_init(&b->m, NULL);
	pthread_cond_init(&b->cv, NULL);
}

static void WaitBlock_free(WaitBlock *b) {
	pthread_cond_destroy(&b->cv);
	pthread_mutex_destroy(&b->m);
	free(b->satisfied);
}

static void WaitBlock_signal(WaitBlock *b, DWORD index, bool abandoned) {
	if (index >= b->count) {
		return;
	}
	bool completedNow = false;
	pthread_mutex_lock(&b->m);
	if (!b->completed && !b->satisfied[index]) {
		b->satisfied[index] = true;
		if (abandoned) {
			b->result = WAIT_ABANDONED + index;
			completedNow = true;
		} else if (!b->waitAll) {
			b->result = WAIT_OBJECT_0 + index;
			completedNow = true;
		} else {
			bool all = true;
			for (DWORD i = 0; i < b->count && all; ++i) {
				all = b->satisfied[i];
			}
			if (all) {
				b->result = WAIT_OBJECT_0;
				completedNow = true;
			}
		}
		if (completedNow) {
			b->completed = true;
			// Broadcast under the lock: once the waiter sees completed it may return and destroy b->cv.
			pthread_cond_broadcast(&b->cv);
		}
	}
	pthread_mutex_unlock(&b->m);
}

static void WaitBlock_onSignal(void *context, WaitableObject *obj, DWORD index, bool abandoned) {
	(void)obj;
	WaitBlock_signal(context, index, abandoned);
}

// Returns the completed result, or WAIT_TIMEOUT if the deadline passes first.
static DWORD WaitBlock_wait(WaitBlock *b, const WaitDeadline *deadline) {
	pthread_mutex_lock(&b->m);
	int rc = 0;
	while (!b->completed && rc != ETIMEDOUT) {
		rc = WaitDeadline_wait(deadline, &b->cv, &b->m);
	}
	DWORD result = b->completed ? b->result : WAIT_TIMEOUT;
	pthread_mutex_unlock(&b->m);
	return result;
}

// Caller holds the object's mutex.
static bool isReadyLocked(ObjectBase *obj) {
	WaitableObject *w = (WaitableObject *)obj;
	switch (obj->type) {
	case ObjectType_Semaphore:
		return ((SemaphoreObject *)obj)->count > 0;
	case ObjectType_Mutex: {
		MutexObject *mu = (MutexObject *)obj;
		return !mu->ownerValid || mu->abandoned || pthread_equal(mu->owner, pthread_self());
	}
	default:
		return w->signaled;
	}
}

// Reports whether a wait on obj would be satisfied right now. *abandoned is set for an abandoned mutex.
static bool isObjectAvailable(ObjectBase *obj, bool *abandoned) {
	WaitableObject *w = (WaitableObject *)obj;
	pthread_mutex_lock(&w->m);
	bool ready = isReadyLocked(obj);
	*abandoned = obj->type == ObjectType_Mutex && ((MutexObject *)obj)->abandoned;
	pthread_mutex_unlock(&w->m);
	return ready;
}

static DWORD owningThreadId(LPCRITICAL_SECTION cs) {
	return (DWORD)__atomic_load_n(&cs->OwningThread, __ATOMIC_ACQUIRE);
}

static void setOwningThread(LPCRITICAL_SECTION cs, DWORD threadId) {
	__atomic_store_n(&cs->OwningThread, (HANDLE)threadId, __ATOMIC_RELEASE);
}

// Blocks while the first `size` bytes at address equal compare. Returns TRUE once they differ. Returns FALSE with
// ERROR_TIMEOUT if ms elapses first.
static BOOL waitWhileEqual(const volatile void *address, const void *compare, size_t size, DWORD ms) {
	uint64_t expected = 0;
	memcpy(&expected, compare, size);
	AddressBucket *bucket = addressBucket(address);
	WaitDeadline deadline;
	WaitDeadline_init(&deadline, ms);
	pthread_mutex_lock(&bucket->m);
	int rc = 0;
	while (loadAddressValue(address, size) == expected) {
		if (rc == ETIMEDOUT) {
			pthread_mutex_unlock(&bucket->m);
			kernel32_setLastError(ERROR_TIMEOUT);
			return FALSE;
		}
		rc = WaitDeadline_wait(&deadline, &bucket->cv, &bucket->m);
	}
	pthread_mutex_unlock(&bucket->m);
	return TRUE;
}

static void waitForCriticalSection(LPCRITICAL_SECTION cs) {
	LONG volatile *tickets = (LONG volatile *)&cs->LockSemaphore;
	for (;;) {
		LONG available = __atomic_load_n(tickets, __ATOMIC_ACQUIRE);
		if (available > 0) {
			if (__atomic_compare_exchange_n(tickets, &available, available - 1, false, __ATOMIC_ACQ_REL,
											__ATOMIC_ACQUIRE)) {
				return;
			}
			continue;
		}
		waitWhileEqual(tickets, &available, sizeof(available), INFINITE);
	}
}

static void signalCriticalSection(LPCRITICAL_SECTION cs) {
	LONG volatile *tickets = (LONG volatile *)&cs->LockSemaphore;
	InterlockedIncrement(tickets);
	wakeAddressWaiters(tickets);
}

static bool trySpinAcquireCriticalSection(LPCRITICAL_SECTION cs, DWORD threadId) {
	if (!cs || cs->SpinCount == 0) {
		return false;
	}
	for (ULONG_PTR spins = cs->SpinCount; spins > 0; --spins) {
		if (TryEnterCriticalSection(cs)) {
			return true;
		}
		if (cs->LockCount > 0) {
			break;
		}
		sched_yield();
		if (owningThreadId(cs) == threadId) {
			// Owner is self, TryEnter would have succeeded; bail out.
			break;
		}
	}
	return false;
}

// Caller holds w->m and has checked that obj is ready. Takes the wait's ownership of the object.
static DWORD acquireReadyLocked(ObjectBase *obj) {
	WaitableObject *w = (WaitableObject *)obj;
	switch (obj->type) {
	case ObjectType_Event:
		if (!((EventObject *)obj)->manualReset) {
			w->signaled = false;
		}
		return WAIT_OBJECT_0;
	case ObjectType_Semaphore:
		--((SemaphoreObject *)obj)->count;
		return WAIT_OBJECT_0;
	case ObjectType_Mutex: {
		MutexObject *mu = (MutexObject *)obj;
		pthread_t self = pthread_self();
		if (mu->ownerValid && pthread_equal(mu->owner, self)) {
			++mu->recursionCount;
			return WAIT_OBJECT_0;
		}
		DWORD result = WAIT_OBJECT_0;
		if (mu->abandoned) {
			mu->abandoned = false;
			result = WAIT_ABANDONED;
		}
		mu->owner = self;
		mu->ownerValid = true;
		mu->recursionCount = 1;
		w->signaled = false;
		return result;
	}
	default:
		return WAIT_OBJECT_0;
	}
}

// Waits until obj is ready, then acquires it. Returns WAIT_TIMEOUT if ms elapses first.
static DWORD waitOnObject(ObjectBase *obj, DWORD ms) {
	WaitableObject *w = (WaitableObject *)obj;
	WaitDeadline deadline;
	WaitDeadline_init(&deadline, ms);
	pthread_mutex_lock(&w->m);
	if (obj->type == ObjectType_Thread && pthread_equal(((ThreadObject *)obj)->thread, pthread_self())) {
		// Windows actually allows you to wait on your own thread, but why bother?
		pthread_mutex_unlock(&w->m);
		return WAIT_TIMEOUT;
	}
	if (obj->type == ObjectType_Process && !w->signaled && !((ProcessObject *)obj)->waitable) {
		// Windows actually allows you to wait on your own process, but why bother?
		pthread_mutex_unlock(&w->m);
		return WAIT_TIMEOUT;
	}
	int rc = 0;
	while (!isReadyLocked(obj)) {
		if (rc == ETIMEDOUT) {
			pthread_mutex_unlock(&w->m);
			return WAIT_TIMEOUT;
		}
		rc = WaitDeadline_wait(&deadline, &w->cv, &w->m);
	}
	DWORD result = acquireReadyLocked(obj);
	pthread_mutex_unlock(&w->m);
	return result;
}

// Acquires obj only if it is still ready. A multi-object wait may report an object that another thread took first.
static void consumeIfReady(ObjectBase *obj) {
	WaitableObject *w = (WaitableObject *)obj;
	pthread_mutex_lock(&w->m);
	if (isReadyLocked(obj)) {
		acquireReadyLocked(obj);
	}
	pthread_mutex_unlock(&w->m);
}

static void unrefObjects(ObjectBase **objects, DWORD count) {
	for (DWORD i = 0; i < count; ++i) {
		ObjectBase_unref(objects[i]);
	}
	free(objects);
}

void WINAPI Sleep(DWORD dwMilliseconds) {
	DEBUG_LOG("Sleep(%u)\n", dwMilliseconds);
	usleep((useconds_t)dwMilliseconds * 1000);
}

typedef struct {
	bool initialOwner;
} MutexMakeArgs;

static ObjectBase *makeMutex(void *ctx) {
	MutexMakeArgs *args = ctx;
	MutexObject *mu = MutexObject_alloc();
	if (args->initialOwner) {
		mu->owner = pthread_self();
		mu->ownerValid = true;
		mu->recursionCount = 1;
		mu->base.signaled = false;
	}
	return &mu->base.base;
}

HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCWSTR lpName) {
	char *nameForLog = wideStringToString(lpName, -1);
	DEBUG_LOG("CreateMutexW(%p, %d, %s)\n", lpMutexAttributes, bInitialOwner ? 1 : 0, nameForLog);
	free(nameForLog);
	const uint32_t grantedAccess = MUTEX_ALL_ACCESS;
	uint32_t handleFlags = 0;
	if (lpMutexAttributes && lpMutexAttributes->bInheritHandle) {
		handleFlags |= HANDLE_FLAG_INHERIT;
	}
	MutexMakeArgs args = {bInitialOwner != FALSE};
	bool created = false;
	ObjectBase *obj = Namespace_getOrCreate(&wibo_g_namespace, lpName, lpName ? wstr_len(lpName) : 0, makeMutex,
											&args, &created);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NO_HANDLE;
	}
	if (obj->type != ObjectType_Mutex) {
		// Name exists but isn't a mutex
		ObjectBase_unref(obj);
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NO_HANDLE;
	}
	HANDLE h = Handles_alloc(wibo_handles(), obj, grantedAccess, handleFlags);
	kernel32_setLastError(created ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS);
	return h;
}

HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName) {
	DEBUG_LOG("CreateMutexA -> ");
	uint16_t *wideName = lpName ? stringToWideString(lpName, SIZE_MAX) : NULL;
	HANDLE h = CreateMutexW(lpMutexAttributes, bInitialOwner, wideName);
	free(wideName);
	return h;
}

BOOL WINAPI ReleaseMutex(HANDLE hMutex) {
	DEBUG_LOG("ReleaseMutex(%p)\n", hMutex);
	ObjectBase *obj = Handles_getAs(wibo_handles(), hMutex, ObjectType_Mutex, NULL);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	MutexObject *mu = (MutexObject *)obj;
	WaitableObject *w = &mu->base;
	const pthread_t self = pthread_self();
	bool notify = false;
	pthread_mutex_lock(&w->m);
	if (!mu->ownerValid || !pthread_equal(mu->owner, self) || mu->recursionCount == 0) {
		pthread_mutex_unlock(&w->m);
		ObjectBase_unref(obj);
		kernel32_setLastError(ERROR_NOT_OWNER);
		return FALSE;
	}
	if (--mu->recursionCount == 0) {
		mu->ownerValid = false;
		w->signaled = true;
		notify = true;
	}
	pthread_mutex_unlock(&w->m);
	if (notify) {
		pthread_cond_broadcast(&w->cv);
		WaitableObject_notifyWaiters(w, false);
	}
	ObjectBase_unref(obj);
	return TRUE;
}

typedef struct {
	bool manualReset;
	bool initialState;
} EventMakeArgs;

static ObjectBase *makeEvent(void *ctx) {
	EventMakeArgs *args = ctx;
	EventObject *ev = EventObject_alloc(args->manualReset);
	ev->base.signaled = args->initialState;
	return &ev->base.base;
}

HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState,
						   LPCWSTR lpName) {
	char *nameForLog = wideStringToString(lpName, -1);
	DEBUG_LOG("CreateEventW(%p, %d, %d, %s)\n", lpEventAttributes, bManualReset ? 1 : 0, bInitialState ? 1 : 0,
			  nameForLog);
	free(nameForLog);
	const uint32_t grantedAccess = EVENT_ALL_ACCESS;
	uint32_t handleFlags = 0;
	if (lpEventAttributes && lpEventAttributes->bInheritHandle) {
		handleFlags |= HANDLE_FLAG_INHERIT;
	}
	EventMakeArgs args = {bManualReset != FALSE, bInitialState != FALSE};
	bool created = false;
	ObjectBase *obj = Namespace_getOrCreate(&wibo_g_namespace, lpName, lpName ? wstr_len(lpName) : 0, makeEvent,
											&args, &created);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NO_HANDLE;
	}
	if (obj->type != ObjectType_Event) {
		// Name exists but isn't an event
		ObjectBase_unref(obj);
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NO_HANDLE;
	}
	HANDLE h = Handles_alloc(wibo_handles(), obj, grantedAccess, handleFlags);
	DEBUG_LOG("-> %p (created=%d)\n", h, created ? 1 : 0);
	kernel32_setLastError(created ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS);
	return h;
}

HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState,
						   LPCSTR lpName) {
	DEBUG_LOG("CreateEventA -> ");
	uint16_t *wideName = lpName ? stringToWideString(lpName, SIZE_MAX) : NULL;
	HANDLE h = CreateEventW(lpEventAttributes, bManualReset, bInitialState, wideName);
	free(wideName);
	return h;
}

typedef struct {
	LONG initialCount;
	LONG maximumCount;
} SemaphoreMakeArgs;

static ObjectBase *makeSemaphore(void *ctx) {
	SemaphoreMakeArgs *args = ctx;
	if (args->maximumCount <= 0 || args->initialCount < 0 || args->initialCount > args->maximumCount) {
		return NULL;
	}
	return &SemaphoreObject_alloc(args->initialCount, args->maximumCount)->base.base;
}

HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
							   LPCWSTR lpName) {
	char *nameForLog = wideStringToString(lpName, -1);
	DEBUG_LOG("CreateSemaphoreW(%p, %ld, %ld, %s)\n", lpSemaphoreAttributes, lInitialCount, lMaximumCount,
			  nameForLog);
	free(nameForLog);
	const uint32_t granted = SEMAPHORE_ALL_ACCESS;
	uint32_t hflags = 0;
	if (lpSemaphoreAttributes && lpSemaphoreAttributes->bInheritHandle) {
		hflags |= HANDLE_FLAG_INHERIT;
	}
	SemaphoreMakeArgs args = {lInitialCount, lMaximumCount};
	bool created = false;
	ObjectBase *obj = Namespace_getOrCreate(&wibo_g_namespace, lpName, lpName ? wstr_len(lpName) : 0, makeSemaphore,
											&args, &created);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NO_HANDLE;
	}
	if (obj->type != ObjectType_Semaphore) {
		// Name exists but isn't a semaphore
		ObjectBase_unref(obj);
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NO_HANDLE;
	}
	HANDLE h = Handles_alloc(wibo_handles(), obj, granted, hflags);
	kernel32_setLastError(created ? ERROR_SUCCESS : ERROR_ALREADY_EXISTS);
	return h;
}

HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
							   LPCSTR lpName) {
	DEBUG_LOG("CreateSemaphoreA -> ");
	uint16_t *wideName = lpName ? stringToWideString(lpName, SIZE_MAX) : NULL;
	HANDLE h = CreateSemaphoreW(lpSemaphoreAttributes, lInitialCount, lMaximumCount, wideName);
	free(wideName);
	return h;
}

BOOL WINAPI ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, PLONG lpPreviousCount) {
	DEBUG_LOG("ReleaseSemaphore(%p, %ld, %p)\n", hSemaphore, lReleaseCount, lpPreviousCount);
	if (lReleaseCount < 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ObjectBase *obj = Handles_getAs(wibo_handles(), hSemaphore, ObjectType_Semaphore, NULL);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	SemaphoreObject *sem = (SemaphoreObject *)obj;
	WaitableObject *w = &sem->base;
	pthread_mutex_lock(&w->m);
	LONG prev = sem->count;
	if (sem->count > sem->maxCount - lReleaseCount) {
		pthread_mutex_unlock(&w->m);
		ObjectBase_unref(obj);
		kernel32_setLastError(ERROR_TOO_MANY_POSTS);
		return FALSE;
	}
	sem->count += lReleaseCount;
	bool notify = sem->count > 0;
	pthread_mutex_unlock(&w->m);
	if (notify) {
		pthread_cond_broadcast(&w->cv);
		WaitableObject_notifyWaiters(w, false);
	}
	if (lpPreviousCount) {
		*lpPreviousCount = prev;
	}
	ObjectBase_unref(obj);
	return TRUE;
}

BOOL WINAPI SetEvent(HANDLE hEvent) {
	DEBUG_LOG("SetEvent(%p)\n", hEvent);
	ObjectBase *obj = Handles_getAs(wibo_handles(), hEvent, ObjectType_Event, NULL);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	EventObject_set((EventObject *)obj);
	ObjectBase_unref(obj);
	return TRUE;
}

BOOL WINAPI ResetEvent(HANDLE hEvent) {
	DEBUG_LOG("ResetEvent(%p)\n", hEvent);
	ObjectBase *obj = Handles_getAs(wibo_handles(), hEvent, ObjectType_Event, NULL);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	EventObject_reset((EventObject *)obj);
	ObjectBase_unref(obj);
	return TRUE;
}

DWORD WINAPI WaitForSingleObjectEx(HANDLE hHandle, DWORD dwMilliseconds, BOOL bAlertable) {
	DEBUG_LOG("WaitForSingleObjectEx(%p, %u, %d)\n", hHandle, dwMilliseconds, bAlertable);
	(void)bAlertable;
	return WaitForSingleObject(hHandle, dwMilliseconds);
}

DWORD WINAPI WaitForMultipleObjectsEx(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds,
									  BOOL bAlertable) {
	DEBUG_LOG("WaitForMultipleObjectsEx(%u, %p, %d, %u, %d)\n", nCount, lpHandles, bWaitAll, dwMilliseconds,
			  bAlertable);
	(void)bAlertable;
	return WaitForMultipleObjects(nCount, lpHandles, bWaitAll, dwMilliseconds);
}

DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds) {
	DEBUG_LOG("WaitForSingleObject(%p, %u)\n", hHandle, dwMilliseconds);
	ObjectBase *obj = Handles_getWithFlag(wibo_handles(), hHandle, Of_Waitable, NULL);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		DEBUG_LOG("-> ERROR_INVALID_HANDLE\n");
		return WAIT_FAILED;
	}
	DEBUG_LOG("Waiting on object with type %d\n", (int)obj->type);
	DWORD result = waitOnObject(obj, dwMilliseconds);
	ObjectBase_unref(obj);
	return result;
}

DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds) {
	DEBUG_LOG("WaitForMultipleObjects(%u, %p, %d, %u)\n", nCount, lpHandles, bWaitAll ? 1 : 0, dwMilliseconds);
	if (nCount == 0 || nCount > MAXIMUM_WAIT_OBJECTS || !lpHandles) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return WAIT_FAILED;
	}

	ObjectBase **objects = xcalloc(nCount, sizeof(*objects));
	for (DWORD i = 0; i < nCount; ++i) {
		objects[i] = Handles_getWithFlag(wibo_handles(), lpHandles[i], Of_Waitable, NULL);
		if (!objects[i]) {
			unrefObjects(objects, i);
			kernel32_setLastError(ERROR_INVALID_HANDLE);
			return WAIT_FAILED;
		}
	}

	WaitBlock block;
	WaitBlock_init(&block, bWaitAll != FALSE, nCount);
	for (DWORD i = 0; i < nCount; ++i) {
		WaitableObject_registerWaiter((WaitableObject *)objects[i], &block, i, WaitBlock_onSignal);
	}
	for (DWORD i = 0; i < nCount; ++i) {
		bool abandoned = false;
		if (isObjectAvailable(objects[i], &abandoned)) {
			WaitBlock_signal(&block, i, abandoned);
		}
	}
	WaitDeadline deadline;
	WaitDeadline_init(&deadline, dwMilliseconds);
	DWORD waitResult = WaitBlock_wait(&block, &deadline);
	for (DWORD i = 0; i < nCount; ++i) {
		WaitableObject_unregisterWaiter((WaitableObject *)objects[i], &block);
	}
	WaitBlock_free(&block);

	if (waitResult == WAIT_TIMEOUT) {
		unrefObjects(objects, nCount);
		return WAIT_TIMEOUT;
	}

	if (bWaitAll != FALSE) {
		if (waitResult == WAIT_OBJECT_0) {
			for (DWORD i = 0; i < nCount; ++i) {
				consumeIfReady(objects[i]);
			}
		} else if (waitResult >= WAIT_ABANDONED && waitResult < WAIT_ABANDONED + nCount) {
			consumeIfReady(objects[waitResult - WAIT_ABANDONED]);
		}
	} else if (waitResult - WAIT_OBJECT_0 < nCount) {
		consumeIfReady(objects[waitResult - WAIT_OBJECT_0]);
	} else if (waitResult - WAIT_ABANDONED < nCount) {
		consumeIfReady(objects[waitResult - WAIT_ABANDONED]);
	}

	unrefObjects(objects, nCount);
	return waitResult;
}

BOOL WINAPI WaitOnAddress(volatile VOID *Address, PVOID CompareAddress, SIZE_T AddressSize, DWORD dwMilliseconds) {
	VERBOSE_LOG("WaitOnAddress(%p, %p, %zu, %u)\n", Address, CompareAddress, AddressSize, dwMilliseconds);
	if (!Address || !CompareAddress || !isAddressWaitSize(AddressSize)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return waitWhileEqual(Address, CompareAddress, AddressSize, dwMilliseconds);
}

void WINAPI WakeByAddressSingle(PVOID Address) {
	VERBOSE_LOG("WakeByAddressSingle(%p)\n", Address);
	if (!Address) {
		return;
	}
	wakeAddressWaiters(Address);
}

void WINAPI WakeByAddressAll(PVOID Address) {
	VERBOSE_LOG("WakeByAddressAll(%p)\n", Address);
	if (!Address) {
		return;
	}
	wakeAddressWaiters(Address);
}

BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount, DWORD Flags) {
	DEBUG_LOG("InitializeCriticalSectionEx(%p, %u, 0x%x)\n", lpCriticalSection, dwSpinCount, Flags);
	if (!lpCriticalSection) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	memset(lpCriticalSection, 0, sizeof(*lpCriticalSection));
	if (Flags & RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO) {
		lpCriticalSection->DebugInfo = (GUEST_PTR)-1;
	} else {
		RTL_CRITICAL_SECTION_DEBUG *debugInfo = wibo_heap_guestMalloc(sizeof(RTL_CRITICAL_SECTION_DEBUG), true);
		debugInfo->CriticalSection = toGuestPtr(lpCriticalSection);
		debugInfo->ProcessLocksList.Blink = toGuestPtr(&debugInfo->ProcessLocksList);
		debugInfo->ProcessLocksList.Flink = toGuestPtr(&debugInfo->ProcessLocksList);
		lpCriticalSection->DebugInfo = toGuestPtr(debugInfo);
	}
	lpCriticalSection->LockCount = -1;
	lpCriticalSection->SpinCount = dwSpinCount;
	return TRUE;
}

void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection) {
	DEBUG_LOG("InitializeCriticalSection(%p)\n", lpCriticalSection);
	InitializeCriticalSectionEx(lpCriticalSection, 0, 0);
}

BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount) {
	DEBUG_LOG("InitializeCriticalSectionAndSpinCount(%p, %u)\n", lpCriticalSection, dwSpinCount);
	InitializeCriticalSectionEx(lpCriticalSection, dwSpinCount, 0);
	return TRUE;
}

void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection) {
	DEBUG_LOG("DeleteCriticalSection(%p)\n", lpCriticalSection);
	if (!lpCriticalSection) {
		return;
	}

	if (lpCriticalSection->DebugInfo && lpCriticalSection->DebugInfo != (GUEST_PTR)-1) {
		RTL_CRITICAL_SECTION_DEBUG *debugInfo = fromGuestPtr(lpCriticalSection->DebugInfo);
		if (debugInfo && debugInfo->Spare[0] == 0) {
			wibo_heap_guestFree(debugInfo);
		}
	}

	lpCriticalSection->DebugInfo = GUEST_NULL;
	lpCriticalSection->RecursionCount = 0;
	lpCriticalSection->SpinCount = 0;
	setOwningThread(lpCriticalSection, 0);

	InterlockedExchange((LONG volatile *)&lpCriticalSection->LockSemaphore, 0);
	wakeAddressWaiters(&lpCriticalSection->LockSemaphore);

	InterlockedExchange(&lpCriticalSection->LockCount, -1);
	wakeAddressWaiters(&lpCriticalSection->LockCount);
}

BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection) {
	VERBOSE_LOG("TryEnterCriticalSection(%p)\n", lpCriticalSection);
	if (!lpCriticalSection) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	LONG volatile *lockCount = &lpCriticalSection->LockCount;
	const DWORD threadId = GetCurrentThreadId();

	LONG previous = InterlockedCompareExchange(lockCount, 0, -1);
	if (previous == -1) {
		setOwningThread(lpCriticalSection, threadId);
		lpCriticalSection->RecursionCount = 1;
		return TRUE;
	}

	if (owningThreadId(lpCriticalSection) == threadId) {
		InterlockedIncrement(lockCount);
		lpCriticalSection->RecursionCount++;
		return TRUE;
	}
	return FALSE;
}

void WINAPI EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection) {
	VERBOSE_LOG("EnterCriticalSection(%p)\n", lpCriticalSection);
	if (!lpCriticalSection) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return;
	}

	const DWORD threadId = GetCurrentThreadId();
	if (trySpinAcquireCriticalSection(lpCriticalSection, threadId)) {
		return;
	}

	LONG result = InterlockedIncrement(&lpCriticalSection->LockCount);
	if (result) {
		if (owningThreadId(lpCriticalSection) == threadId) {
			lpCriticalSection->RecursionCount++;
			return;
		}
		waitForCriticalSection(lpCriticalSection);
	}
	setOwningThread(lpCriticalSection, threadId);
	lpCriticalSection->RecursionCount = 1;
}

void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection) {
	VERBOSE_LOG("LeaveCriticalSection(%p)\n", lpCriticalSection);
	if (!lpCriticalSection) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return;
	}

	if (--lpCriticalSection->RecursionCount != 0) {
		InterlockedDecrement(&lpCriticalSection->LockCount);
		return;
	}

	setOwningThread(lpCriticalSection, 0);
	LONG newValue = InterlockedDecrement(&lpCriticalSection->LockCount);
	if (newValue >= 0) {
		signalCriticalSection(lpCriticalSection);
	}
}

BOOL WINAPI InitOnceExecuteOnce(PINIT_ONCE lpInitOnce, PINIT_ONCE_FN lpInitFn, LPVOID lpParameter,
								GUEST_PTR *lpContext) {
	DEBUG_LOG("InitOnceExecuteOnce(%p, %p, %p, %p)\n", lpInitOnce, lpInitFn, lpParameter, lpContext);
	BOOL pending = FALSE;
	GUEST_PTR context = GUEST_NULL;
	if (!InitOnceBeginInitialize(lpInitOnce, 0, &pending, &context)) {
		return FALSE;
	}
	if (!pending) {
		if (lpContext) {
			*lpContext = context;
		}
		return TRUE;
	}
	// The callback writes its context through a pointer, which must be visible to the guest.
	GUEST_PTR *slot = wibo_heap_guestMalloc(sizeof(GUEST_PTR), false);
	if (!slot) {
		InitOnceComplete(lpInitOnce, INIT_ONCE_INIT_FAILED, NULL);
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	*slot = GUEST_NULL;
	BOOL ok = call_PINIT_ONCE_FN(lpInitFn, lpInitOnce, lpParameter, slot);
	GUEST_PTR result = *slot;
	wibo_heap_guestFree(slot);
	// A failed initialization must not pass a context, which InitOnceComplete rejects; the state would stay at
	// "running" and leave waiters blocked.
	InitOnceComplete(lpInitOnce, ok ? 0 : INIT_ONCE_INIT_FAILED, ok ? fromGuestPtr(result) : NULL);
	if (!ok) {
		return FALSE;
	}
	if (lpContext) {
		*lpContext = result;
	}
	return TRUE;
}

BOOL WINAPI InitOnceBeginInitialize(LPINIT_ONCE lpInitOnce, DWORD dwFlags, PBOOL fPending, GUEST_PTR *lpContext) {
	DEBUG_LOG("InitOnceBeginInitialize(%p, %u, %p, %p)\n", lpInitOnce, dwFlags, fPending, lpContext);
	if (!lpInitOnce) {
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (dwFlags & ~(INIT_ONCE_CHECK_ONLY | INIT_ONCE_ASYNC)) {
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	GUEST_PTR *state = &lpInitOnce->Ptr;

	if (dwFlags & INIT_ONCE_CHECK_ONLY) {
		if (dwFlags & INIT_ONCE_ASYNC) {
			DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		GUEST_PTR val = __atomic_load_n(state, __ATOMIC_ACQUIRE);
		if ((val & kInitOnceStateMask) != kInitOnceStateCompleted) {
			if (fPending) {
				*fPending = TRUE;
			}
			DEBUG_LOG("-> ERROR_GEN_FAILURE\n");
			kernel32_setLastError(ERROR_GEN_FAILURE);
			return FALSE;
		}
		if (fPending) {
			*fPending = FALSE;
		}
		if (lpContext) {
			*lpContext = val & ~kInitOnceStateMask;
		}
		return TRUE;
	}

	while (true) {
		GUEST_PTR val = __atomic_load_n(state, __ATOMIC_ACQUIRE);
		switch (val & kInitOnceStateMask) {
		case 0: { // first time
			GUEST_PTR expected = 0;
			if (dwFlags & INIT_ONCE_ASYNC) {
				if (__atomic_compare_exchange_n(state, &expected, (GUEST_PTR)kInitOnceStateAsyncPending, false,
												__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
					if (fPending) {
						*fPending = TRUE;
					}
					return TRUE;
				}
			} else if (__atomic_compare_exchange_n(state, &expected, (GUEST_PTR)kInitOnceStateSyncRunning, false,
												   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
				if (fPending) {
					*fPending = TRUE;
				}
				return TRUE;
			}
			break;
		}
		case kInitOnceStateSyncRunning: { // synchronous initialization in progress
			if (dwFlags & INIT_ONCE_ASYNC) {
				DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
				kernel32_setLastError(ERROR_INVALID_PARAMETER);
				return FALSE;
			}
			GUEST_PTR running = kInitOnceStateSyncRunning;
			waitWhileEqual(state, &running, sizeof(running), INFINITE);
			break;
		}
		case kInitOnceStateCompleted: {
			if (fPending) {
				*fPending = FALSE;
			}
			if (lpContext) {
				*lpContext = val & ~kInitOnceStateMask;
			}
			return TRUE;
		}
		case kInitOnceStateAsyncPending: { // async pending
			if (!(dwFlags & INIT_ONCE_ASYNC)) {
				DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
				kernel32_setLastError(ERROR_INVALID_PARAMETER);
				return FALSE;
			}
			if (fPending) {
				*fPending = TRUE;
			}
			return TRUE;
		}
		default:
			break;
		}
	}
}

BOOL WINAPI InitOnceComplete(LPINIT_ONCE lpInitOnce, DWORD dwFlags, LPVOID lpContext) {
	DEBUG_LOG("InitOnceComplete(%p, %u, %p)\n", lpInitOnce, dwFlags, lpContext);
	if (!lpInitOnce) {
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (dwFlags & ~(INIT_ONCE_ASYNC | INIT_ONCE_INIT_FAILED)) {
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const bool markFailed = (dwFlags & INIT_ONCE_INIT_FAILED) != 0;
	if (markFailed) {
		if (lpContext) {
			DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		if (dwFlags & INIT_ONCE_ASYNC) {
			DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
	}

	const GUEST_PTR contextValue = (GUEST_PTR)(uintptr_t)lpContext;
	if (!markFailed && (contextValue & kInitOnceReservedMask)) {
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	GUEST_PTR *state = &lpInitOnce->Ptr;
	const GUEST_PTR finalValue = markFailed ? 0 : (contextValue | kInitOnceStateCompleted);

	while (true) {
		GUEST_PTR val = __atomic_load_n(state, __ATOMIC_ACQUIRE);
		switch (val & kInitOnceStateMask) {
		case kInitOnceStateSyncRunning:
			if (!__atomic_compare_exchange_n(state, &val, finalValue, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
				continue;
			}
			wakeAddressWaiters(state);
			return TRUE;
		case kInitOnceStateAsyncPending:
			if (!(dwFlags & INIT_ONCE_ASYNC)) {
				DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
				kernel32_setLastError(ERROR_INVALID_PARAMETER);
				return FALSE;
			}
			if (!__atomic_compare_exchange_n(state, &val, finalValue, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
				continue;
			}
			return TRUE;
		default:
			DEBUG_LOG("-> ERROR_GEN_FAILURE\n");
			kernel32_setLastError(ERROR_GEN_FAILURE);
			return FALSE;
		}
	}
}

void WINAPI InitializeSRWLock(PSRWLOCK SRWLock) {
	DEBUG_LOG("InitializeSRWLock(%p)\n", SRWLock);
	if (SRWLock) {
		SRWLock->Value = 0;
	}
}

void WINAPI AcquireSRWLockShared(PSRWLOCK SRWLock) {
	VERBOSE_LOG("AcquireSRWLockShared(%p)\n", SRWLock);
	if (!SRWLock) {
		return;
	}
	ULONG *value = &SRWLock->Value;
	while (true) {
		ULONG current = __atomic_load_n(value, __ATOMIC_ACQUIRE);
		if (current & kSrwLockExclusive) {
			ULONG observed = current;
			waitWhileEqual(value, &observed, sizeof(observed), INFINITE);
			continue;
		}
		ULONG desired = current + kSrwLockSharedIncrement;
		if (__atomic_compare_exchange_n(value, &current, desired, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return;
		}
	}
}

void WINAPI ReleaseSRWLockShared(PSRWLOCK SRWLock) {
	VERBOSE_LOG("ReleaseSRWLockShared(%p)\n", SRWLock);
	if (!SRWLock) {
		return;
	}
	ULONG *value = &SRWLock->Value;
	ULONG previous = __atomic_fetch_sub(value, kSrwLockSharedIncrement, __ATOMIC_ACQ_REL);
	ULONG newValue = previous - kSrwLockSharedIncrement;
	if (newValue == 0) {
		wakeAddressWaiters(value);
	}
}

void WINAPI AcquireSRWLockExclusive(PSRWLOCK SRWLock) {
	VERBOSE_LOG("AcquireSRWLockExclusive(%p)\n", SRWLock);
	if (!SRWLock) {
		return;
	}
	ULONG *value = &SRWLock->Value;
	while (true) {
		ULONG expected = 0;
		if (__atomic_compare_exchange_n(value, &expected, kSrwLockExclusive, false, __ATOMIC_ACQ_REL,
										__ATOMIC_ACQUIRE)) {
			return;
		}
		waitWhileEqual(value, &expected, sizeof(expected), INFINITE);
	}
}

void WINAPI ReleaseSRWLockExclusive(PSRWLOCK SRWLock) {
	VERBOSE_LOG("ReleaseSRWLockExclusive(%p)\n", SRWLock);
	if (!SRWLock) {
		return;
	}
	__atomic_store_n(&SRWLock->Value, 0u, __ATOMIC_RELEASE);
	wakeAddressWaiters(&SRWLock->Value);
}

BOOLEAN WINAPI TryAcquireSRWLockExclusive(PSRWLOCK SRWLock) {
	VERBOSE_LOG("TryAcquireSRWLockExclusive(%p)\n", SRWLock);
	if (!SRWLock) {
		return FALSE;
	}
	ULONG expected = 0;
	return __atomic_compare_exchange_n(&SRWLock->Value, &expected, kSrwLockExclusive, false, __ATOMIC_ACQ_REL,
									   __ATOMIC_ACQUIRE);
}

BOOLEAN WINAPI TryAcquireSRWLockShared(PSRWLOCK SRWLock) {
	VERBOSE_LOG("TryAcquireSRWLockShared(%p)\n", SRWLock);
	if (!SRWLock) {
		return FALSE;
	}
	ULONG current = __atomic_load_n(&SRWLock->Value, __ATOMIC_ACQUIRE);
	while (!(current & kSrwLockExclusive)) {
		ULONG desired = current + kSrwLockSharedIncrement;
		if (__atomic_compare_exchange_n(&SRWLock->Value, &current, desired, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return TRUE;
		}
	}
	return FALSE;
}
