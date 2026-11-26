#include "winbase.h"

#include "common.h"
#include "context.h"
#include "cutil.h"
#include "errors.h"
#include "files.h"
#include "heap.h"
#include "internal.h"
#include "modules.h"
#include "strutil.h"
#include "types.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

#define GMEM_MOVEABLE ((UINT)0x0002)
#define GMEM_ZEROINIT ((UINT)0x0040)
#define GMEM_MODIFY ((UINT)0x0080)

#define LMEM_MOVEABLE ((UINT)0x0002)
#define LMEM_ZEROINIT ((UINT)0x0040)

#define kMinIntegerAtom ((ATOM)0x0001)
#define kMaxIntegerAtom ((ATOM)0xBFFF)
#define kMinStringAtom ((ATOM)0xC000)
#define kMaxStringAtom ((ATOM)0xFFFF)
#define kStringAtomRange ((unsigned int)(kMaxStringAtom - kMinStringAtom + 1))

static SIZE_T clampToSizeT(uint64_t value) {
	return value > (uint64_t)SIZE_MAX ? (SIZE_T)SIZE_MAX : (SIZE_T)value;
}

typedef struct {
	uint64_t totalPhys;
	uint64_t availPhys;
	uint64_t totalPageFile;
	uint64_t availPageFile;
} MemorySnapshot;

static bool queryHostMemory(MemorySnapshot *out) {
	memset(out, 0, sizeof *out);
#if defined(__linux__)
	struct sysinfo info = {0};
	if (sysinfo(&info) != 0) {
		return false;
	}
	const uint64_t unit = info.mem_unit ? (uint64_t)info.mem_unit : 1;
	out->totalPhys = (uint64_t)info.totalram * unit;
	out->availPhys = (uint64_t)info.freeram * unit;
	out->totalPageFile = ((uint64_t)info.totalram + (uint64_t)info.totalswap) * unit;
	out->availPageFile = ((uint64_t)info.freeram + (uint64_t)info.freeswap) * unit;
	if (info.bufferram > 0) {
		uint64_t buffers = (uint64_t)info.bufferram * unit;
		out->availPhys += buffers;
		out->availPageFile += buffers;
	}
	return true;
#elif defined(__APPLE__)
	uint64_t totalPhys = 0;
	size_t totalPhysSize = sizeof(totalPhys);
	if (sysctlbyname("hw.memsize", &totalPhys, &totalPhysSize, NULL, 0) != 0) {
		return false;
	}
	vm_size_t pageSize = 0;
	if (host_page_size(mach_host_self(), &pageSize) != KERN_SUCCESS || pageSize == 0) {
		return false;
	}
	vm_statistics64_data_t vmstat = {0};
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vmstat, &count) != KERN_SUCCESS) {
		return false;
	}
	uint64_t freePages = (uint64_t)vmstat.free_count + (uint64_t)vmstat.inactive_count +
						 (uint64_t)vmstat.speculative_count;
	out->totalPhys = totalPhys;
	out->availPhys = freePages * (uint64_t)pageSize;

	struct xsw_usage swap = {0};
	size_t swapSize = sizeof(swap);
	if (sysctlbyname("vm.swapusage", &swap, &swapSize, NULL, 0) == 0 && swapSize == sizeof(swap)) {
		out->totalPageFile = swap.xsu_total;
		out->availPageFile = swap.xsu_avail;
	} else {
		out->totalPageFile = totalPhys;
		out->availPageFile = out->availPhys;
	}
	return true;
#else
	return false;
#endif
}

typedef struct {
	uint16_t refCount;
	char *original;
} AtomData;

// Keys are normalized names (stringToAtom) or ATOM bytes (atomToData); values are ATOM or AtomData * respectively.
typedef struct {
	pthread_mutex_t mutex;
	hashmap *stringToAtom;
	hashmap *atomToData;
	ATOM nextStringAtom;
} AtomTable;

static pthread_once_t g_atomTableOnce = PTHREAD_ONCE_INIT;
static AtomTable g_atomTable = {PTHREAD_MUTEX_INITIALIZER, NULL, NULL, kMinStringAtom};

static void initAtomTable(void) {
	g_atomTable.stringToAtom = hashmap_create();
	g_atomTable.atomToData = hashmap_create();
}

static AtomTable *localAtomTable(void) {
	pthread_once(&g_atomTableOnce, initAtomTable);
	return &g_atomTable;
}

static ATOM allocateStringAtomLocked(AtomTable *table) {
	unsigned int startOffset = 0;
	unsigned int next = table->nextStringAtom;
	if (next >= kMinStringAtom && next <= kMaxStringAtom) {
		startOffset = next - kMinStringAtom;
	}
	for (unsigned int i = 0; i < kStringAtomRange; ++i) {
		unsigned int offset = (startOffset + i) % kStringAtomRange;
		ATOM candidate = (ATOM)(kMinStringAtom + offset);
		if (!hashmap_find(table->atomToData, &candidate, sizeof candidate, NULL)) {
			unsigned int nextAtom = candidate + 1;
			if (nextAtom > kMaxStringAtom) {
				nextAtom = kMinStringAtom;
			}
			table->nextStringAtom = (ATOM)nextAtom;
			return candidate;
		}
	}
	return 0;
}

static bool tryHandleIntegerAtomPointer(const void *ptr, ATOM *atomOut) {
	uintptr_t value = (uintptr_t)ptr;
	if ((value >> 16) != 0) {
		return false;
	}
	ATOM maybeAtom = (ATOM)(value & 0xFFFFu);
	if (maybeAtom < kMinIntegerAtom || maybeAtom > kMaxIntegerAtom) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		*atomOut = 0;
		return true;
	}
	*atomOut = maybeAtom;
	return true;
}

static ATOM findAtomByNormalizedKey(const char *normalizedKey) {
	AtomTable *table = localAtomTable();
	pthread_mutex_lock(&table->mutex);
	void *value = NULL;
	if (!hashmap_find(table->stringToAtom, normalizedKey, strlen(normalizedKey), &value)) {
		pthread_mutex_unlock(&table->mutex);
		kernel32_setLastError(ERROR_FILE_NOT_FOUND);
		return 0;
	}
	pthread_mutex_unlock(&table->mutex);
	return (ATOM)(uintptr_t)value;
}

static ATOM tryParseIntegerAtomString(const char *value, bool *handled) {
	*handled = false;
	if (value[0] != '#') {
		return 0;
	}
	char *end = NULL;
	unsigned long parsed = strtoul(value + 1, &end, 10);
	if (end == value + 1 || *end != '\0') {
		return 0;
	}
	*handled = true;
	if (parsed < kMinIntegerAtom || parsed > kMaxIntegerAtom) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	return (ATOM)parsed;
}

static ATOM findAtomByString(const char *value) {
	bool handledInteger = false;
	ATOM atom = tryParseIntegerAtomString(value, &handledInteger);
	if (handledInteger) {
		return atom;
	}
	char *normalized = stringToLower(value, strlen(value));
	atom = findAtomByNormalizedKey(normalized);
	free(normalized);
	return atom;
}

static ATOM addAtomByString(const char *value) {
	bool handledInteger = false;
	ATOM atom = tryParseIntegerAtomString(value, &handledInteger);
	if (handledInteger) {
		return atom;
	}
	size_t len = strlen(value);
	if (len == 0 || len > 255) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *normalized = stringToLower(value, len);
	size_t normalizedLen = strlen(normalized);
	AtomTable *table = localAtomTable();
	pthread_mutex_lock(&table->mutex);
	void *existing = NULL;
	if (hashmap_find(table->stringToAtom, normalized, normalizedLen, &existing)) {
		ATOM existingAtom = (ATOM)(uintptr_t)existing;
		AtomData *data = NULL;
		if (hashmap_find(table->atomToData, &existingAtom, sizeof existingAtom, (void **)&data) &&
			data->refCount < UINT16_MAX) {
			data->refCount++;
		}
		pthread_mutex_unlock(&table->mutex);
		free(normalized);
		return existingAtom;
	}
	ATOM newAtom = allocateStringAtomLocked(table);
	if (newAtom == 0) {
		pthread_mutex_unlock(&table->mutex);
		free(normalized);
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	AtomData *data = xcalloc(1, sizeof *data);
	data->refCount = 1;
	data->original = xstrdup(value);
	hashmap_put(table->stringToAtom, normalized, normalizedLen, (void *)(uintptr_t)newAtom);
	hashmap_put(table->atomToData, &newAtom, sizeof newAtom, data);
	pthread_mutex_unlock(&table->mutex);
	free(normalized);
	return newAtom;
}

static bool tryGetCurrentDirectoryPath(char **outPath) {
	char *cwd = getcwd(NULL, 0);
	if (!cwd) {
		kernel32_setLastError(wibo_winErrorFromErrno(errno));
		return false;
	}
	*outPath = files_pathToWindows(cwd);
	free(cwd);
	return true;
}

// Returns a malloc'd absolute host path, or NULL if the working directory cannot be read.
static char *makeAbsoluteHostPath(const char *path) {
	if (path[0] == '/') {
		return xstrdup(path);
	}
	char *cwd = getcwd(NULL, 0);
	if (!cwd) {
		return NULL;
	}
	Buf buf;
	Buf_init(&buf);
	Buf_appendstr(&buf, cwd);
	Buf_appendc(&buf, '/');
	Buf_appendstr(&buf, path);
	free(cwd);
	return Buf_cstr(&buf);
}

static bool computeLongWindowsPath(const char *inputPath, char **longPath) {
	size_t inputLen = strlen(inputPath);
	bool hasTrailingSlash = inputLen > 0 && (inputPath[inputLen - 1] == '\\' || inputPath[inputLen - 1] == '/');

	char *hostPath = files_pathFromWindows(inputPath);
	if (!hostPath || !*hostPath) {
		free(hostPath);
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		return false;
	}

	struct stat st;
	if (stat(hostPath, &st) != 0) {
		free(hostPath);
		kernel32_setLastError(ERROR_FILE_NOT_FOUND);
		return false;
	}

	char *winPath = files_pathToWindows(hostPath);
	free(hostPath);
	Buf buf;
	Buf_init(&buf);
	Buf_appendstr(&buf, winPath);
	free(winPath);
	if (hasTrailingSlash && buf.len > 0 && buf.data[buf.len - 1] != '\\') {
		Buf_appendc(&buf, '\\');
	}
	*longPath = Buf_cstr(&buf);
	return true;
}

// Strips the last component of an absolute path in place; returns false once the root is reached.
static bool stripLastPathComponent(char *path) {
	size_t len = strlen(path);
	while (len > 1 && path[len - 1] == '/') {
		len--;
	}
	if (len <= 1) {
		return false;
	}
	path[len] = '\0';
	char *slash = strrchr(path, '/');
	if (!slash) {
		return false;
	}
	if (slash == path) {
		path[1] = '\0';
	} else {
		*slash = '\0';
	}
	return true;
}

static bool resolveDiskFreeSpaceStat(const char *rootPathName, struct statvfs *outBuf, char **resolvedPath) {
	char *hostPath = NULL;
	if (rootPathName && *rootPathName) {
		hostPath = files_pathFromWindows(rootPathName);
	} else {
		hostPath = getcwd(NULL, 0);
	}
	if (!hostPath || !*hostPath) {
		free(hostPath);
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		return false;
	}

	char *queryPath = makeAbsoluteHostPath(hostPath);
	free(hostPath);
	if (!queryPath) {
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		return false;
	}
	while (true) {
		if (statvfs(queryPath, outBuf) == 0) {
			*resolvedPath = queryPath;
			return true;
		}

		int savedErrno = errno;
		if (savedErrno != ENOENT && savedErrno != ENOTDIR) {
			free(queryPath);
			kernel32_setLastError(wibo_winErrorFromErrno(savedErrno));
			return false;
		}

		if (!stripLastPathComponent(queryPath)) {
			free(queryPath);
			kernel32_setLastError(wibo_winErrorFromErrno(savedErrno));
			return false;
		}
	}
}

#define kComputerNameLength ((DWORD)8)
#define kComputerNameRequiredSize ((DWORD)(kComputerNameLength + 1))
static const char kComputerNameAnsi[] = "COMPNAME";
static const uint16_t kComputerNameWide[] = {u'C', u'O', u'M', u'P', u'N', u'A', u'M', u'E', 0};

typedef struct {
	char *nameLower;
	ACTIVATION_CONTEXT_DATA_DLL_REDIRECTION *dllData;
} DllRedirectionEntry;

typedef struct {
	VEC(DllRedirectionEntry) dllRedirections;
} ActivationContext;

static ActivationContext *g_builtinActCtx = NULL;
static pthread_once_t g_defaultActCtxOnce = PTHREAD_ONCE_INIT;

static ActivationContext *currentActivationContext(void) {
	if (!g_builtinActCtx) {
		g_builtinActCtx = wibo_heap_guestMalloc(sizeof *g_builtinActCtx, true);
	}
	return g_builtinActCtx;
}

static void addDefaultActivationDll(ActivationContext *ctx, const char *name) {
	char *lowerName = stringToLower(name, strlen(name));
	for (size_t i = 0; i < ctx->dllRedirections.len; ++i) {
		if (str_eq(ctx->dllRedirections.data[i].nameLower, lowerName)) {
			free(lowerName);
			return;
		}
	}

	DllRedirectionEntry entry;
	entry.nameLower = lowerName;
	entry.dllData = wibo_heap_guestMalloc(sizeof(ACTIVATION_CONTEXT_DATA_DLL_REDIRECTION), true);
	entry.dllData->Size = sizeof(ACTIVATION_CONTEXT_DATA_DLL_REDIRECTION);
	entry.dllData->Flags = ACTIVATION_CONTEXT_DATA_DLL_REDIRECTION_PATH_OMITS_ASSEMBLY_ROOT;
	entry.dllData->TotalPathLength = 0;
	entry.dllData->PathSegmentCount = 0;
	entry.dllData->PathSegmentOffset = 0;
	vec_push(&ctx->dllRedirections, entry);
}

static void addLoadedModuleActivationDll(const void *key, size_t keylen, void *value, void *ctx) {
	(void)key;
	(void)keylen;
	const ModuleInfo *module = value;
	if (!module->moduleStub) {
		addDefaultActivationDll(ctx, module->normalizedName);
	}
}

static void initDefaultActivationContext(void) {
	ActivationContext *ctx = currentActivationContext();
	addDefaultActivationDll(ctx, "msvcr80.dll");
	addDefaultActivationDll(ctx, "msvcp80.dll");
	hashmap *modules = wibo_allLoadedModules();
	hashmap_foreach(modules, addLoadedModuleActivationDll, ctx);
	hashmap_free(modules);
}

void ensureDefaultActivationContext(void) {
	pthread_once(&g_defaultActCtxOnce, initDefaultActivationContext);
}

LPSTR WINAPI lstrcpynA(LPSTR lpString1, LPCSTR lpString2, int iMaxLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcpynA(%p, %p, %d)\n", lpString1, lpString2, iMaxLength);

	UINT count = (UINT)iMaxLength;
	if (!lpString1) {
		return NULL;
	}
	if (!lpString2 && count > 1) {
		return NULL;
	}

	LPSTR destination = lpString1;
	LPCSTR source = lpString2;
	while (count > 1 && *source) {
		count--;
		*destination++ = *source++;
	}
	if (count) {
		*destination = '\0';
	}
	return lpString1;
}

ATOM WINAPI AddAtomA(LPCSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = 0;
	if (tryHandleIntegerAtomPointer(lpString, &atom)) {
		DEBUG_LOG("AddAtomA(int:%u)\n", atom);
		return atom;
	}
	DEBUG_LOG("AddAtomA(%s)\n", lpString ? lpString : "<null>");
	if (!lpString) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	size_t len = strnlen(lpString, 256);
	if (len == 0 || len >= 256) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *value = xstrndup(lpString, len);
	ATOM result = addAtomByString(value);
	free(value);
	DEBUG_LOG("AddAtomA -> %u (lastError=%u)\n", result, kernel32_getLastError());
	return result;
}

ATOM WINAPI AddAtomW(LPCWSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = 0;
	if (tryHandleIntegerAtomPointer(lpString, &atom)) {
		DEBUG_LOG("AddAtomW(int:%u)\n", atom);
		return atom;
	}
	if (!lpString) {
		DEBUG_LOG("AddAtomW(<null>)\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	size_t len = wstrnlen((const uint16_t *)lpString, 256);
	if (len == 0 || len >= 256) {
		DEBUG_LOG("AddAtomW(invalid length)\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *value = wideStringToString((const uint16_t *)lpString, (int)len);
	DEBUG_LOG("AddAtomW(%s)\n", value);
	ATOM result = addAtomByString(value);
	free(value);
	DEBUG_LOG("AddAtomW -> %u (lastError=%u)\n", result, kernel32_getLastError());
	return result;
}

ATOM WINAPI FindAtomA(LPCSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = 0;
	if (tryHandleIntegerAtomPointer(lpString, &atom)) {
		DEBUG_LOG("FindAtomA(int:%u)\n", atom);
		return atom;
	}
	DEBUG_LOG("FindAtomA(%s)\n", lpString ? lpString : "<null>");
	if (!lpString) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	size_t len = strnlen(lpString, 256);
	if (len == 0 || len >= 256) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *value = xstrndup(lpString, len);
	ATOM result = findAtomByString(value);
	free(value);
	DEBUG_LOG("FindAtomA -> %u (lastError=%u)\n", result, kernel32_getLastError());
	return result;
}

ATOM WINAPI FindAtomW(LPCWSTR lpString) {
	HOST_CONTEXT_GUARD();
	ATOM atom = 0;
	if (tryHandleIntegerAtomPointer(lpString, &atom)) {
		DEBUG_LOG("FindAtomW(int:%u)\n", atom);
		return atom;
	}
	if (!lpString) {
		DEBUG_LOG("FindAtomW(<null>)\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	size_t len = wstrnlen((const uint16_t *)lpString, 256);
	if (len == 0 || len >= 256) {
		DEBUG_LOG("FindAtomW(invalid length)\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *value = wideStringToString((const uint16_t *)lpString, (int)len);
	DEBUG_LOG("FindAtomW(%s)\n", value);
	ATOM result = findAtomByString(value);
	free(value);
	DEBUG_LOG("FindAtomW -> %u (lastError=%u)\n", result, kernel32_getLastError());
	return result;
}

// Returns a malloc'd name; integer atoms are rendered as "#<n>".
static char *atomNameForAtom(ATOM nAtom, bool *ok) {
	*ok = true;
	char buf[16];
	if (nAtom >= kMinIntegerAtom && nAtom <= kMaxIntegerAtom) {
		snprintf(buf, sizeof buf, "#%u", (unsigned int)nAtom);
		return xstrdup(buf);
	}
	AtomTable *table = localAtomTable();
	pthread_mutex_lock(&table->mutex);
	AtomData *data = NULL;
	if (!hashmap_find(table->atomToData, &nAtom, sizeof nAtom, (void **)&data)) {
		pthread_mutex_unlock(&table->mutex);
		*ok = false;
		return NULL;
	}
	char *value = xstrdup(data->original);
	pthread_mutex_unlock(&table->mutex);
	return value;
}

UINT WINAPI GetAtomNameA(ATOM nAtom, LPSTR lpBuffer, int nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetAtomNameA(%u, %p, %d)\n", nAtom, lpBuffer, nSize);
	if (!lpBuffer || nSize <= 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	bool ok = false;
	char *value = atomNameForAtom(nAtom, &ok);
	if (!ok) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return 0;
	}
	size_t valueLen = strlen(value);
	if (valueLen + 1 > (size_t)nSize) {
		free(value);
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	memcpy(lpBuffer, value, valueLen);
	lpBuffer[valueLen] = '\0';
	free(value);
	UINT written = (UINT)valueLen;
	DEBUG_LOG("GetAtomNameA -> %u (lastError=%u)\n", written, kernel32_getLastError());
	return written;
}

UINT WINAPI GetAtomNameW(ATOM nAtom, LPWSTR lpBuffer, int nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetAtomNameW(%u, %p, %d)\n", nAtom, lpBuffer, nSize);
	if (!lpBuffer || nSize <= 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	bool ok = false;
	char *narrow = atomNameForAtom(nAtom, &ok);
	if (!ok) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return 0;
	}
	size_t narrowLen = strlen(narrow);
	uint16_t *wide = stringToWideString(narrow, narrowLen);
	free(narrow);
	// Includes the terminating NUL.
	size_t needed = narrowLen + 1;
	if (needed > (size_t)nSize) {
		free(wide);
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	memcpy(lpBuffer, wide, needed * sizeof(uint16_t));
	free(wide);
	UINT written = (UINT)narrowLen;
	DEBUG_LOG("GetAtomNameW -> %u (lastError=%u)\n", written, kernel32_getLastError());
	return written;
}

UINT WINAPI SetHandleCount(UINT uNumber) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetHandleCount(%u)\n", uNumber);
	(void)uNumber;
	return 0x3FFE;
}

DWORD WINAPI FormatMessageA(DWORD dwFlags, LPCVOID lpSource, DWORD dwMessageId, DWORD dwLanguageId, LPSTR lpBuffer,
							DWORD nSize, va_list *Arguments) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FormatMessageA(%u, %p, %u, %u, %p, %u, %p)\n", dwFlags, lpSource, dwMessageId, dwLanguageId, lpBuffer,
			  nSize, Arguments);

	if (dwFlags & 0x00000100) {
		// FORMAT_MESSAGE_ALLOCATE_BUFFER
	} else if (dwFlags & 0x00002000) {
		// FORMAT_MESSAGE_ARGUMENT_ARRAY
	} else if (dwFlags & 0x00000800) {
		// FORMAT_MESSAGE_FROM_HMODULE
	} else if (dwFlags & 0x00000400) {
		// FORMAT_MESSAGE_FROM_STRING
	} else if (dwFlags & 0x00001000) {
		// FORMAT_MESSAGE_FROM_SYSTEM
		const char *message = strerror((int)dwMessageId);
		size_t length = strlen(message);
		if (!lpBuffer || nSize == 0) {
			kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		strncpy(lpBuffer, message, (size_t)nSize);
		if ((size_t)nSize <= length) {
			if (nSize > 0) {
				lpBuffer[nSize - 1] = '\0';
			}
			kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
			return 0;
		}
		lpBuffer[length] = '\0';
		return (DWORD)length;
	} else if (dwFlags & 0x00000200) {
		// FORMAT_MESSAGE_IGNORE_INSERTS
	} else {
		// unhandled?
	}

	if (lpBuffer && nSize > 0) {
		lpBuffer[0] = '\0';
	}
	kernel32_setLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return 0;
}

PVOID WINAPI EncodePointer(PVOID Ptr) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EncodePointer(%p)\n", Ptr);
	return Ptr;
}

PVOID WINAPI DecodePointer(PVOID Ptr) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DecodePointer(%p)\n", Ptr);
	return Ptr;
}

BOOL WINAPI SetDllDirectoryA(LPCSTR lpPathName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetDllDirectoryA(%s)\n", lpPathName);
	if (!lpPathName || lpPathName[0] == '\0') {
		wibo_clearDllDirectoryOverride();
		return TRUE;
	}

	char *hostPath = files_pathFromWindows(lpPathName);
	struct stat st;
	if (!hostPath || !*hostPath || stat(hostPath, &st) != 0) {
		free(hostPath);
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}

	char *absolutePath = makeAbsoluteHostPath(hostPath);
	free(hostPath);
	if (!absolutePath) {
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}
	wibo_setDllDirectoryOverride(absolutePath);
	free(absolutePath);
	return TRUE;
}

BOOL WINAPI FindActCtxSectionStringA(DWORD dwFlags, const GUID *lpExtensionGuid, ULONG ulSectionId,
									 LPCSTR lpStringToFind, PACTCTX_SECTION_KEYED_DATA ReturnedData) {
	DEBUG_LOG("FindActCtxSectionStringA(%#x, %p, %u, %s, %p)\n", dwFlags, lpExtensionGuid, ulSectionId,
			  lpStringToFind ? lpStringToFind : "<null>", ReturnedData);
	uint16_t *wideStorage = NULL;
	if (lpStringToFind) {
		size_t length = strlen(lpStringToFind);
		wideStorage = xmalloc((length + 1) * sizeof(uint16_t));
		for (size_t i = 0; i <= length; ++i) {
			wideStorage[i] = (uint8_t)lpStringToFind[i];
		}
	}
	BOOL result = FindActCtxSectionStringW(dwFlags, lpExtensionGuid, ulSectionId, (LPCWSTR)wideStorage, ReturnedData);
	free(wideStorage);
	return result;
}

BOOL WINAPI FindActCtxSectionStringW(DWORD dwFlags, const GUID *lpExtensionGuid, ULONG ulSectionId,
									 LPCWSTR lpStringToFind, PACTCTX_SECTION_KEYED_DATA ReturnedData) {
	char *lookup = wideStringToString((const uint16_t *)lpStringToFind,
									  lpStringToFind ? (int)wstrlen((const uint16_t *)lpStringToFind) : 0);
	DEBUG_LOG("FindActCtxSectionStringW(%#x, %p, %u, %s, %p)\n", dwFlags, lpExtensionGuid, ulSectionId, lookup,
			  ReturnedData);
	BOOL result = FALSE;

	if (lpExtensionGuid) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		goto done;
	}

	if (!ReturnedData) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		goto done;
	}

	if (dwFlags & ~FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		goto done;
	}

	ULONG originalSize = ReturnedData->cbSize;
	if (originalSize < sizeof(ACTCTX_SECTION_KEYED_DATA)) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		goto done;
	}

	ensureDefaultActivationContext();
	ActivationContext *ctx = currentActivationContext();
	const DllRedirectionEntry *matchedEntry = NULL;
	if (ulSectionId == ACTIVATION_CONTEXT_SECTION_DLL_REDIRECTION && *lookup) {
		char *lowerLookup = stringToLower(lookup, strlen(lookup));
		for (size_t i = 0; i < ctx->dllRedirections.len; ++i) {
			if (str_eq(ctx->dllRedirections.data[i].nameLower, lowerLookup)) {
				matchedEntry = &ctx->dllRedirections.data[i];
				break;
			}
		}
		free(lowerLookup);
	}

	size_t zeroSize = originalSize < sizeof(*ReturnedData) ? originalSize : sizeof(*ReturnedData);
	memset(ReturnedData, 0, zeroSize);
	ReturnedData->cbSize = originalSize;
	ReturnedData->ulDataFormatVersion = 1;
	ReturnedData->ulFlags = ACTCTX_SECTION_KEYED_DATA_FLAG_FOUND_IN_ACTCTX;
	if (dwFlags & FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX) {
		ReturnedData->hActCtx = (HANDLE)toGuestPtr(currentActivationContext());
	}

	if (!matchedEntry) {
		kernel32_setLastError(ERROR_SXS_KEY_NOT_FOUND);
		goto done;
	}

	ReturnedData->lpData = toGuestPtr(matchedEntry->dllData);
	ReturnedData->ulLength = matchedEntry->dllData->Size;
	ReturnedData->lpSectionBase = toGuestPtr(matchedEntry->dllData);
	ReturnedData->ulSectionTotalLength = matchedEntry->dllData->Size;
	ReturnedData->ulAssemblyRosterIndex = 1;
	memset(&ReturnedData->AssemblyMetadata, 0, sizeof ReturnedData->AssemblyMetadata);
	result = TRUE;

done:
	free(lookup);
	return result;
}

static uint64_t minU64(uint64_t a, uint64_t b) {
	return a < b ? a : b;
}

void kernel32_tryMarkExecutable(void *mem) {
	if (!mem) {
		return;
	}
	size_t usable = wibo_heap_guestSize(mem);
	if (usable == 0 || usable == SIZE_MAX) {
		return;
	}
	long pageSize = sysconf(_SC_PAGESIZE);
	if (pageSize <= 0) {
		return;
	}
	uintptr_t start = (uintptr_t)mem;
	uintptr_t alignedStart = start & ~(uintptr_t)(pageSize - 1);
	uintptr_t end = (start + usable + pageSize - 1) & ~(uintptr_t)(pageSize - 1);
	size_t length = (size_t)(end - alignedStart);
	if (length == 0) {
		return;
	}
	mprotect((void *)alignedStart, length, PROT_READ | PROT_WRITE | PROT_EXEC);
}

BOOL WINAPI IsBadReadPtr(LPCVOID lp, UINT_PTR ucb) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: IsBadReadPtr(ptr=%p, size=%zu)\n", lp, (size_t)ucb);
	if (!lp) {
		return TRUE;
	}
	return FALSE;
}

BOOL WINAPI IsBadWritePtr(LPVOID lp, UINT_PTR ucb) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: IsBadWritePtr(ptr=%p, size=%zu)\n", lp, (size_t)ucb);
	if (!lp && ucb != 0) {
		return TRUE;
	}
	return FALSE;
}

BOOL WINAPI GetComputerNameA(LPSTR lpBuffer, LPDWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetComputerNameA(%p, %p)\n", lpBuffer, nSize);
	if (!nSize || !lpBuffer) {
		if (nSize) {
			*nSize = 0;
		}
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (*nSize < kComputerNameRequiredSize) {
		*nSize = kComputerNameRequiredSize;
		kernel32_setLastError(ERROR_BUFFER_OVERFLOW);
		return FALSE;
	}

	strcpy(lpBuffer, kComputerNameAnsi);
	*nSize = kComputerNameLength;
	return TRUE;
}

BOOL WINAPI GetComputerNameW(LPWSTR lpBuffer, LPDWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetComputerNameW(%p, %p)\n", lpBuffer, nSize);
	if (!nSize || !lpBuffer) {
		if (nSize) {
			*nSize = 0;
		}
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (*nSize < kComputerNameRequiredSize) {
		*nSize = kComputerNameRequiredSize;
		kernel32_setLastError(ERROR_BUFFER_OVERFLOW);
		return FALSE;
	}

	wstrncpy(lpBuffer, kComputerNameWide, (size_t)kComputerNameRequiredSize);
	*nSize = kComputerNameLength;
	return TRUE;
}

HGLOBAL WINAPI GlobalAlloc(UINT uFlags, SIZE_T dwBytes) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("GlobalAlloc(%x, %zu)\n", uFlags, (size_t)dwBytes);
	if (uFlags & GMEM_MOVEABLE) {
		// not implemented rn
		assert(0);
		return GUEST_NULL;
	}
	bool zero = (uFlags & GMEM_ZEROINIT) != 0;
	void *ret = wibo_heap_guestMalloc((UINT)dwBytes, zero);
	VERBOSE_LOG("-> %p\n", ret);
	if (!ret) {
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	return toGuestPtr(ret);
}

HGLOBAL WINAPI GlobalFree(HGLOBAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("GlobalFree(%p)\n", (void *)(uintptr_t)hMem);
	if (wibo_heap_guestFree(fromGuestPtr(hMem))) {
		VERBOSE_LOG("-> success\n");
		return GUEST_NULL;
	} else {
		VERBOSE_LOG("-> failure\n");
		return hMem;
	}
}

HGLOBAL WINAPI GlobalReAlloc(HGLOBAL hMem, SIZE_T dwBytes, UINT uFlags) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("GlobalReAlloc(%p, %zu, %x)\n", (void *)(uintptr_t)hMem, (size_t)dwBytes, uFlags);
	if (uFlags & GMEM_MODIFY) {
		assert(0);
		return GUEST_NULL;
	}
	bool zero = (uFlags & GMEM_ZEROINIT) != 0;
	void *ret = wibo_heap_guestRealloc(fromGuestPtr(hMem), (UINT)dwBytes, zero);
	VERBOSE_LOG("-> %p\n", ret);
	if (!ret) {
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	return toGuestPtr(ret);
}

UINT WINAPI GlobalFlags(HGLOBAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("GlobalFlags(%p)\n", (void *)(uintptr_t)hMem);
	(void)hMem;
	return 0;
}

void WINAPI GlobalMemoryStatus(LPMEMORYSTATUS lpBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GlobalMemoryStatus(%p)\n", lpBuffer);
	if (!lpBuffer) {
		return;
	}

	memset(lpBuffer, 0, sizeof(*lpBuffer));
	lpBuffer->dwLength = sizeof(*lpBuffer);

	MemorySnapshot snapshot;
	if (!queryHostMemory(&snapshot)) {
		return;
	}

	uint64_t totalPhys = snapshot.totalPhys;
	uint64_t availPhys = snapshot.availPhys;
	uint64_t totalPageFile = snapshot.totalPageFile;
	uint64_t availPageFile = snapshot.availPageFile;

	const uint64_t kMinAppAddr = 0x00010000ULL;
	const uint64_t kMaxAppAddr = 0x7FFEFFFFULL;
	uint64_t totalVirtual = kMaxAppAddr - kMinAppAddr + 1;
	uint64_t availVirtual = totalVirtual;

	const uint64_t kMaxLegacy = (uint64_t)INT_MAX;
	totalPhys = minU64(totalPhys, kMaxLegacy);
	availPhys = minU64(availPhys, kMaxLegacy);
	totalVirtual = minU64(totalVirtual, kMaxLegacy);
	availVirtual = minU64(availVirtual, kMaxLegacy);
	availPhys = minU64(availPhys, totalPhys);
	availPageFile = minU64(availPageFile, totalPageFile);
	availVirtual = minU64(availVirtual, totalVirtual);

	if (totalPhys > 0) {
		uint64_t used = totalPhys > availPhys ? totalPhys - availPhys : 0;
		uint64_t load = (used * 100) / totalPhys;
		lpBuffer->dwMemoryLoad = (DWORD)minU64(load, 100);
	}

	lpBuffer->dwTotalPhys = clampToSizeT(totalPhys);
	lpBuffer->dwAvailPhys = clampToSizeT(availPhys);
	lpBuffer->dwTotalPageFile = clampToSizeT(totalPageFile);
	lpBuffer->dwAvailPageFile = clampToSizeT(availPageFile);
	lpBuffer->dwTotalVirtual = clampToSizeT(totalVirtual);
	lpBuffer->dwAvailVirtual = clampToSizeT(availVirtual);
}

HLOCAL WINAPI LocalAlloc(UINT uFlags, SIZE_T uBytes) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalAlloc(%x, %zu)\n", uFlags, (size_t)uBytes);
	bool zero = (uFlags & LMEM_ZEROINIT) != 0;
	if ((uFlags & LMEM_MOVEABLE) != 0) {
		VERBOSE_LOG("  ignoring LMEM_MOVEABLE\n");
	}
	void *result = wibo_heap_guestMalloc((UINT)uBytes, zero);
	if (!result) {
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	// Legacy Windows applications (pre-NX and DEP) may expect executable memory from LocalAlloc.
	kernel32_tryMarkExecutable(result);
	VERBOSE_LOG("  -> %p\n", result);
	return toGuestPtr(result);
}

HLOCAL WINAPI LocalFree(HLOCAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalFree(%p)\n", (void *)(uintptr_t)hMem);
	if (wibo_heap_guestFree(fromGuestPtr(hMem))) {
		VERBOSE_LOG("-> success\n");
		return GUEST_NULL;
	} else {
		VERBOSE_LOG("-> failure\n");
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return hMem;
	}
}

HLOCAL WINAPI LocalReAlloc(HLOCAL hMem, SIZE_T uBytes, UINT uFlags) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalReAlloc(%p, %zu, %x)\n", (void *)(uintptr_t)hMem, (size_t)uBytes, uFlags);
	bool zero = (uFlags & LMEM_ZEROINIT) != 0;
	if ((uFlags & LMEM_MOVEABLE) != 0) {
		VERBOSE_LOG("  ignoring LMEM_MOVEABLE\n");
	}
	void *result = wibo_heap_guestRealloc(fromGuestPtr(hMem), (UINT)uBytes, zero);
	if (!result && uBytes != 0) {
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	// Legacy Windows applications (pre-NX and DEP) may expect executable memory from LocalReAlloc.
	kernel32_tryMarkExecutable(result);
	VERBOSE_LOG("  -> %p\n", result);
	return toGuestPtr(result);
}

HLOCAL WINAPI LocalHandle(LPCVOID pMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalHandle(%p)\n", pMem);
	return toGuestPtr(pMem);
}

LPVOID WINAPI LocalLock(HLOCAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("STUB: LocalLock(%p)\n", (void *)(uintptr_t)hMem);
	return fromGuestPtr(hMem);
}

BOOL WINAPI LocalUnlock(HLOCAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalUnlock(%p)\n", (void *)(uintptr_t)hMem);
	(void)hMem;
	return TRUE;
}

SIZE_T WINAPI LocalSize(HLOCAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("LocalSize(%p)\n", (void *)(uintptr_t)hMem);
	if (!hMem) {
		return 0;
	}
	size_t size = wibo_heap_guestSize(fromGuestPtr(hMem));
	return size == SIZE_MAX ? 0 : size;
}

UINT WINAPI LocalFlags(HLOCAL hMem) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("STUB: LocalFlags(%p)\n", (void *)(uintptr_t)hMem);
	(void)hMem;
	return 0;
}

#define kSystemDirectoryA "C:\\Windows\\System32"

UINT WINAPI GetSystemDirectoryA(LPSTR lpBuffer, UINT uSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemDirectoryA(%p, %u)\n", lpBuffer, uSize);
	if (!lpBuffer) {
		return 0;
	}

	const size_t len = strlen(kSystemDirectoryA);
	if (uSize < len + 1) {
		return (UINT)(len + 1);
	}
	strcpy(lpBuffer, kSystemDirectoryA);
	return (UINT)len;
}

UINT WINAPI GetSystemDirectoryW(LPWSTR lpBuffer, UINT uSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemDirectoryW(%p, %u)\n", lpBuffer, uSize);
	if (!lpBuffer) {
		return 0;
	}

	uint16_t *wide = stringToWideString(kSystemDirectoryA, SIZE_MAX);
	UINT length = (UINT)wstrlen(wide);
	if (uSize < length + 1) {
		free(wide);
		return length + 1;
	}
	memcpy(lpBuffer, wide, (length + 1) * sizeof(uint16_t));
	free(wide);
	return length;
}

UINT WINAPI GetSystemWow64DirectoryA(LPSTR lpBuffer, UINT uSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemWow64DirectoryA(%p, %u)\n", lpBuffer, uSize);
	(void)lpBuffer;
	(void)uSize;
	kernel32_setLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return 0;
}

UINT WINAPI GetSystemWow64DirectoryW(LPWSTR lpBuffer, UINT uSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemWow64DirectoryW(%p, %u)\n", lpBuffer, uSize);
	(void)lpBuffer;
	(void)uSize;
	kernel32_setLastError(ERROR_CALL_NOT_IMPLEMENTED);
	return 0;
}

UINT WINAPI GetWindowsDirectoryA(LPSTR lpBuffer, UINT uSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetWindowsDirectoryA(%p, %u)\n", lpBuffer, uSize);
	if (!lpBuffer) {
		return 0;
	}

	const char *windowsDir = "C:\\Windows";
	const size_t len = strlen(windowsDir);
	if (uSize < len + 1) {
		return (UINT)(len + 1);
	}
	strcpy(lpBuffer, windowsDir);
	return (UINT)len;
}

UINT WINAPI GetSystemWindowsDirectoryA(LPSTR lpBuffer, UINT uSize) {
	DEBUG_LOG("GetSystemWindowsDirectoryA(%p, %u)\n", lpBuffer, uSize);
	return GetWindowsDirectoryA(lpBuffer, uSize);
}

UINT WINAPI GetSystemWindowsDirectoryW(LPWSTR lpBuffer, UINT uSize) {
	DEBUG_LOG("GetSystemWindowsDirectoryW(%p, %u)\n", lpBuffer, uSize);
	if (!lpBuffer) {
		return 0;
	}

	uint16_t *wide = stringToWideString("C:\\Windows", SIZE_MAX);
	UINT length = (UINT)wstrlen(wide);
	if (uSize < length + 1) {
		free(wide);
		return length + 1;
	}
	memcpy(lpBuffer, wide, (length + 1) * sizeof(uint16_t));
	free(wide);
	return length;
}

DWORD WINAPI GetCurrentDirectoryA(DWORD nBufferLength, LPSTR lpBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCurrentDirectoryA(%u, %p)\n", nBufferLength, lpBuffer);

	char *path = NULL;
	DWORD result = 0;
	if (!tryGetCurrentDirectoryPath(&path)) {
		return 0;
	}

	const DWORD required = (DWORD)(strlen(path) + 1);
	if (nBufferLength == 0) {
		result = required;
		goto done;
	}
	if (!lpBuffer) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		goto done;
	}
	if (nBufferLength < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		result = required;
		goto done;
	}
	memcpy(lpBuffer, path, required);
	result = required - 1;

done:
	free(path);
	return result;
}

DWORD WINAPI GetCurrentDirectoryW(DWORD nBufferLength, LPWSTR lpBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCurrentDirectoryW(%u, %p)\n", nBufferLength, lpBuffer);

	char *path = NULL;
	DWORD result = 0;
	if (!tryGetCurrentDirectoryPath(&path)) {
		return 0;
	}
	uint16_t *widePath = stringToWideString(path, SIZE_MAX);
	const DWORD required = (DWORD)(wstrlen(widePath) + 1);
	if (nBufferLength == 0) {
		result = required;
		goto done;
	}
	if (!lpBuffer) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		goto done;
	}
	if (nBufferLength < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		result = required;
		goto done;
	}
	memcpy(lpBuffer, widePath, required * sizeof(uint16_t));
	result = required - 1;

done:
	free(widePath);
	free(path);
	return result;
}

int WINAPI SetCurrentDirectoryA(LPCSTR lpPathName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetCurrentDirectoryA(%s)\n", lpPathName ? lpPathName : "(null)");
	if (!lpPathName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *hostPath = files_pathFromWindows(lpPathName);
	int rc = chdir(hostPath);
	int savedErrno = errno;
	free(hostPath);
	if (rc != 0) {
		kernel32_setLastError(wibo_winErrorFromErrno(savedErrno));
		return 0;
	}
	return 1;
}

int WINAPI SetCurrentDirectoryW(LPCWSTR lpPathName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetCurrentDirectoryW\n");
	if (!lpPathName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *path = wideStringToString(lpPathName, -1);
	int result = SetCurrentDirectoryA(path);
	free(path);
	return result;
}

DWORD WINAPI GetLongPathNameA(LPCSTR lpszShortPath, LPSTR lpszLongPath, DWORD cchBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLongPathNameA(%s, %p, %u)\n", lpszShortPath ? lpszShortPath : "(null)", lpszLongPath, cchBuffer);
	if (!lpszShortPath) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	char *longPath = NULL;
	DWORD result = 0;
	if (!computeLongWindowsPath(lpszShortPath, &longPath)) {
		return 0;
	}

	DWORD required = (DWORD)(strlen(longPath) + 1);
	if (cchBuffer == 0) {
		result = required;
		goto done;
	}
	if (!lpszLongPath) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		goto done;
	}
	if (cchBuffer < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		result = required;
		goto done;
	}
	memcpy(lpszLongPath, longPath, required);
	result = required - 1;

done:
	free(longPath);
	return result;
}

DWORD WINAPI GetLongPathNameW(LPCWSTR lpszShortPath, LPWSTR lpszLongPath, DWORD cchBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLongPathNameW(%p, %p, %u)\n", lpszShortPath, lpszLongPath, cchBuffer);
	if (!lpszShortPath) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *input = wideStringToString(lpszShortPath, -1);
	char *longPath = NULL;
	uint16_t *wideLong = NULL;
	DWORD result = 0;
	if (!computeLongWindowsPath(input, &longPath)) {
		goto done;
	}
	wideLong = stringToWideString(longPath, SIZE_MAX);
	DWORD required = (DWORD)wstrlen(wideLong) + 1;
	if (cchBuffer == 0) {
		result = required;
		goto done;
	}
	if (!lpszLongPath) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		goto done;
	}
	if (cchBuffer < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		result = required;
		goto done;
	}
	memcpy(lpszLongPath, wideLong, required * sizeof(uint16_t));
	result = required - 1;

done:
	free(wideLong);
	free(longPath);
	free(input);
	return result;
}

BOOL WINAPI GetDiskFreeSpaceA(LPCSTR lpRootPathName, LPDWORD lpSectorsPerCluster, LPDWORD lpBytesPerSector,
							  LPDWORD lpNumberOfFreeClusters, LPDWORD lpTotalNumberOfClusters) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetDiskFreeSpaceA(%s)\n", lpRootPathName ? lpRootPathName : "(null)");
	struct statvfs buf = {0};
	char *resolvedPath = NULL;
	if (!resolveDiskFreeSpaceStat(lpRootPathName, &buf, &resolvedPath)) {
		return FALSE;
	}

	uint64_t blockSize = buf.f_frsize ? buf.f_frsize : buf.f_bsize;
	if (blockSize == 0) {
		blockSize = 4096;
	}
	unsigned int bytesPerSector = 512;
	if (blockSize % bytesPerSector != 0) {
		bytesPerSector = (unsigned int)minU64(blockSize, UINT_MAX);
	}
	unsigned int sectorsPerCluster = (unsigned int)(blockSize / bytesPerSector);
	if (sectorsPerCluster == 0) {
		sectorsPerCluster = 1;
		bytesPerSector = (unsigned int)minU64(blockSize, UINT_MAX);
	}

	uint64_t totalClusters64 = buf.f_blocks;
	uint64_t freeClusters64 = buf.f_bavail;

	if (lpSectorsPerCluster) {
		*lpSectorsPerCluster = sectorsPerCluster;
	}
	if (lpBytesPerSector) {
		*lpBytesPerSector = bytesPerSector;
	}
	if (lpNumberOfFreeClusters) {
		*lpNumberOfFreeClusters = (DWORD)minU64(freeClusters64, UINT_MAX);
	}
	if (lpTotalNumberOfClusters) {
		*lpTotalNumberOfClusters = (DWORD)minU64(totalClusters64, UINT_MAX);
	}

	DEBUG_LOG("\t-> host %s, spc %u, bps %u, free clusters %u, total clusters %u\n", resolvedPath,
			  lpSectorsPerCluster ? *lpSectorsPerCluster : 0, lpBytesPerSector ? *lpBytesPerSector : 0,
			  lpNumberOfFreeClusters ? *lpNumberOfFreeClusters : 0,
			  lpTotalNumberOfClusters ? *lpTotalNumberOfClusters : 0);
	free(resolvedPath);
	return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceW(LPCWSTR lpRootPathName, LPDWORD lpSectorsPerCluster, LPDWORD lpBytesPerSector,
							  LPDWORD lpNumberOfFreeClusters, LPDWORD lpTotalNumberOfClusters) {
	HOST_CONTEXT_GUARD();
	char *rootPath = wideStringToString(lpRootPathName, -1);
	BOOL result = GetDiskFreeSpaceA(lpRootPathName ? rootPath : NULL, lpSectorsPerCluster, lpBytesPerSector,
									lpNumberOfFreeClusters, lpTotalNumberOfClusters);
	free(rootPath);
	return result;
}

BOOL WINAPI GetDiskFreeSpaceExA(LPCSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller,
								PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetDiskFreeSpaceExA(%s)\n", lpDirectoryName ? lpDirectoryName : "(null)");
	struct statvfs buf = {0};
	char *resolvedPath = NULL;
	if (!resolveDiskFreeSpaceStat(lpDirectoryName, &buf, &resolvedPath)) {
		return FALSE;
	}

	uint64_t blockSize = buf.f_frsize ? buf.f_frsize : buf.f_bsize;
	if (blockSize == 0) {
		blockSize = 4096;
	}

	uint64_t freeToCaller = (uint64_t)buf.f_bavail * blockSize;
	uint64_t totalBytes = (uint64_t)buf.f_blocks * blockSize;
	uint64_t totalFree = (uint64_t)buf.f_bfree * blockSize;

	if (lpFreeBytesAvailableToCaller) {
		lpFreeBytesAvailableToCaller->QuadPart = freeToCaller;
	}
	if (lpTotalNumberOfBytes) {
		lpTotalNumberOfBytes->QuadPart = totalBytes;
	}
	if (lpTotalNumberOfFreeBytes) {
		lpTotalNumberOfFreeBytes->QuadPart = totalFree;
	}

	DEBUG_LOG("\t-> host %s, free %llu, total %llu, total free %llu\n", resolvedPath, (unsigned long long)freeToCaller,
			  (unsigned long long)totalBytes, (unsigned long long)totalFree);
	free(resolvedPath);
	return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceExW(LPCWSTR lpDirectoryName, PULARGE_INTEGER lpFreeBytesAvailableToCaller,
								PULARGE_INTEGER lpTotalNumberOfBytes, PULARGE_INTEGER lpTotalNumberOfFreeBytes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetDiskFreeSpaceExW -> ");
	char *directoryName = wideStringToString(lpDirectoryName, -1);
	BOOL result = GetDiskFreeSpaceExA(lpDirectoryName ? directoryName : NULL, lpFreeBytesAvailableToCaller,
									  lpTotalNumberOfBytes, lpTotalNumberOfFreeBytes);
	free(directoryName);
	return result;
}

LPWSTR WINAPI lstrcpynW(LPWSTR lpString1, LPCWSTR lpString2, int iMaxLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcpynW(%p, %p, %d)\n", lpString1, lpString2, iMaxLength);
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	LPWSTR d = lpString1;
	LPCWSTR s = lpString2;
	UINT count = (UINT)iMaxLength;
	while (count > 1 && *s) {
		--count;
		*d++ = *s++;
	}
	if (count) {
		*d = 0;
	}
	return lpString1;
}

LPSTR WINAPI lstrcpyA(LPSTR lpString1, LPCSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcpyA(%p, %p)\n", lpString1, lpString2);
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	strcpy(lpString1, lpString2);
	return lpString1;
}

LPWSTR WINAPI lstrcpyW(LPWSTR lpString1, LPCWSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcpyW(%p, %p)\n", lpString1, lpString2);
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	wstrcpy(lpString1, lpString2);
	return lpString1;
}

LPSTR WINAPI lstrcatA(LPSTR lpString1, LPCSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcatA(%p, %p)\n", lpString1, lpString2);
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	strcat(lpString1, lpString2);
	return lpString1;
}

LPWSTR WINAPI lstrcatW(LPWSTR lpString1, LPCWSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcatW(%p, %p)\n", lpString1, lpString2);
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	wstrcat(lpString1, lpString2);
	return lpString1;
}

int WINAPI lstrlenA(LPCSTR lpString) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("lstrlenA(%p)\n", lpString);
	if (!lpString) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	return (int)strlen(lpString);
}

int WINAPI lstrlenW(LPCWSTR lpString) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("lstrlenW(%p)\n", lpString);
	if (!lpString) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	return (int)wstrlen(lpString);
}

// TODO: Use Windows locale ordering for lstrcmp/lstrcmpi.
int WINAPI lstrcmpA(LPCSTR lpString1, LPCSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcmpA(%p, %p)\n", lpString1, lpString2);
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	int cmp = strcmp(lpString1, lpString2);
	return (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
}

int WINAPI lstrcmpW(LPCWSTR lpString1, LPCWSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcmpW(%p, %p)\n", lpString1, lpString2);
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	const uint16_t *a = lpString1;
	const uint16_t *b = lpString2;
	while (*a && *a == *b) {
		++a;
		++b;
	}
	if (*a == *b) {
		return 0;
	}
	return (*a < *b) ? -1 : 1;
}

int WINAPI lstrcmpiA(LPCSTR lpString1, LPCSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcmpiA(%p, %p)\n", lpString1, lpString2);
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	LPCSTR a = lpString1;
	LPCSTR b = lpString2;
	while (*a && *b) {
		int ca = tolower((unsigned char)*a);
		int cb = tolower((unsigned char)*b);
		if (ca != cb) {
			return (ca < cb) ? -1 : 1;
		}
		++a;
		++b;
	}
	if (*a == *b) {
		return 0;
	}
	return (*a == '\0') ? -1 : 1;
}

int WINAPI lstrcmpiW(LPCWSTR lpString1, LPCWSTR lpString2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("lstrcmpiW(%p, %p)\n", lpString1, lpString2);
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	const uint16_t *a = lpString1;
	const uint16_t *b = lpString2;
	while (*a && *b) {
		uint16_t ca = wcharToLower(*a);
		uint16_t cb = wcharToLower(*b);
		if (ca != cb) {
			return (ca < cb) ? -1 : 1;
		}
		++a;
		++b;
	}
	if (*a == *b) {
		return 0;
	}
	return (*a == 0) ? -1 : 1;
}

