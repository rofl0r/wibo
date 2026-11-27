#include "winreg.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "kernel32/internal.h"
#include "strutil.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef VEC(uint16_t) U16Vec;

typedef struct RegistryKeyObject {
	ObjectBase base;
	pthread_mutex_t m;
	U16Vec canonicalPath;
	bool closed;
	bool predefined;
} RegistryKeyObject;

typedef struct PredefinedKeyInfo {
	HKEY value;
	const uint16_t *name;
} PredefinedKeyInfo;

static const PredefinedKeyInfo kPredefinedKeyInfos[] = {
	{(HKEY)0x80000000u, u"HKEY_CLASSES_ROOT"},
	{(HKEY)0x80000001u, u"HKEY_CURRENT_USER"},
	{(HKEY)0x80000002u, u"HKEY_LOCAL_MACHINE"},
	{(HKEY)0x80000003u, u"HKEY_USERS"},
	{(HKEY)0x80000004u, u"HKEY_PERFORMANCE_DATA"},
	{(HKEY)0x80000005u, u"HKEY_CURRENT_CONFIG"},
};

#define kPredefinedKeyCount (sizeof(kPredefinedKeyInfos) / sizeof(kPredefinedKeyInfos[0]))

static pthread_mutex_t g_registryMutex = PTHREAD_MUTEX_INITIALIZER;
// Keys are UTF-16 canonical paths keyed by their raw bytes; values are unused.
static hashmap *g_existingKeys;
static RegistryKeyObject *g_predefinedHandles[kPredefinedKeyCount];
static bool g_predefinedHandlesInitialized;

static void RegistryKeyObject_destroy(ObjectBase *base) {
	RegistryKeyObject *self = (RegistryKeyObject *)base;
	pthread_mutex_destroy(&self->m);
	vec_free(&self->canonicalPath);
	free(self);
}

// Consumes canonicalPath.
static RegistryKeyObject *RegistryKeyObject_alloc(U16Vec canonicalPath) {
	RegistryKeyObject *self = xcalloc(1, sizeof(*self));
	ObjectBase_init(&self->base, ObjectType_RegistryKey, RegistryKeyObject_destroy);
	pthread_mutex_init(&self->m, NULL);
	self->canonicalPath = canonicalPath;
	return self;
}

static void u16VecAppend(U16Vec *dst, const uint16_t *src, size_t len) {
	if (len == 0) {
		return;
	}
	vec_reserve(dst, len);
	memcpy(dst->data + dst->len, src, len * sizeof(uint16_t));
	dst->len += len;
}

static U16Vec u16VecCopy(const U16Vec *src) {
	U16Vec dst = {0};
	u16VecAppend(&dst, src->data, src->len);
	return dst;
}

static U16Vec canonicalizeKeySegment(const uint16_t *input, size_t len) {
	U16Vec result = {0};
	vec_reserve(&result, len);
	bool lastWasSlash = false;
	for (size_t i = 0; i < len; ++i) {
		uint16_t normalized = (input[i] == u'/') ? u'\\' : input[i];
		if (normalized == u'\\') {
			if (result.len != 0 && !lastWasSlash) {
				vec_push(&result, u'\\');
			}
			lastWasSlash = true;
			continue;
		}
		lastWasSlash = false;
		vec_push(&result, wcharToLower(normalized));
	}
	while (result.len != 0 && result.data[result.len - 1] == u'\\') {
		result.len--;
	}
	return result;
}

static hashmap *existingKeysLocked(void) {
	if (!g_existingKeys) {
		g_existingKeys = hashmap_create();
	}
	return g_existingKeys;
}

// Must be called with g_registryMutex held.
static void predefinedHandlesInitLocked(void) {
	for (size_t i = 0; i < kPredefinedKeyCount; ++i) {
		const uint16_t *name = kPredefinedKeyInfos[i].name;
		RegistryKeyObject *obj = RegistryKeyObject_alloc(canonicalizeKeySegment(name, wstrlen(name)));
		obj->predefined = true;
		g_predefinedHandles[i] = obj;
	}
	g_predefinedHandlesInitialized = true;
}

// Returns a new reference, or NULL if value is not a predefined key.
static RegistryKeyObject *predefinedHandleForValue(HKEY value) {
	if (!g_predefinedHandlesInitialized) {
		predefinedHandlesInitLocked();
	}
	for (size_t i = 0; i < kPredefinedKeyCount; ++i) {
		if (kPredefinedKeyInfos[i].value == value) {
			ObjectBase_ref(&g_predefinedHandles[i]->base);
			return g_predefinedHandles[i];
		}
	}
	return NULL;
}

// Returns a new reference that the caller must unref, or NULL.
static RegistryKeyObject *handleDataFromHKeyLocked(HKEY hKey) {
	if (hKey == NO_HANDLE) {
		return NULL;
	}
	RegistryKeyObject *predefined = predefinedHandleForValue(hKey);
	if (predefined) {
		return predefined;
	}
	RegistryKeyObject *obj = (RegistryKeyObject *)Handles_getAs(wibo_handles(), hKey, ObjectType_RegistryKey, NULL);
	if (!obj) {
		return NULL;
	}
	if (obj->closed) {
		ObjectBase_unref(&obj->base);
		return NULL;
	}
	return obj;
}

static bool isPredefinedKeyHandle(HKEY hKey) {
	for (size_t i = 0; i < kPredefinedKeyCount; ++i) {
		if (kPredefinedKeyInfos[i].value == hKey) {
			return true;
		}
	}
	return false;
}

// Must be called with g_registryMutex held.
static LSTATUS createKeyLocked(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult, LPDWORD lpdwDisposition) {
	RegistryKeyObject *baseHandle = handleDataFromHKeyLocked(hKey);
	if (!baseHandle) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return ERROR_INVALID_HANDLE;
	}
	U16Vec targetPath = u16VecCopy(&baseHandle->canonicalPath);
	ObjectBase_unref(&baseHandle->base);
	bool targetingBase = true;
	if (lpSubKey && lpSubKey[0] != 0) {
		U16Vec subComponent = canonicalizeKeySegment(lpSubKey, wstrlen(lpSubKey));
		if (subComponent.len != 0) {
			targetingBase = false;
			if (targetPath.len != 0) {
				vec_push(&targetPath, u'\\');
			}
			u16VecAppend(&targetPath, subComponent.data, subComponent.len);
		}
		vec_free(&subComponent);
	}
	if (targetPath.len == 0) {
		vec_free(&targetPath);
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return ERROR_INVALID_HANDLE;
	}
	hashmap *existing = existingKeysLocked();
	size_t keyBytes = targetPath.len * sizeof(uint16_t);
	bool existed = hashmap_find(existing, targetPath.data, keyBytes, NULL);
	if (!existed) {
		hashmap_put(existing, targetPath.data, keyBytes, NULL);
	}
	if (lpdwDisposition) {
		*lpdwDisposition = existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY;
	}
	if (targetingBase) {
		vec_free(&targetPath);
		*phkResult = hKey;
		return ERROR_SUCCESS;
	}
	RegistryKeyObject *obj = RegistryKeyObject_alloc(targetPath);
	*phkResult = (HKEY)Handles_alloc(wibo_handles(), &obj->base, 0, 0);
	return ERROR_SUCCESS;
}

// Must be called with g_registryMutex held.
static LSTATUS openKeyLocked(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult) {
	RegistryKeyObject *baseHandle = handleDataFromHKeyLocked(hKey);
	if (!baseHandle) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return ERROR_INVALID_HANDLE;
	}
	U16Vec targetPath = u16VecCopy(&baseHandle->canonicalPath);
	bool basePredefined = baseHandle->predefined;
	ObjectBase_unref(&baseHandle->base);
	if (lpSubKey && lpSubKey[0] != 0) {
		U16Vec subComponent = canonicalizeKeySegment(lpSubKey, wstrlen(lpSubKey));
		if (subComponent.len != 0) {
			if (targetPath.len != 0) {
				vec_push(&targetPath, u'\\');
			}
			u16VecAppend(&targetPath, subComponent.data, subComponent.len);
		}
		vec_free(&subComponent);
	}
	if (targetPath.len == 0) {
		vec_free(&targetPath);
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return ERROR_INVALID_HANDLE;
	}
	if (!hashmap_find(existingKeysLocked(), targetPath.data, targetPath.len * sizeof(uint16_t), NULL)) {
		vec_free(&targetPath);
		kernel32_setLastError(ERROR_FILE_NOT_FOUND);
		return ERROR_FILE_NOT_FOUND;
	}
	if ((!lpSubKey || lpSubKey[0] == 0) && basePredefined) {
		vec_free(&targetPath);
		*phkResult = hKey;
		return ERROR_SUCCESS;
	}
	RegistryKeyObject *obj = RegistryKeyObject_alloc(targetPath);
	*phkResult = (HKEY)Handles_alloc(wibo_handles(), &obj->base, 0, 0);
	return ERROR_SUCCESS;
}

LSTATUS WINAPI RegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved, LPWSTR lpClass, DWORD dwOptions,
							   REGSAM samDesired, void *lpSecurityAttributes, PHKEY phkResult,
							   LPDWORD lpdwDisposition) {
	HOST_CONTEXT_GUARD();
	char *subKeyString = lpSubKey ? wideStringToString(lpSubKey, -1) : xstrdup("(null)");
	char *classString = lpClass ? wideStringToString(lpClass, -1) : xstrdup("(null)");
	DEBUG_LOG("RegCreateKeyExW(%p, %s, %u, %s, 0x%x, 0x%x, %p, %p, %p)\n", (void *)(uintptr_t)hKey, subKeyString,
			  Reserved, classString, dwOptions, samDesired, lpSecurityAttributes, phkResult, lpdwDisposition);
	free(subKeyString);
	free(classString);
	(void)lpClass;
	(void)lpSecurityAttributes;
	if (!phkResult) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	*phkResult = NO_HANDLE;
	if (Reserved != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	if (dwOptions != 0) {
		DEBUG_LOG("RegCreateKeyExW: unsupported options 0x%x\n", dwOptions);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	REGSAM sanitizedAccess = samDesired & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY);
	if (sanitizedAccess != samDesired) {
		DEBUG_LOG("RegCreateKeyExW: ignoring WOW64 access mask 0x%x\n", samDesired ^ sanitizedAccess);
	}
	pthread_mutex_lock(&g_registryMutex);
	LSTATUS status = createKeyLocked(hKey, lpSubKey, phkResult, lpdwDisposition);
	pthread_mutex_unlock(&g_registryMutex);
	return status;
}

LSTATUS WINAPI RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions,
							   REGSAM samDesired, void *lpSecurityAttributes, PHKEY phkResult,
							   LPDWORD lpdwDisposition) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegCreateKeyExA(%p, %s, %u, %s, 0x%x, 0x%x, %p, %p, %p)\n", (void *)(uintptr_t)hKey,
			  lpSubKey ? lpSubKey : "(null)", Reserved, lpClass ? lpClass : "(null)", dwOptions, samDesired,
			  lpSecurityAttributes, phkResult, lpdwDisposition);
	uint16_t *subKeyWide = lpSubKey ? stringToWideString(lpSubKey, SIZE_MAX) : NULL;
	uint16_t *classWide = lpClass ? stringToWideString(lpClass, SIZE_MAX) : NULL;
	LSTATUS status = RegCreateKeyExW(hKey, subKeyWide, Reserved, classWide, dwOptions, samDesired,
									 lpSecurityAttributes, phkResult, lpdwDisposition);
	free(subKeyWide);
	free(classWide);
	return status;
}

LSTATUS WINAPI RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
	HOST_CONTEXT_GUARD();
	char *subKeyString = lpSubKey ? wideStringToString(lpSubKey, -1) : xstrdup("(null)");
	DEBUG_LOG("RegOpenKeyExW(%p, %s, %u, 0x%x, %p)\n", (void *)(uintptr_t)hKey, subKeyString, ulOptions, samDesired,
			  phkResult);
	free(subKeyString);
	if (!phkResult) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	*phkResult = NO_HANDLE;
	if ((ulOptions & ~REG_OPTION_OPEN_LINK) != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	if (ulOptions & REG_OPTION_OPEN_LINK) {
		DEBUG_LOG("RegOpenKeyExW: ignoring REG_OPTION_OPEN_LINK\n");
	}
	REGSAM sanitizedAccess = samDesired & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY);
	if (sanitizedAccess != samDesired) {
		DEBUG_LOG("RegOpenKeyExW: ignoring WOW64 access mask 0x%x\n", samDesired ^ sanitizedAccess);
	}
	(void)sanitizedAccess;
	pthread_mutex_lock(&g_registryMutex);
	LSTATUS status = openKeyLocked(hKey, lpSubKey, phkResult);
	pthread_mutex_unlock(&g_registryMutex);
	return status;
}

LSTATUS WINAPI RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegOpenKeyExA(%p, %s, %u, 0x%x, %p)\n", (void *)(uintptr_t)hKey, lpSubKey ? lpSubKey : "(null)",
			  ulOptions, samDesired, phkResult);
	uint16_t *wide = lpSubKey ? stringToWideString(lpSubKey, SIZE_MAX) : NULL;
	LSTATUS status = RegOpenKeyExW(hKey, wide, ulOptions, samDesired, phkResult);
	free(wide);
	return status;
}

LSTATUS WINAPI RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, BYTE *lpData,
								LPDWORD lpcbData) {
	HOST_CONTEXT_GUARD();
	char *valueName = lpValueName ? wideStringToString(lpValueName, -1) : xstrdup("(default)");
	DEBUG_LOG("RegQueryValueExW(%p, %s, %p, %p, %p, %p)\n", (void *)(uintptr_t)hKey, valueName, lpReserved, lpType,
			  lpData, lpcbData);
	free(valueName);
	if (lpReserved) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	if (lpcbData) {
		*lpcbData = 0;
	}
	if (lpType) {
		*lpType = 0;
	}
	(void)hKey;
	(void)lpData;
	kernel32_setLastError(ERROR_FILE_NOT_FOUND);
	return ERROR_FILE_NOT_FOUND;
}

LSTATUS WINAPI RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, BYTE *lpData,
								LPDWORD lpcbData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegQueryValueExA(%p, %s, %p, %p, %p, %p)\n", (void *)(uintptr_t)hKey,
			  lpValueName ? lpValueName : "(null)", lpReserved, lpType, lpData, lpcbData);
	uint16_t *valueWide = lpValueName ? stringToWideString(lpValueName, SIZE_MAX) : NULL;
	LSTATUS status = RegQueryValueExW(hKey, valueWide, lpReserved, lpType, lpData, lpcbData);
	free(valueWide);
	return status;
}

LSTATUS WINAPI RegEnumKeyExW(HKEY hKey, DWORD dwIndex, LPWSTR lpName, LPDWORD lpcchName, LPDWORD lpReserved,
							 LPWSTR lpClass, LPDWORD lpcchClass, FILETIME *lpftLastWriteTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegEnumKeyExW(%p, %u, %p, %p, %p, %p, %p, %p)\n", (void *)(uintptr_t)hKey, dwIndex, lpName, lpcchName,
			  lpReserved, lpClass, lpcchClass, lpftLastWriteTime);
	(void)hKey;
	(void)dwIndex;
	if (lpReserved) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	if (lpcchName) {
		*lpcchName = 0;
	}
	if (lpName && lpcchName && *lpcchName > 0) {
		lpName[0] = 0;
	}
	if (lpClass && lpcchClass && *lpcchClass > 0) {
		lpClass[0] = 0;
	}
	if (lpcchClass) {
		*lpcchClass = 0;
	}
	(void)lpftLastWriteTime;
	kernel32_setLastError(ERROR_NO_MORE_ITEMS);
	return ERROR_NO_MORE_ITEMS;
}

LSTATUS WINAPI RegEnumKeyExA(HKEY hKey, DWORD dwIndex, LPSTR lpName, LPDWORD lpcchName, LPDWORD lpReserved,
							 LPSTR lpClass, LPDWORD lpcchClass, FILETIME *lpftLastWriteTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegEnumKeyExA(%p, %u, %p, %p, %p, %p, %p, %p)\n", (void *)(uintptr_t)hKey, dwIndex, lpName, lpcchName,
			  lpReserved, lpClass, lpcchClass, lpftLastWriteTime);
	(void)hKey;
	(void)dwIndex;
	if (lpReserved) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return ERROR_INVALID_PARAMETER;
	}
	if (lpcchName) {
		*lpcchName = 0;
	}
	if (lpName && lpcchName && *lpcchName > 0) {
		lpName[0] = '\0';
	}
	if (lpClass && lpcchClass && *lpcchClass > 0) {
		lpClass[0] = '\0';
	}
	if (lpcchClass) {
		*lpcchClass = 0;
	}
	(void)lpftLastWriteTime;
	kernel32_setLastError(ERROR_NO_MORE_ITEMS);
	return ERROR_NO_MORE_ITEMS;
}

LSTATUS WINAPI RegCloseKey(HKEY hKey) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegCloseKey(%p)\n", (void *)(uintptr_t)hKey);
	if (isPredefinedKeyHandle(hKey)) {
		return ERROR_SUCCESS;
	}
	RegistryKeyObject *obj = (RegistryKeyObject *)Handles_getAs(wibo_handles(), hKey, ObjectType_RegistryKey, NULL);
	bool valid = obj && !obj->closed;
	if (obj) {
		ObjectBase_unref(&obj->base);
	}
	if (!valid) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return ERROR_INVALID_HANDLE;
	}
	return ERROR_SUCCESS;
}
