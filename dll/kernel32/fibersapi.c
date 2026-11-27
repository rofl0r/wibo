#include "fibersapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"

#include <pthread.h>

#define kMaxFlsValues ((DWORD)0x100)

typedef struct {
	DWORD generation;
	LPVOID value;
} FlsSlot;

static pthread_mutex_t g_flsMutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_flsValuesUsed[kMaxFlsValues];
static DWORD g_flsGenerations[kMaxFlsValues];
static __thread FlsSlot t_flsValues[kMaxFlsValues];

static void resetThreadFlsSlot(DWORD index) {
	FlsSlot *slot = &t_flsValues[index];
	if (slot->generation != g_flsGenerations[index]) {
		slot->generation = g_flsGenerations[index];
		slot->value = NULL;
	}
}

DWORD WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FlsAlloc(%p)", lpCallback);
	// If the function succeeds, the return value is an FLS index initialized to zero.
	pthread_mutex_lock(&g_flsMutex);
	for (DWORD i = 0; i < kMaxFlsValues; i++) {
		if (g_flsValuesUsed[i] == false) {
			g_flsValuesUsed[i] = true;
			g_flsGenerations[i]++;
			resetThreadFlsSlot(i);
			pthread_mutex_unlock(&g_flsMutex);
			DEBUG_LOG(" -> %d\n", i);
			return i;
		}
	}
	pthread_mutex_unlock(&g_flsMutex);
	DEBUG_LOG(" -> -1\n");
	kernel32_setLastError(FLS_OUT_OF_INDEXES);
	return FLS_OUT_OF_INDEXES;
}

BOOL WINAPI FlsFree(DWORD dwFlsIndex) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FlsFree(%u)\n", dwFlsIndex);
	pthread_mutex_lock(&g_flsMutex);
	if (dwFlsIndex < kMaxFlsValues && g_flsValuesUsed[dwFlsIndex]) {
		g_flsValuesUsed[dwFlsIndex] = false;
		pthread_mutex_unlock(&g_flsMutex);
		return TRUE;
	} else {
		pthread_mutex_unlock(&g_flsMutex);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
}

PVOID WINAPI FlsGetValue(DWORD dwFlsIndex) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("FlsGetValue(%u)\n", dwFlsIndex);
	PVOID result = NULL;
	pthread_mutex_lock(&g_flsMutex);
	if (dwFlsIndex < kMaxFlsValues && g_flsValuesUsed[dwFlsIndex]) {
		resetThreadFlsSlot(dwFlsIndex);
		result = t_flsValues[dwFlsIndex].value;
		pthread_mutex_unlock(&g_flsMutex);
		// See https://learn.microsoft.com/en-us/windows/win32/api/fibersapi/nf-fibersapi-flsgetvalue
		kernel32_setLastError(ERROR_SUCCESS);
	} else {
		pthread_mutex_unlock(&g_flsMutex);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
	}
	// DEBUG_LOG(" -> %p\n", result);
	return result;
}

BOOL WINAPI FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("FlsSetValue(%u, %p)\n", dwFlsIndex, lpFlsData);
	pthread_mutex_lock(&g_flsMutex);
	if (dwFlsIndex < kMaxFlsValues && g_flsValuesUsed[dwFlsIndex]) {
		resetThreadFlsSlot(dwFlsIndex);
		t_flsValues[dwFlsIndex].value = lpFlsData;
		pthread_mutex_unlock(&g_flsMutex);
		return TRUE;
	} else {
		pthread_mutex_unlock(&g_flsMutex);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
}
