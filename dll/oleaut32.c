#include "oleaut32.h"

#include "common.h"
#include "context.h"
#include "heap.h"
#include "modules.h"

#include <string.h>

// Returns a string pointer, The preceding uint contains the length in bytes (excluding NUL terminator).
GUEST_PTR WINAPI SysAllocString(LPCWSTR psz) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SysAllocString(%p)\n", psz);
	if (!psz) {
		return GUEST_NULL;
	}
	size_t len = 0;
	while (psz[len]) {
		++len;
	}
	uint8_t *block = (uint8_t *)wibo_heap_guestMalloc(4 + (len + 1) * sizeof(uint16_t), false);
	if (!block) {
		return GUEST_NULL;
	}
	uint32_t bytes = (uint32_t)(len * sizeof(uint16_t));
	memcpy(block, &bytes, 4);
	memcpy(block + 4, psz, (len + 1) * sizeof(uint16_t));
	return toGuestPtr(block + 4);
}

void WINAPI SysFreeString(LPWSTR bstrString) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SysFreeString(%p)\n", bstrString);
	if (bstrString) {
		wibo_heap_guestFree((uint8_t *)bstrString - 4);
	}
}

UINT WINAPI SysStringLen(LPCWSTR pbstr) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SysStringLen(%p)\n", pbstr);
	if (!pbstr) {
		return 0;
	}
	uint32_t bytes;
	memcpy(&bytes, (const uint8_t *)pbstr - 4, 4);
	return bytes / sizeof(uint16_t);
}

#include "oleaut32_trampolines.h"

static void *resolveByOrdinal(uint16_t ordinal) {
	switch (ordinal) {
	case 2:
		return (void *)thunk_oleaut32_SysAllocString;
	case 6:
		return (void *)thunk_oleaut32_SysFreeString;
	case 7:
		return (void *)thunk_oleaut32_SysStringLen;
	}
	return NULL;
}

const ModuleStub lib_oleaut32 = {
	.names =
		(const char *[]){
			"oleaut32",
			NULL,
		},
	.byName = oleaut32ThunkByName,
	.byOrdinal = resolveByOrdinal,
};
