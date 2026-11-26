#include "tls.h"
#include "common.h"
#include "cutil.h"
#include "heap.h"
#include "types.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define kMaxExpansionSlots (kTlsMaxSlotCount - kTlsSlotCount)

typedef VEC(TEB *) TebVec;

typedef struct TlsArray {
	size_t capacity;
	GUEST_PTR slots[];
} TlsArray;

typedef struct {
	TEB *tib;
	TlsArray *array;
} GarbageEntry;

typedef VEC(GarbageEntry) GarbageVec;

typedef struct {
	TEB *tib;
	TlsArray *oldArray;
	TlsArray *newArray;
} PendingResize;

typedef VEC(PendingResize) PendingResizeVec;

static pthread_mutex_t g_tlsMutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_slotUsed[kTlsMaxSlotCount];
static TebVec g_activeTibs;
static size_t g_expansionCapacity = 0;

static hashmap *g_moduleArrays = NULL;
static GarbageVec g_moduleGarbage;
static size_t g_moduleArrayCapacity = 0;

static size_t minSize(size_t a, size_t b) { return a < b ? a : b; }
static size_t maxSize(size_t a, size_t b) { return a > b ? a : b; }

static hashmap *moduleArrayMap(void) {
	if (!g_moduleArrays) {
		g_moduleArrays = hashmap_create();
	}
	return g_moduleArrays;
}

static TlsArray *allocateTlsArray(size_t capacity) {
	if (capacity == 0 || capacity > kMaxExpansionSlots) {
		return NULL;
	}
	const size_t bytes = sizeof(TlsArray) + capacity * sizeof(GUEST_PTR);
	TlsArray *arr = wibo_heap_guestMalloc(bytes, true);
	if (!arr) {
		return NULL;
	}
	arr->capacity = capacity;
	return arr;
}

static inline TlsArray *arrayFromSlots(GUEST_PTR slots) {
	return slots ? (TlsArray *)((uint8_t *)fromGuestPtr(slots) - offsetof(TlsArray, slots)) : NULL;
}

static TlsArray *getExpansionArray(TEB *tib) {
	if (!tib) {
		return NULL;
	}
	return arrayFromSlots(tib->TlsExpansionSlots);
}

static void setExpansionArray(TEB *tib, TlsArray *arr) {
	if (!tib) {
		return;
	}
	tib->TlsExpansionSlots = arr ? toGuestPtr(arr->slots) : GUEST_NULL;
}

static size_t chooseCapacity(size_t current, size_t required) {
	if (required == 0) {
		return current;
	}
	if (required > kMaxExpansionSlots) {
		return 0;
	}
	size_t capacity = current;
	if (capacity == 0) {
		capacity = 1;
	}
	while (capacity < required) {
		size_t next = capacity * 2;
		if (next <= capacity || next > kMaxExpansionSlots) {
			capacity = kMaxExpansionSlots;
		} else {
			capacity = next;
		}
	}
	if (capacity > kMaxExpansionSlots) {
		capacity = kMaxExpansionSlots;
	}
	if (capacity < required) {
		return 0;
	}
	return capacity;
}

static bool ensureGlobalExpansionCapacityLocked(size_t required) {
	if (required == 0) {
		return true;
	}
	if (required <= g_expansionCapacity) {
		return true;
	}
	size_t target = chooseCapacity(g_expansionCapacity, required);
	if (target == 0) {
		return false;
	}
	PendingResizeVec pending = {0};
	vec_reserve(&pending, g_activeTibs.len);
	for (size_t i = 0; i < g_activeTibs.len; ++i) {
		TEB *tib = g_activeTibs.data[i];
		TlsArray *currArr = getExpansionArray(tib);
		size_t currentCapacity = currArr ? currArr->capacity : 0;
		if (currentCapacity >= target) {
			continue;
		}
		TlsArray *newArr = allocateTlsArray(target);
		if (!newArr) {
			for (size_t j = 0; j < pending.len; ++j) {
				wibo_heap_guestFree(pending.data[j].newArray);
			}
			vec_free(&pending);
			return false;
		}
		if (currArr) {
			memcpy(newArr->slots, currArr->slots, minSize(currArr->capacity, newArr->capacity) * sizeof(GUEST_PTR));
		}
		PendingResize entry = {tib, currArr, newArr};
		vec_push(&pending, entry);
	}
	for (size_t i = 0; i < pending.len; ++i) {
		setExpansionArray(pending.data[i].tib, pending.data[i].newArray);
	}
	for (size_t i = 0; i < pending.len; ++i) {
		if (pending.data[i].oldArray) {
			wibo_heap_guestFree(pending.data[i].oldArray);
		}
	}
	vec_free(&pending);
	g_expansionCapacity = target;
	return true;
}

static TlsArray *getModuleArray(TEB *tib) {
	if (!tib) {
		return NULL;
	}
	void *cached = NULL;
	if (hashmap_find(moduleArrayMap(), &tib, sizeof tib, &cached)) {
		return cached;
	}
	TlsArray *array = arrayFromSlots(tib->ThreadLocalStoragePointer);
	if (array) {
		hashmap_put(moduleArrayMap(), &tib, sizeof tib, array);
	}
	return array;
}

static void queueOldModuleArray(TEB *tib, TlsArray *array) {
	if (!tib || !array) {
		return;
	}
	GarbageEntry entry = {tib, array};
	vec_push(&g_moduleGarbage, entry);
}

static bool ensureModuleArrayCapacityLocked(size_t required) {
	if (required == 0) {
		return true;
	}
	if (required <= g_moduleArrayCapacity) {
		return true;
	}
	size_t target = g_moduleArrayCapacity ? g_moduleArrayCapacity : (size_t)1;
	while (target < required) {
		size_t next = target * 2;
		if (next <= target) {
			target = required;
			break;
		}
		target = maxSize(next, required);
	}
	PendingResizeVec pending = {0};
	vec_reserve(&pending, g_activeTibs.len);
	for (size_t i = 0; i < g_activeTibs.len; ++i) {
		TEB *tib = g_activeTibs.data[i];
		TlsArray *current = getModuleArray(tib);
		size_t currentCapacity = current ? current->capacity : 0;
		if (currentCapacity >= target) {
			continue;
		}
		TlsArray *newArray = allocateTlsArray(target);
		if (!newArray) {
			for (size_t j = 0; j < pending.len; ++j) {
				wibo_heap_guestFree(pending.data[j].newArray);
			}
			vec_free(&pending);
			return false;
		}
		if (current) {
			memcpy(newArray->slots, current->slots, minSize(current->capacity, newArray->capacity) * sizeof(GUEST_PTR));
		}
		PendingResize entry = {tib, current, newArray};
		vec_push(&pending, entry);
	}
	for (size_t i = 0; i < pending.len; ++i) {
		PendingResize *entry = &pending.data[i];
		hashmap_put(moduleArrayMap(), &entry->tib, sizeof entry->tib, entry->newArray);
		entry->tib->ThreadLocalStoragePointer = toGuestPtr(entry->newArray->slots);
		if (entry->oldArray) {
			queueOldModuleArray(entry->tib, entry->oldArray);
		}
	}
	vec_free(&pending);
	g_moduleArrayCapacity = target;
	return true;
}

static void zeroSlotForAllTibs(size_t index) {
	if (index < kTlsSlotCount) {
		for (size_t i = 0; i < g_activeTibs.len; ++i) {
			g_activeTibs.data[i]->TlsSlots[index] = GUEST_NULL;
		}
		return;
	}
	size_t expansionIndex = index - kTlsSlotCount;
	for (size_t i = 0; i < g_activeTibs.len; ++i) {
		TlsArray *arr = getExpansionArray(g_activeTibs.data[i]);
		if (!arr || expansionIndex >= arr->capacity) {
			continue;
		}
		arr->slots[expansionIndex] = GUEST_NULL;
	}
}

static size_t indexOfActiveTibLocked(TEB *tib) {
	for (size_t i = 0; i < g_activeTibs.len; ++i) {
		if (g_activeTibs.data[i] == tib) {
			return i;
		}
	}
	return g_activeTibs.len;
}

void wibo_tls_initializeTib(TEB *tib) {
	if (!tib) {
		return;
	}
	pthread_mutex_lock(&g_tlsMutex);
	if (indexOfActiveTibLocked(tib) != g_activeTibs.len) {
		pthread_mutex_unlock(&g_tlsMutex);
		return;
	}
	vec_push(&g_activeTibs, tib);
	if (g_expansionCapacity > 0 && !getExpansionArray(tib)) {
		TlsArray *arr = allocateTlsArray(g_expansionCapacity);
		if (arr) {
			setExpansionArray(tib, arr);
		}
	}
	if (g_moduleArrayCapacity > 0 && !getModuleArray(tib)) {
		TlsArray *arr = allocateTlsArray(g_moduleArrayCapacity);
		if (arr) {
			hashmap_put(moduleArrayMap(), &tib, sizeof tib, arr);
			tib->ThreadLocalStoragePointer = toGuestPtr(arr->slots);
		} else {
			DEBUG_LOG("initializeTib: failed to allocate module TLS array for %p\n", tib);
		}
	}
	pthread_mutex_unlock(&g_tlsMutex);
}

void wibo_tls_cleanupTib(TEB *tib) {
	if (!tib) {
		return;
	}
	pthread_mutex_lock(&g_tlsMutex);
	TlsArray *expansion = getExpansionArray(tib);
	if (expansion) {
		wibo_heap_guestFree(expansion);
		setExpansionArray(tib, NULL);
	}
	TlsArray *module = getModuleArray(tib);
	if (module) {
		hashmap_remove(moduleArrayMap(), &tib, sizeof tib, NULL);
		wibo_heap_guestFree(module);
	}
	for (size_t i = 0; i < g_moduleGarbage.len;) {
		if (g_moduleGarbage.data[i].tib == tib) {
			wibo_heap_guestFree(g_moduleGarbage.data[i].array);
			vec_erase(&g_moduleGarbage, i);
		} else {
			++i;
		}
	}
	tib->ThreadLocalStoragePointer = GUEST_NULL;
	size_t index = indexOfActiveTibLocked(tib);
	if (index != g_activeTibs.len) {
		vec_erase(&g_activeTibs, index);
	}
	pthread_mutex_unlock(&g_tlsMutex);
}

DWORD wibo_tls_reserveSlot(void) {
	pthread_mutex_lock(&g_tlsMutex);
	for (DWORD index = 0; index < (DWORD)kTlsMaxSlotCount; ++index) {
		if (g_slotUsed[index]) {
			continue;
		}
		if (index >= (DWORD)kTlsSlotCount) {
			size_t required = (size_t)index - kTlsSlotCount + 1;
			if (!ensureGlobalExpansionCapacityLocked(required)) {
				pthread_mutex_unlock(&g_tlsMutex);
				return kInvalidTlsIndex;
			}
		}
		g_slotUsed[index] = true;
		zeroSlotForAllTibs(index);
		pthread_mutex_unlock(&g_tlsMutex);
		return index;
	}
	pthread_mutex_unlock(&g_tlsMutex);
	return kInvalidTlsIndex;
}

bool wibo_tls_releaseSlot(DWORD index) {
	if (index >= (DWORD)kTlsMaxSlotCount) {
		return false;
	}
	pthread_mutex_lock(&g_tlsMutex);
	bool released = g_slotUsed[index];
	if (released) {
		g_slotUsed[index] = false;
		zeroSlotForAllTibs(index);
	}
	pthread_mutex_unlock(&g_tlsMutex);
	return released;
}

bool wibo_tls_isSlotAllocated(DWORD index) {
	pthread_mutex_lock(&g_tlsMutex);
	bool allocated = index < (DWORD)kTlsMaxSlotCount && g_slotUsed[index];
	pthread_mutex_unlock(&g_tlsMutex);
	return allocated;
}

GUEST_PTR wibo_tls_getTibValue(TEB *tib, DWORD index) {
	if (!tib || index >= (DWORD)kTlsMaxSlotCount) {
		return GUEST_NULL;
	}
	if (index < (DWORD)kTlsSlotCount) {
		return tib->TlsSlots[index];
	}
	pthread_mutex_lock(&g_tlsMutex);
	GUEST_PTR value = GUEST_NULL;
	TlsArray *arr = getExpansionArray(tib);
	size_t expansionIndex = (size_t)index - kTlsSlotCount;
	if (arr && expansionIndex < arr->capacity) {
		value = arr->slots[expansionIndex];
	}
	pthread_mutex_unlock(&g_tlsMutex);
	return value;
}

bool wibo_tls_setTibValue(TEB *tib, DWORD index, GUEST_PTR value) {
	if (!tib || index >= (DWORD)kTlsMaxSlotCount) {
		return false;
	}
	if (index < (DWORD)kTlsSlotCount) {
		tib->TlsSlots[index] = value;
		return true;
	}
	pthread_mutex_lock(&g_tlsMutex);
	size_t expansionIndex = (size_t)index - kTlsSlotCount;
	TlsArray *arr = getExpansionArray(tib);
	if ((!arr || expansionIndex >= arr->capacity) && !ensureGlobalExpansionCapacityLocked(expansionIndex + 1)) {
		pthread_mutex_unlock(&g_tlsMutex);
		return false;
	}
	arr = getExpansionArray(tib);
	if (!arr || expansionIndex >= arr->capacity) {
		pthread_mutex_unlock(&g_tlsMutex);
		return false;
	}
	arr->slots[expansionIndex] = value;
	pthread_mutex_unlock(&g_tlsMutex);
	return true;
}

GUEST_PTR wibo_tls_getValue(DWORD index) { return wibo_tls_getTibValue(currentThreadTeb, index); }

bool wibo_tls_setValue(DWORD index, GUEST_PTR value) { return wibo_tls_setTibValue(currentThreadTeb, index, value); }

void wibo_tls_forEachTib(void (*callback)(TEB *, void *), void *context) {
	if (!callback) {
		return;
	}
	TebVec tibs = {0};
	pthread_mutex_lock(&g_tlsMutex);
	if (g_activeTibs.len) {
		vec_reserve(&tibs, g_activeTibs.len);
		memcpy(tibs.data, g_activeTibs.data, g_activeTibs.len * sizeof *g_activeTibs.data);
		tibs.len = g_activeTibs.len;
	}
	pthread_mutex_unlock(&g_tlsMutex);
	for (size_t i = 0; i < tibs.len; ++i) {
		callback(tibs.data[i], context);
	}
	vec_free(&tibs);
}

bool wibo_tls_ensureModulePointerCapacity(size_t capacity) {
	pthread_mutex_lock(&g_tlsMutex);
	bool ok = ensureModuleArrayCapacityLocked(capacity);
	pthread_mutex_unlock(&g_tlsMutex);
	return ok;
}

bool wibo_tls_setModulePointer(TEB *tib, size_t index, GUEST_PTR value) {
	if (!tib) {
		return false;
	}
	pthread_mutex_lock(&g_tlsMutex);
	if (!ensureModuleArrayCapacityLocked(index + 1)) {
		pthread_mutex_unlock(&g_tlsMutex);
		return false;
	}
	TlsArray *array = getModuleArray(tib);
	if (!array || index >= array->capacity) {
		pthread_mutex_unlock(&g_tlsMutex);
		return false;
	}
	array->slots[index] = value;
	tib->ThreadLocalStoragePointer = toGuestPtr(array->slots);
	pthread_mutex_unlock(&g_tlsMutex);
	return true;
}

void wibo_tls_clearModulePointer(TEB *tib, size_t index) {
	if (!tib) {
		return;
	}
	pthread_mutex_lock(&g_tlsMutex);
	TlsArray *array = getModuleArray(tib);
	if (array && index < array->capacity) {
		array->slots[index] = GUEST_NULL;
	}
	pthread_mutex_unlock(&g_tlsMutex);
}
