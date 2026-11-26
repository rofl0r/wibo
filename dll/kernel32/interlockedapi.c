#include "interlockedapi.h"

#include "common.h"
#include "context.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_slistLock = PTHREAD_MUTEX_INITIALIZER;

LONG WINAPI InterlockedIncrement(LONG volatile *Addend) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedIncrement(%p)\n", Addend);
	LONG *ptr = (LONG *)Addend;
	return __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedDecrement(LONG volatile *Addend) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedDecrement(%p)\n", Addend);
	LONG *ptr = (LONG *)Addend;
	return __atomic_sub_fetch(ptr, 1, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedExchange(LONG volatile *Target, LONG Value) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedExchange(%p, %ld)\n", Target, (long)Value);
	LONG *ptr = (LONG *)Target;
	return __atomic_exchange_n(ptr, Value, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedCompareExchange(LONG volatile *Destination, LONG Exchange, LONG Comperand) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedCompareExchange(%p, %ld, %ld)\n", Destination, (long)Exchange, (long)Comperand);

	LONG *ptr = (LONG *)Destination;
	LONG expected = Comperand;
	__atomic_compare_exchange_n(ptr, &expected, Exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}

void WINAPI InitializeSListHead(PSLIST_HEADER ListHead) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InitializeSListHead(%p)\n", ListHead);
	if (!ListHead) {
		return;
	}
	memset(ListHead, 0, sizeof(*ListHead));
}

long long WINAPI InterlockedCompareExchange64(long long volatile *Destination, long long Exchange,
											  long long Comperand) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedCompareExchange64(%p)\n", Destination);
	long long *ptr = (long long *)Destination;
	long long expected = Comperand;
	__atomic_compare_exchange_n(ptr, &expected, Exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}

PSLIST_ENTRY WINAPI InterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InterlockedPushEntrySList(%p, %p)\n", ListHead, ListEntry);
	pthread_mutex_lock(&g_slistLock);
	GUEST_PTR previous = ListHead->Head;
	ListEntry->Next = previous;
	ListHead->Head = toGuestPtr(ListEntry);
	ListHead->Depth++;
	ListHead->Sequence++;
	pthread_mutex_unlock(&g_slistLock);
	return (PSLIST_ENTRY)fromGuestPtr(previous);
}

PSLIST_ENTRY WINAPI InterlockedPopEntrySList(PSLIST_HEADER ListHead) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InterlockedPopEntrySList(%p)\n", ListHead);
	pthread_mutex_lock(&g_slistLock);
	if (ListHead->Head == GUEST_NULL) {
		pthread_mutex_unlock(&g_slistLock);
		return NULL;
	}
	PSLIST_ENTRY entry = (PSLIST_ENTRY)fromGuestPtr(ListHead->Head);
	ListHead->Head = entry->Next;
	ListHead->Depth--;
	ListHead->Sequence++;
	pthread_mutex_unlock(&g_slistLock);
	return entry;
}
