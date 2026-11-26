#include "heapapi.h"
#include "heap.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"

#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>

static pthread_once_t g_processHeapInitOnce = PTHREAD_ONCE_INIT;
static HANDLE g_processHeapHandle = NO_HANDLE;
static HeapObject *g_processHeapRecord = NULL;

static void ensureProcessHeapInitializedOnce(void) {
	HeapObject *record = HeapObject_alloc(NULL);
	record->isProcessHeap = true;
	g_processHeapRecord = record;
	g_processHeapHandle = Handles_alloc(wibo_handles(), &record->base, 0, 0);
}

static void ensureProcessHeapInitialized(void) { pthread_once(&g_processHeapInitOnce, ensureProcessHeapInitializedOnce); }

static bool isExecutableHeap(const HeapObject *record) {
	return record && ((record->createFlags & HEAP_CREATE_ENABLE_EXECUTE) != 0);
}

static HeapObject *heapObjectFromHandle(HANDLE hHeap) {
	return (HeapObject *)Handles_getAs(wibo_handles(), hHeap, ObjectType_Heap, NULL);
}

static void releaseHeapObject(HeapObject *record) {
	if (record) {
		ObjectBase_unref(&record->base);
	}
}

static LPVOID heapAllocFromRecord(HeapObject *record, DWORD dwFlags, SIZE_T dwBytes) {
	if (!record) {
		return NULL;
	}
	if ((record->createFlags | dwFlags) & HEAP_GENERATE_EXCEPTIONS) {
		DEBUG_LOG("HeapAlloc: HEAP_GENERATE_EXCEPTIONS not supported\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	const bool zeroMemory = (dwFlags & HEAP_ZERO_MEMORY) != 0;
	const SIZE_T requestSize = dwBytes > 0 ? dwBytes : 1;
	void *mem = record->heap ? Heap_malloc(record->heap, requestSize, zeroMemory)
							 : wibo_heap_guestMalloc(requestSize, zeroMemory);
	if (!mem) {
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	if (isExecutableHeap(record)) {
		kernel32_tryMarkExecutable(mem);
	}
	return mem;
}

static BOOL heapSetInformationOnRecord(HeapObject *record, HEAP_INFORMATION_CLASS heapInformationClass,
									   PVOID heapInformation, SIZE_T heapInformationLength) {
	switch (heapInformationClass) {
	case HeapCompatibilityInformation: {
		if (!heapInformation || heapInformationLength < sizeof(ULONG)) {
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		record->compatibility = *(ULONG *)heapInformation;
		return TRUE;
	}
	case HeapEnableTerminationOnCorruption:
		return TRUE;
	case HeapOptimizeResources:
		kernel32_setLastError(ERROR_CALL_NOT_IMPLEMENTED);
		return FALSE;
	default:
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
}

static LPVOID heapReAllocFromRecord(HeapObject *record, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes) {
	if (lpMem == NULL) {
		void *alloc = heapAllocFromRecord(record, dwFlags, dwBytes);
		VERBOSE_LOG("-> %p (alloc)\n", alloc);
		return alloc;
	}
	if ((record->createFlags | dwFlags) & HEAP_GENERATE_EXCEPTIONS) {
		VERBOSE_LOG("-> NULL (exceptions unsupported)\n");
		kernel32_setLastError(ERROR_NOT_SUPPORTED);
		return NULL;
	}
	const bool inplaceOnly = (dwFlags & HEAP_REALLOC_IN_PLACE_ONLY) != 0;
	const bool zeroMemory = (dwFlags & HEAP_ZERO_MEMORY) != 0;
	if (dwBytes == 0) {
		if (!inplaceOnly) {
			wibo_heap_guestFree(lpMem);
			VERBOSE_LOG("-> NULL (freed)\n");
			return NULL;
		}
		VERBOSE_LOG("-> NULL (zero size with in-place flag)\n");
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}

	const SIZE_T requestSize = dwBytes > 0 ? dwBytes : 1;
	const SIZE_T oldSize = wibo_heap_guestSize(lpMem);
	if (inplaceOnly || requestSize <= oldSize) {
		if (requestSize > oldSize) {
			VERBOSE_LOG("-> NULL (cannot grow in place)\n");
			kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
			return NULL;
		}
		VERBOSE_LOG("-> %p (in-place)\n", lpMem);
		return lpMem;
	}

	void *ret = record->heap ? Heap_realloc(record->heap, lpMem, requestSize, zeroMemory)
							 : wibo_heap_guestRealloc(lpMem, requestSize, zeroMemory);
	if (isExecutableHeap(record)) {
		kernel32_tryMarkExecutable(ret);
	}
	VERBOSE_LOG("-> %p\n", ret);
	return ret;
}

void HeapObject_free(HeapObject *self) {
	if (self->isProcessHeap) {
		g_processHeapHandle = NO_HANDLE;
		g_processHeapRecord = NULL;
	}
	if (self->heap) {
		Heap_destroy(self->heap);
		self->heap = NULL;
	}
}

HANDLE WINAPI HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("HeapCreate(%u, %zu, %zu)\n", flOptions, dwInitialSize, dwMaximumSize);
	if (dwMaximumSize != 0 && dwInitialSize > dwMaximumSize) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}

	HeapObject *record = HeapObject_alloc(Heap_create());
	record->createFlags = flOptions;
	record->initialSize = dwInitialSize;
	record->maximumSize = dwMaximumSize;
	return Handles_alloc(wibo_handles(), &record->base, 0, 0);
}

BOOL WINAPI HeapDestroy(HANDLE hHeap) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("HeapDestroy(%p)\n", hHeap);
	HeapObject *record = heapObjectFromHandle(hHeap);
	if (!record || !HeapObject_isOwner(record) || record->isProcessHeap) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		releaseHeapObject(record);
		return FALSE;
	}
	if (record->heap) {
		Heap_destroy(record->heap);
		record->heap = NULL;
	}
	Handles_release(wibo_handles(), hHeap);
	releaseHeapObject(record);
	return TRUE;
}

HANDLE WINAPI GetProcessHeap() {
	HOST_CONTEXT_GUARD();
	ensureProcessHeapInitialized();
	DEBUG_LOG("GetProcessHeap() -> %p\n", g_processHeapHandle);
	return g_processHeapHandle;
}

BOOL WINAPI HeapSetInformation(HANDLE HeapHandle, HEAP_INFORMATION_CLASS HeapInformationClass, PVOID HeapInformation,
							   SIZE_T HeapInformationLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("HeapSetInformation(%p, %d, %p, %zu)\n", HeapHandle, (int)HeapInformationClass, HeapInformation,
			  HeapInformationLength);
	HeapObject *record = heapObjectFromHandle(HeapHandle);
	if (!record || !HeapObject_canAccess(record)) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		releaseHeapObject(record);
		return FALSE;
	}
	BOOL ret = heapSetInformationOnRecord(record, HeapInformationClass, HeapInformation, HeapInformationLength);
	releaseHeapObject(record);
	return ret;
}

LPVOID WINAPI HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("HeapAlloc(%p, 0x%x, %zu) ", hHeap, dwFlags, dwBytes);
	HeapObject *record = heapObjectFromHandle(hHeap);
	if (!record || !HeapObject_canAccess(record)) {
		VERBOSE_LOG("-> NULL\n");
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		releaseHeapObject(record);
		return NULL;
	}
	void *mem = heapAllocFromRecord(record, dwFlags, dwBytes);
	VERBOSE_LOG("-> %p\n", mem);
	releaseHeapObject(record);
	return mem;
}

LPVOID WINAPI HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("HeapReAlloc(%p, 0x%x, %p, %zu) ", hHeap, dwFlags, lpMem, dwBytes);
	HeapObject *record = heapObjectFromHandle(hHeap);
	if (!record || !HeapObject_canAccess(record)) {
		VERBOSE_LOG("-> NULL\n");
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		releaseHeapObject(record);
		return NULL;
	}
	void *ret = heapReAllocFromRecord(record, dwFlags, lpMem, dwBytes);
	releaseHeapObject(record);
	return ret;
}

SIZE_T WINAPI HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("HeapSize(%p, 0x%x, %p)\n", hHeap, dwFlags, lpMem);
	(void)dwFlags;
	HeapObject *record = heapObjectFromHandle(hHeap);
	if (!record || !HeapObject_canAccess(record)) {
		VERBOSE_LOG("-> ERROR_INVALID_HANDLE\n");
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		releaseHeapObject(record);
		return (SIZE_T)-1;
	}
	if (!lpMem) {
		VERBOSE_LOG("-> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		releaseHeapObject(record);
		return (SIZE_T)-1;
	}
	SIZE_T size = wibo_heap_guestSize(lpMem);
	releaseHeapObject(record);
	return size;
}

BOOL WINAPI HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("HeapFree(%p, 0x%x, %p)\n", hHeap, dwFlags, lpMem);
	(void)dwFlags;
	if (lpMem == NULL) {
		return TRUE;
	}
	HeapObject *record = heapObjectFromHandle(hHeap);
	if (!record || !HeapObject_canAccess(record)) {
		VERBOSE_LOG("-> ERROR_INVALID_HANDLE\n");
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		releaseHeapObject(record);
		return FALSE;
	}
	bool ret = record->heap ? Heap_free(record->heap, lpMem) : wibo_heap_guestFree(lpMem);
	releaseHeapObject(record);
	if (!ret) {
		VERBOSE_LOG("-> ERROR_INVALID_PARAMETER (not owned)\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	VERBOSE_LOG("-> SUCCESS\n");
	return TRUE;
}

BOOL WINAPI HeapValidate(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("STUB: HeapValidate(0x%x, 0x%x, %p)\n", (unsigned int)(uintptr_t)hHeap, dwFlags, lpMem);
	(void)dwFlags;
	(void)lpMem;
	HeapObject *record = heapObjectFromHandle(hHeap);
	BOOL ok = record && HeapObject_canAccess(record);
	releaseHeapObject(record);
	return ok;
}
