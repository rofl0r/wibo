#include "user32.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "resources.h"
#include "strutil.h"

#include <stdio.h>

#define RT_STRING_ID 6
#define UOI_FLAGS 1
#define WSF_VISIBLE 0x0001

static const HKL kDefaultKeyboardLayout = 0x04090409;

typedef struct {
	BOOL fInherit;
	BOOL fReserved;
	DWORD dwFlags;
} USEROBJECTFLAGS;

// Returns a pointer to the length word of the string entry and stores its length, or NULL if not found.
static const uint16_t *findStringResource(HMODULE hInstance, UINT uID, uint16_t *lengthOut) {
	Executable *mod = wibo_executableFromModule(hInstance);
	if (!mod) {
		return NULL;
	}
	ResourceIdentifier type = ResourceIdentifier_fromID(RT_STRING_ID);
	ResourceIdentifier table = ResourceIdentifier_fromID((uID >> 4) + 1);
	ResourceLocation loc;
	bool found = Executable_findResource(mod, &type, &table, NULL, &loc);
	ResourceIdentifier_free(&table);
	ResourceIdentifier_free(&type);
	if (!found) {
		return NULL;
	}
	const uint16_t *cursor = (const uint16_t *)loc.data;
	const uint16_t *end = cursor + (loc.size / sizeof(uint16_t));
	unsigned int entryIndex = uID & 0x0Fu;
	for (unsigned int i = 0; i < entryIndex; ++i) {
		if (cursor >= end) {
			return NULL;
		}
		uint16_t length = *cursor++;
		if (cursor + length > end) {
			return NULL;
		}
		cursor += length;
	}
	if (cursor >= end) {
		return NULL;
	}
	uint16_t length = *cursor++;
	if (cursor + length > end) {
		return NULL;
	}
	*lengthOut = length;
	return cursor;
}

DWORD WINAPI CharLowerBuffA(LPSTR lpsz, DWORD cchLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CharLowerBuffA(%p, %u)\n", lpsz, cchLength);
	if (!lpsz) {
		return 0;
	}
	for (DWORD i = 0; i < cchLength; ++i) {
		lpsz[i] = (char)wcharToLower((unsigned char)lpsz[i]);
	}
	return cchLength;
}

DWORD WINAPI CharUpperBuffW(LPWSTR lpsz, DWORD cchLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CharUpperBuffW(%p, %u)\n", lpsz, cchLength);
	if (!lpsz) {
		return 0;
	}
	for (DWORD i = 0; i < cchLength; ++i) {
		lpsz[i] = wcharToUpper(lpsz[i]);
	}
	return cchLength;
}

LPSTR WINAPI CharNextA(LPCSTR lpsz) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CharNextA(%p)\n", lpsz);
	return (LPSTR)(*lpsz ? lpsz + 1 : lpsz);
}

int WINAPI LoadStringA(HMODULE hInstance, UINT uID, LPSTR lpBuffer, int cchBufferMax) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadStringA(%p, %u, %p, %d)\n", hInstance, uID, lpBuffer, cchBufferMax);
	if (!lpBuffer || cchBufferMax <= 0) {
		return 0;
	}
	uint16_t length = 0;
	const uint16_t *cursor = findStringResource(hInstance, uID, &length);
	if (!cursor) {
		return 0;
	}
	int copyLength = length;
	if (copyLength > cchBufferMax - 1) {
		copyLength = cchBufferMax - 1;
	}
	for (int i = 0; i < copyLength; ++i) {
		lpBuffer[i] = (char)(cursor[i] & 0xFF);
	}
	lpBuffer[copyLength] = 0;
	DEBUG_LOG("LoadStringA -> %.*s\n", copyLength, lpBuffer);
	return copyLength;
}

int WINAPI LoadStringW(HMODULE hInstance, UINT uID, LPWSTR lpBuffer, int cchBufferMax) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadStringW(%p, %u, %p, %d)\n", hInstance, uID, lpBuffer, cchBufferMax);
	uint16_t length = 0;
	const uint16_t *cursor = findStringResource(hInstance, uID, &length);
	if (!cursor) {
		return 0;
	}
	if (cchBufferMax == 0) {
		if (lpBuffer) {
			*(const uint16_t **)lpBuffer = cursor;
		}
		return length;
	}
	if (!lpBuffer || cchBufferMax <= 0) {
		return 0;
	}
	int copyLength = length;
	if (copyLength > cchBufferMax - 1) {
		copyLength = cchBufferMax - 1;
	}
	for (int i = 0; i < copyLength; ++i) {
		lpBuffer[i] = cursor[i];
	}
	lpBuffer[copyLength] = 0;
	DEBUG_LOG("LoadStringW -> length %d\n", copyLength);
	return copyLength;
}

int WINAPI MessageBoxA(HWND hwnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
	HOST_CONTEXT_GUARD();
	(void)hwnd;
	(void)uType;
	printf("MESSAGE BOX: [%s] %s\n", lpCaption, lpText);
	fflush(stdout);
	return 1;
}

HKL WINAPI GetKeyboardLayout(DWORD idThread) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetKeyboardLayout(%u)\n", idThread);
	(void)idThread;
	return kDefaultKeyboardLayout;
}

HWINSTA WINAPI GetProcessWindowStation(void) {
	DEBUG_LOG("STUB: GetProcessWindowStation()\n");
	return NO_HANDLE;
}

BOOL WINAPI GetUserObjectInformationA(HANDLE hObj, int nIndex, PVOID pvInfo, DWORD nLength, LPDWORD lpnLengthNeeded) {
	DEBUG_LOG("GetUserObjectInformationA(%p, %d, %p, %u, %p)\n", hObj, nIndex, pvInfo, nLength, lpnLengthNeeded);
	(void)hObj;

	if (lpnLengthNeeded) {
		*lpnLengthNeeded = sizeof(USEROBJECTFLAGS);
	}

	if (nIndex != UOI_FLAGS) {
		kernel32_setLastError(ERROR_CALL_NOT_IMPLEMENTED);
		return FALSE;
	}

	if (!pvInfo || nLength < sizeof(USEROBJECTFLAGS)) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}

	USEROBJECTFLAGS *flags = (USEROBJECTFLAGS *)pvInfo;
	flags->fInherit = FALSE;
	flags->fReserved = FALSE;
	flags->dwFlags = WSF_VISIBLE;
	return TRUE;
}

HWND WINAPI GetActiveWindow(void) {
	DEBUG_LOG("GetActiveWindow()\n");
	return NO_HANDLE;
}

#include "user32_trampolines.h"

const ModuleStub lib_user32 = {
	.names =
		(const char *[]){
			"user32",
			NULL,
		},
	.byName = user32ThunkByName,
};
