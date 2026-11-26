#include "unwind.h"

#include "cutil.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct WiboFunctionTable {
	uint64_t imageBase;
	uint32_t imageSize;
	size_t count;
	WiboRuntimeFunction *entries;
};

typedef VEC(WiboFunctionTable *) FunctionTableVec;

static FunctionTableVec g_tables;
static pthread_mutex_t g_tablesMutex = PTHREAD_MUTEX_INITIALIZER;

static int compareRuntimeFunctions(const void *a, const void *b) {
	const WiboRuntimeFunction *fa = a;
	const WiboRuntimeFunction *fb = b;
	if (fa->beginAddress != fb->beginAddress) {
		return fa->beginAddress < fb->beginAddress ? -1 : 1;
	}
	return 0;
}

WiboFunctionTable *wibo_registerFunctionTable(const void *imageBase, size_t imageSize,
											  const WiboRuntimeFunction *entries, size_t count) {
	if (!imageBase || !entries || count == 0 || imageSize > UINT32_MAX) {
		return NULL;
	}
	WiboFunctionTable *table = xcalloc(1, sizeof(*table));
	table->imageBase = (uint64_t)(uintptr_t)imageBase;
	table->imageSize = (uint32_t)imageSize;
	table->count = count;
	table->entries = xmalloc(count * sizeof(*table->entries));
	memcpy(table->entries, entries, count * sizeof(*table->entries));
	qsort(table->entries, count, sizeof(*table->entries), compareRuntimeFunctions);

	pthread_mutex_lock(&g_tablesMutex);
	vec_push(&g_tables, table);
	pthread_mutex_unlock(&g_tablesMutex);
	return table;
}

void wibo_unregisterFunctionTable(WiboFunctionTable *table) {
	if (!table) {
		return;
	}
	pthread_mutex_lock(&g_tablesMutex);
	for (size_t i = 0; i < g_tables.len; i++) {
		if (g_tables.data[i] == table) {
			vec_erase(&g_tables, i);
			break;
		}
	}
	pthread_mutex_unlock(&g_tablesMutex);
	free(table->entries);
	free(table);
}

static bool lookupInTable(const WiboFunctionTable *table, uint32_t rva, WiboRuntimeFunction *out) {
	size_t lo = 0;
	size_t hi = table->count;
	// Find the last entry whose beginAddress is <= rva.
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (table->entries[mid].beginAddress <= rva) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo == 0) {
		return false;
	}
	const WiboRuntimeFunction *candidate = &table->entries[lo - 1];
	if (rva >= candidate->endAddress) {
		return false;
	}
	*out = *candidate;
	return true;
}

bool wibo_lookupRuntimeFunction(uint64_t pc, uint64_t *imageBase, WiboRuntimeFunction *out) {
	bool found = false;
	pthread_mutex_lock(&g_tablesMutex);
	for (size_t i = 0; i < g_tables.len && !found; i++) {
		const WiboFunctionTable *table = g_tables.data[i];
		if (pc < table->imageBase || pc - table->imageBase >= table->imageSize) {
			continue;
		}
		uint32_t rva = (uint32_t)(pc - table->imageBase);
		if (lookupInTable(table, rva, out)) {
			*imageBase = table->imageBase;
			found = true;
		}
	}
	pthread_mutex_unlock(&g_tablesMutex);
	return found;
}
