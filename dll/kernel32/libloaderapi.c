#include "libloaderapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "files.h"
#include "internal.h"
#include "modules.h"
#include "resources.h"
#include "strutil.h"
#include "types.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define GET_MODULE_HANDLE_EX_FLAG_PIN 0x00000001
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0x00000002
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 0x00000004
#define GET_MODULE_HANDLE_EX_VALID_FLAGS                                                                               \
	(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)

static HRSRC findResourceInternal(HMODULE hModule, const ResourceIdentifier *type, const ResourceIdentifier *name,
								  const uint16_t *language) {
	Executable *exe = wibo_executableFromModule(hModule);
	if (!exe) {
		kernel32_setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return NO_HANDLE;
	}
	ResourceLocation loc;
	if (!Executable_findResource(exe, type, name, language, &loc)) {
		return NO_HANDLE;
	}
	return toGuestPtr(loc.dataEntry);
}

// Returns a malloc'd Windows path for the module, falling back to the name it was loaded with.
static char *moduleWindowsPath(const ModuleInfo *info) {
	if (info->resolvedPath) {
		return files_pathToWindows(info->resolvedPath);
	}
	return xstrdup(info->originalName ? info->originalName : "");
}

BOOL WINAPI DisableThreadLibraryCalls(HMODULE hLibModule) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DisableThreadLibraryCalls(%p)\n", hLibModule);
	if (!hLibModule) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	ModuleInfo *info = wibo_moduleInfoFromHandle(hLibModule);
	if (!info) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!wibo_disableThreadNotifications(info)) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleHandleA(%s)\n", lpModuleName);
	const ModuleInfo *module = wibo_findLoadedModule(lpModuleName);
	if (!module) {
		kernel32_setLastError(ERROR_MOD_NOT_FOUND);
		return NO_HANDLE;
	}
	return module->handle;
}

HMODULE WINAPI GetModuleHandleW(LPCWSTR lpModuleName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleHandleW -> ");
	if (lpModuleName) {
		char *lpModuleNameA = wideStringToString(lpModuleName, -1);
		HMODULE result = GetModuleHandleA(lpModuleNameA);
		free(lpModuleNameA);
		return result;
	}
	return GetModuleHandleA(NULL);
}

BOOL WINAPI GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE *phModule) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleHandleExW(%x, %p, %p)\n", dwFlags, lpModuleName, phModule);
	if (!phModule) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*phModule = NO_HANDLE;
	if ((dwFlags & ~GET_MODULE_HANDLE_EX_VALID_FLAGS) != 0 ||
		((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) && (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	ModuleInfo *module = NULL;
	if (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
		module = wibo_moduleInfoFromAddress((void *)lpModuleName);
	} else if (lpModuleName) {
		char *lpModuleNameA = wideStringToString(lpModuleName, -1);
		module = wibo_findLoadedModule(lpModuleNameA);
		free(lpModuleNameA);
	} else {
		module = wibo_findLoadedModule(NULL);
	}

	if (!module) {
		kernel32_setLastError(ERROR_MOD_NOT_FOUND);
		return FALSE;
	}
	if (dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) {
		module->refCount = UINT_MAX;
	} else if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) && module->refCount != UINT_MAX) {
		module->refCount++;
	}
	*phModule = module->handle;
	return TRUE;
}

DWORD WINAPI GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleFileNameA(%p, %p, %u)\n", hModule, lpFilename, nSize);
	if (!lpFilename) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	ModuleInfo *info = wibo_moduleInfoFromHandle(hModule);
	if (!info) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *path = moduleWindowsPath(info);
	DEBUG_LOG("-> %s\n", path);
	if (nSize == 0) {
		free(path);
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	const size_t len = strlen(path);
	const size_t copyLen = len < (size_t)(nSize - 1) ? len : (size_t)(nSize - 1);
	memcpy(lpFilename, path, copyLen);
	lpFilename[copyLen] = '\0';
	free(path);
	if (copyLen < len) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return nSize;
	}
	return (DWORD)copyLen;
}

DWORD WINAPI GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleFileNameW(%p, %p, %u)\n", hModule, lpFilename, nSize);
	if (!lpFilename) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	ModuleInfo *info = wibo_moduleInfoFromHandle(hModule);
	if (!info) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *path = moduleWindowsPath(info);
	if (nSize == 0) {
		free(path);
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	uint16_t *wide = stringToWideString(path, SIZE_MAX);
	free(path);
	const size_t len = wstrlen(wide);
	const size_t copyLen = len < (size_t)(nSize - 1) ? len : (size_t)(nSize - 1);
	for (size_t i = 0; i < copyLen; ++i) {
		lpFilename[i] = wide[i];
	}
	lpFilename[copyLen] = 0;
	free(wide);
	if (copyLen < len) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return nSize;
	}
	return (DWORD)copyLen;
}

HRSRC WINAPI FindResourceA(HMODULE hModule, LPCSTR lpName, LPCSTR lpType) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindResourceA(%p, %s, %s)\n", hModule, lpName, lpType);
	ResourceIdentifier type = wibo_resourceIdentifierFromAnsi(lpType);
	ResourceIdentifier name = wibo_resourceIdentifierFromAnsi(lpName);
	HRSRC result = findResourceInternal(hModule, &type, &name, NULL);
	ResourceIdentifier_free(&type);
	ResourceIdentifier_free(&name);
	return result;
}

HRSRC WINAPI FindResourceExA(HMODULE hModule, LPCSTR lpType, LPCSTR lpName, WORD wLanguage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindResourceExA(%p, %s, %s, %u)\n", hModule, lpName, lpType, wLanguage);
	ResourceIdentifier type = wibo_resourceIdentifierFromAnsi(lpType);
	ResourceIdentifier name = wibo_resourceIdentifierFromAnsi(lpName);
	uint16_t language = wLanguage;
	HRSRC result = findResourceInternal(hModule, &type, &name, &language);
	ResourceIdentifier_free(&type);
	ResourceIdentifier_free(&name);
	return result;
}

HRSRC WINAPI FindResourceW(HMODULE hModule, LPCWSTR lpName, LPCWSTR lpType) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindResourceW(%p, %p, %p)\n", hModule, lpName, lpType);
	ResourceIdentifier type = wibo_resourceIdentifierFromWide(lpType);
	ResourceIdentifier name = wibo_resourceIdentifierFromWide(lpName);
	HRSRC result = findResourceInternal(hModule, &type, &name, NULL);
	ResourceIdentifier_free(&type);
	ResourceIdentifier_free(&name);
	return result;
}

HRSRC WINAPI FindResourceExW(HMODULE hModule, LPCWSTR lpType, LPCWSTR lpName, WORD wLanguage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindResourceExW(%p, %p, %p, %u)\n", hModule, lpName, lpType, wLanguage);
	ResourceIdentifier type = wibo_resourceIdentifierFromWide(lpType);
	ResourceIdentifier name = wibo_resourceIdentifierFromWide(lpName);
	uint16_t language = wLanguage;
	HRSRC result = findResourceInternal(hModule, &type, &name, &language);
	ResourceIdentifier_free(&type);
	ResourceIdentifier_free(&name);
	return result;
}

HGLOBAL WINAPI LoadResource(HMODULE hModule, HRSRC hResInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadResource(%p, %p)\n", hModule, hResInfo);
	if (!hResInfo) {
		kernel32_setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return GUEST_NULL;
	}
	Executable *exe = wibo_executableFromModule(hModule);
	if (!exe || !exe->rsrcBase) {
		kernel32_setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return GUEST_NULL;
	}
	const ImageResourceDataEntry *entry = fromGuestPtr(hResInfo);
	if (!wibo_resourceEntryBelongsToExecutable(exe, entry)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return GUEST_NULL;
	}
	return toGuestPtr(Executable_fromRVA(exe, entry->offsetToData));
}

LPVOID WINAPI LockResource(HGLOBAL hResData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: LockResource(%p)\n", hResData);
	return fromGuestPtr(hResData);
}

DWORD WINAPI SizeofResource(HMODULE hModule, HRSRC hResInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SizeofResource(%p, %p)\n", hModule, hResInfo);
	if (!hResInfo) {
		kernel32_setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return 0;
	}
	Executable *exe = wibo_executableFromModule(hModule);
	if (!exe || !exe->rsrcBase) {
		kernel32_setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return 0;
	}
	const ImageResourceDataEntry *entry = fromGuestPtr(hResInfo);
	if (!wibo_resourceEntryBelongsToExecutable(exe, entry)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	return entry->size;
}

HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadLibraryA(%s)\n", lpLibFileName);
	const ModuleInfo *info = wibo_loadModule(lpLibFileName);
	if (!info) {
		// lastError is set by loadModule
		return NO_HANDLE;
	}
	return info->handle;
}

HMODULE WINAPI LoadLibraryW(LPCWSTR lpLibFileName) {
	HOST_CONTEXT_GUARD();
	if (!lpLibFileName) {
		return NO_HANDLE;
	}
	char *filename = wideStringToString(lpLibFileName, -1);
	DEBUG_LOG("LoadLibraryW(%s)\n", filename);
	HMODULE result = LoadLibraryA(filename);
	free(filename);
	return result;
}

HMODULE WINAPI LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	(void)hFile;
	// TODO: handle dwFlags properly
	DEBUG_LOG("LoadLibraryExA(%s, %x)\n", lpLibFileName, dwFlags);
	return LoadLibraryA(lpLibFileName);
}

HMODULE WINAPI LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	(void)hFile;
	// TOOD: handle dwFlags properly
	DEBUG_LOG("LoadLibraryExW(%x) -> ", dwFlags);
	char *filename = wideStringToString(lpLibFileName, -1);
	HMODULE result = LoadLibraryA(filename);
	free(filename);
	return result;
}

BOOL WINAPI FreeLibrary(HMODULE hLibModule) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FreeLibrary(%p)\n", hLibModule);
	ModuleInfo *info = wibo_moduleInfoFromHandle(hLibModule);
	if (!info) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	wibo_freeModule(info);
	return TRUE;
}

FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
	HOST_CONTEXT_GUARD();
	FARPROC result;
	ModuleInfo *info = wibo_moduleInfoFromHandle(hModule);
	if (!info) {
		DEBUG_LOG("GetProcAddress(%p) -> ERROR_INVALID_HANDLE\n", hModule);
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	const uintptr_t proc = (uintptr_t)lpProcName;
	if (proc & ~0xFFFFu) {
		DEBUG_LOG("GetProcAddress(%s, %s) ", info->normalizedName, lpProcName);
		result = wibo_findExportByName(info, lpProcName);
	} else {
		DEBUG_LOG("GetProcAddress(%s, %u) ", info->normalizedName, proc);
		result = wibo_findExportByOrdinal(info, (uint16_t)proc);
	}
	DEBUG_LOG("-> %p\n", result);
	if (!result) {
		kernel32_setLastError(ERROR_PROC_NOT_FOUND);
	}
	return result;
}
