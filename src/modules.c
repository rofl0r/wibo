#include "modules.h"

#include "bundle.h"
#include "common.h"
#include "entry.h"
#include "entry_trampolines.h"
#include "errors.h"
#include "files.h"
#include "heap.h"
#include "kernel32/errhandlingapi.h"
#include "kernel32/internal.h"
#include "strutil.h"
#include "tls.h"
#include "types.h"

#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern const ModuleStub lib_advapi32;
extern const ModuleStub lib_bcrypt;
#if WIBO_HAS_CRTDLL
extern ModuleStub lib_crtdll;
#endif
extern const ModuleStub lib_kernel32;
extern const ModuleStub lib_dbghelp;
extern const ModuleStub lib_lmgr;
extern const ModuleStub lib_mscoree;
#if WIBO_HAS_MSVCRT
extern ModuleStub lib_msvcrt;
#endif
#if WIBO_HAS_MSVCIRT
extern ModuleStub lib_msvcirt;
#endif
#if WIBO_HAS_MSVCRT20
extern ModuleStub lib_msvcrt20;
#endif
#if WIBO_HAS_MSVCRT40
extern ModuleStub lib_msvcrt40;
#endif
#if WIBO_HAS_MSVCR71
extern ModuleStub lib_msvcr71;
#endif
#if WIBO_HAS_MSVCR90
extern ModuleStub lib_msvcr90;
#endif
#if WIBO_HAS_MSVCR100
extern ModuleStub lib_msvcr100;
#endif
#if WIBO_HAS_MSVCR120
extern ModuleStub lib_msvcr120;
#endif
#if WIBO_HAS_UCRTBASE
extern ModuleStub lib_ucrtbase;
#endif
extern const ModuleStub lib_ntdll;
extern const ModuleStub lib_rpcrt4;
extern const ModuleStub lib_ole32;
extern const ModuleStub lib_oleaut32;
extern const ModuleStub lib_shlwapi;
extern const ModuleStub lib_user32;
extern const ModuleStub lib_vcruntime;
extern const ModuleStub lib_version;
extern const ModuleStub lib_ws2;

// setup.S
#if defined(__x86_64__)
#define STUB_THUNK_ASM_NAME(N) "_Z9stubThunkILm" #N "EEvv"
#else
#define STUB_THUNK_ASM_NAME(N) "_Z9stubThunkILj" #N "EEvv"
#endif

#define STUB_THUNK_INDICES(X)                                                                                          \
	X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)                             \
	X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)                   \
	X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47)                   \
	X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) X(56) X(57) X(58) X(59) X(60) X(61) X(62) X(63)                   \
	X(64) X(65) X(66) X(67) X(68) X(69) X(70) X(71) X(72) X(73) X(74) X(75) X(76) X(77) X(78) X(79)                   \
	X(80) X(81) X(82) X(83) X(84) X(85) X(86) X(87) X(88) X(89) X(90) X(91) X(92) X(93) X(94) X(95)                   \
	X(96) X(97) X(98) X(99) X(100) X(101) X(102) X(103) X(104) X(105) X(106) X(107) X(108) X(109) X(110) X(111)       \
	X(112) X(113) X(114) X(115) X(116) X(117) X(118) X(119) X(120) X(121) X(122) X(123) X(124) X(125) X(126) X(127)   \
	X(128) X(129) X(130) X(131) X(132) X(133) X(134) X(135) X(136) X(137) X(138) X(139) X(140) X(141) X(142) X(143)   \
	X(144) X(145) X(146) X(147) X(148) X(149) X(150) X(151) X(152) X(153) X(154) X(155) X(156) X(157) X(158) X(159)   \
	X(160) X(161) X(162) X(163) X(164) X(165) X(166) X(167) X(168) X(169) X(170) X(171) X(172) X(173) X(174) X(175)   \
	X(176) X(177) X(178) X(179) X(180) X(181) X(182) X(183) X(184) X(185) X(186) X(187) X(188) X(189) X(190) X(191)   \
	X(192) X(193) X(194) X(195) X(196) X(197) X(198) X(199) X(200) X(201) X(202) X(203) X(204) X(205) X(206) X(207)   \
	X(208) X(209) X(210) X(211) X(212) X(213) X(214) X(215) X(216) X(217) X(218) X(219) X(220) X(221) X(222) X(223)   \
	X(224) X(225) X(226) X(227) X(228) X(229) X(230) X(231) X(232) X(233) X(234) X(235) X(236) X(237) X(238) X(239)   \
	X(240) X(241) X(242) X(243) X(244) X(245) X(246) X(247) X(248) X(249) X(250) X(251) X(252) X(253) X(254) X(255)

#define DECLARE_STUB_THUNK(N) extern void stubThunk##N(void) __asm__(STUB_THUNK_ASM_NAME(N));
STUB_THUNK_INDICES(DECLARE_STUB_THUNK)
#undef DECLARE_STUB_THUNK

#define STUB_TABLE_ENTRY(N) stubThunk##N,

typedef struct {
	const char *dll;
	const char *api;
} ApiSetEntry;

static const ApiSetEntry kApiSet[] = {
	{"api-ms-win-core-crt-l1-1-0.dll", "msvcrt.dll"},
	{"api-ms-win-core-crt-l2-1-0.dll", "msvcrt.dll"},
	{"api-ms-win-crt-conio-l1-1-0.dll", "msvcrt.dll"},
	{"api-ms-win-crt-convert-l1-1-0.dll", "msvcrt.dll"},
	{"api-ms-win-crt-environment-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-filesystem-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-heap-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-locale-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-math-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-multibyte-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-private-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-process-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-runtime-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-stdio-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-string-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-time-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-crt-utility-l1-1-0.dll", "ucrtbase.dll"},
	{"api-ms-win-core-synch-l1-2-0.dll", "kernelbase.dll"},
};

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3
#define TLS_PROCESS_ATTACH DLL_PROCESS_ATTACH
#define TLS_PROCESS_DETACH DLL_PROCESS_DETACH
#define TLS_THREAD_ATTACH DLL_THREAD_ATTACH
#define TLS_THREAD_DETACH DLL_THREAD_DETACH

typedef struct __attribute__((packed)) {
	uint32_t characteristics;
	uint32_t timeDateStamp;
	uint16_t majorVersion;
	uint16_t minorVersion;
	uint32_t name;
	uint32_t base;
	uint32_t numberOfFunctions;
	uint32_t numberOfNames;
	uint32_t addressOfFunctions;
	uint32_t addressOfNames;
	uint32_t addressOfNameOrdinals;
} PEExportDirectory;

typedef void (*StubFuncType)(void);
#define MAX_STUBS 0x100
static size_t g_stubIndex = 0;
static char *g_stubDlls[MAX_STUBS];
static char *g_stubFuncNames[MAX_STUBS];
// Keyed by "lower(dll):lower(func)"; values hold a StubFuncType cast to void *.
static hashmap *g_stubCache = NULL;
static HANDLE g_nextStubHandle = 1;
static const StubFuncType g_stubFuncs[MAX_STUBS] = {STUB_THUNK_INDICES(STUB_TABLE_ENTRY)};
#undef STUB_TABLE_ENTRY

// Every ModuleInfo ever created is kept alive for the life of the process.
static PtrVec g_allModules;

static char *makeStubKey(const char *dllName, const char *funcName) {
	size_t dllLen = dllName ? strlen(dllName) : 0;
	size_t funcLen = funcName ? strlen(funcName) : 0;
	char *key = xmalloc(dllLen + 1 + funcLen + 1);
	if (dllName) {
		memcpy(key, dllName, dllLen);
	}
	key[dllLen] = '\0';
	toLowerInPlace(key);
	key[dllLen] = ':';
	if (funcName) {
		memcpy(key + dllLen + 1, funcName, funcLen);
	}
	key[dllLen + 1 + funcLen] = '\0';
	toLowerInPlace(key + dllLen + 1);
	return key;
}

static StubFuncType resolveMissingFuncName(const char *dllName, const char *funcName) {
	DEBUG_LOG("Missing function: %s (%s)\n", dllName, funcName);
	if (!g_stubCache) {
		g_stubCache = hashmap_create();
	}
	char *key = makeStubKey(dllName, funcName);
	size_t keyLen = strlen(key);
	void *existing = NULL;
	if (hashmap_find(g_stubCache, key, keyLen, &existing)) {
		free(key);
		return (StubFuncType)existing;
	}
	if (g_stubIndex >= MAX_STUBS) {
		fprintf(stderr, "wibo: too many missing functions encountered (>%zu). Last failure: %s (%s)\n", (size_t)MAX_STUBS,
				funcName, dllName);
		fflush(stderr);
		abort();
	}
	g_stubFuncNames[g_stubIndex] = xstrdup(funcName ? funcName : "");
	g_stubDlls[g_stubIndex] = xstrdup(dllName ? dllName : "");
	StubFuncType stub = g_stubFuncs[g_stubIndex];
	hashmap_put(g_stubCache, key, keyLen, (void *)stub);
	free(key);
	g_stubIndex++;
	return stub;
}

static StubFuncType resolveMissingFuncOrdinal(const char *dllName, uint16_t ordinal) {
	char funcName[8];
	snprintf(funcName, sizeof funcName, "%u", (unsigned)ordinal);
	return resolveMissingFuncName(dllName, funcName);
}

static ModuleInfo *loadForwarderTargetModule(char **dllName) {
	ModuleInfo *target = wibo_loadModule(*dllName);
	if (target || (*dllName)[0] != '_') {
		return target;
	}

	char *undecoratedName = xstrdup(*dllName + 1);
	target = wibo_loadModule(undecoratedName);
	if (target) {
		DEBUG_LOG("Forwarded export: treating decorated DLL name %s as %s\n", *dllName, undecoratedName);
		free(*dllName);
		*dllName = undecoratedName;
	} else {
		free(undecoratedName);
	}
	return target;
}

static void *resolveForwardedExport(ModuleInfo *source, const char *forwarder) {
	if (!forwarder || !*forwarder) {
		return (void *)resolveMissingFuncName(source->originalName, "");
	}

	const char *separator = strchr(forwarder, '.');
	if (!separator || separator == forwarder || !separator[1]) {
		return (void *)resolveMissingFuncName(source->originalName, forwarder);
	}

	char *dllName = xstrndup(forwarder, (size_t)(separator - forwarder));
	char *exportName = xstrdup(separator + 1);
	DEBUG_LOG("Forwarded export: %s!%s -> %s!%s\n", source->originalName, forwarder, dllName, exportName);

	ModuleInfo *target = loadForwarderTargetModule(&dllName);
	if (!target) {
		void *missing = (void *)resolveMissingFuncName(dllName, exportName);
		free(dllName);
		free(exportName);
		return missing;
	}

	if (exportName[0] == '#') {
		char *end = NULL;
		unsigned long ordinal = strtoul(exportName + 1, &end, 10);
		if (end && *end == '\0' && ordinal <= UINT16_MAX) {
			void *func = wibo_resolveFuncByOrdinal(target, (uint16_t)ordinal);
			if (func) {
				free(dllName);
				free(exportName);
				return func;
			}
		}
		void *missing = (void *)resolveMissingFuncName(dllName, exportName);
		free(dllName);
		free(exportName);
		return missing;
	}

	void *func = wibo_resolveFuncByName(target, exportName);
	if (func) {
		free(dllName);
		free(exportName);
		return func;
	}
	void *missing = (void *)resolveMissingFuncName(dllName, exportName);
	free(dllName);
	free(exportName);
	return missing;
}

typedef struct {
	hashmap *modulesByKey;    // name -> ModuleInfo *
	hashmap *modulesByAlias;  // alias -> ModuleInfo *
	char *dllDirectory;       // NULL when no override is set
	bool initialized;
	hashmap *builtinAliasMap; // alias -> ModuleInfo *
	hashmap *pinnedAliases;   // alias -> (void *)1
	hashmap *pinnedModules;   // ModuleInfo * bytes -> (void *)1
	PtrVec tlsModuleSlots;
} ModuleRegistry;

static ModuleRegistry g_registry;
static pthread_mutex_t g_registryMutex;
static pthread_once_t g_registryOnce = PTHREAD_ONCE_INIT;

static void initRegistryStorage(void) {
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&g_registryMutex, &attr);
	pthread_mutexattr_destroy(&attr);
	g_registry.modulesByKey = hashmap_create();
	g_registry.modulesByAlias = hashmap_create();
	g_registry.builtinAliasMap = hashmap_create();
	g_registry.pinnedAliases = hashmap_create();
	g_registry.pinnedModules = hashmap_create();
}

static void registerBuiltinModule(ModuleRegistry *reg, const ModuleStub *module);

// Returns with the registry mutex held; release with unlockRegistry.
static ModuleRegistry *lockRegistry(void) {
	pthread_once(&g_registryOnce, initRegistryStorage);
	pthread_mutex_lock(&g_registryMutex);
	if (!g_registry.initialized) {
		g_registry.initialized = true;
		const ModuleStub *builtins[] = {
			&lib_advapi32,
			&lib_bcrypt,
			&lib_dbghelp,
			&lib_kernel32,
			&lib_lmgr,
			&lib_mscoree,
			&lib_ntdll,
			&lib_ole32,
			&lib_oleaut32,
			&lib_rpcrt4,
			&lib_shlwapi,
			&lib_user32,
			&lib_vcruntime,
			&lib_version,
			&lib_ws2,
#if WIBO_HAS_CRTDLL
			&lib_crtdll,
#endif
#if WIBO_HAS_MSVCRT
			&lib_msvcrt,
#endif
#if WIBO_HAS_MSVCIRT
			&lib_msvcirt,
#endif
#if WIBO_HAS_MSVCRT20
			&lib_msvcrt20,
#endif
#if WIBO_HAS_MSVCRT40
			&lib_msvcrt40,
#endif
#if WIBO_HAS_MSVCR71
			&lib_msvcr71,
#endif
#if WIBO_HAS_MSVCR90
			&lib_msvcr90,
#endif
#if WIBO_HAS_MSVCR100
			&lib_msvcr100,
#endif
#if WIBO_HAS_MSVCR120
			&lib_msvcr120,
#endif
#if WIBO_HAS_UCRTBASE
			&lib_ucrtbase,
#endif
			NULL,
		};
		for (const ModuleStub **module = builtins; *module; ++module) {
			registerBuiltinModule(&g_registry, *module);
		}
	}
	return &g_registry;
}

static void unlockRegistry(void) {
	pthread_mutex_unlock(&g_registryMutex);
}

static DWORD allocateModuleTlsSlot(ModuleRegistry *reg, ModuleInfo *module) {
	for (DWORD i = 0; i < (DWORD)reg->tlsModuleSlots.len; ++i) {
		if (reg->tlsModuleSlots.data[i] == NULL) {
			reg->tlsModuleSlots.data[i] = module;
			return i;
		}
	}
	vec_push(&reg->tlsModuleSlots, module);
	return (DWORD)(reg->tlsModuleSlots.len - 1);
}

static void releaseModuleTlsSlot(ModuleRegistry *reg, DWORD index) {
	if (index < reg->tlsModuleSlots.len) {
		reg->tlsModuleSlots.data[index] = NULL;
	}
}

// Returns a malloc'd copy with '/' mapped to '\' and lowercased.
static char *normalizeAlias(const char *value) {
	char *out = xstrdup(value);
	for (char *p = out; *p; ++p) {
		if (*p == '/') {
			*p = '\\';
		}
	}
	toLowerInPlace(out);
	return out;
}

typedef struct {
	char *directory; // Windows-style directory component; empty if none (owned).
	char *base;      // (owned)
	bool hasExtension;
	bool endsWithDot;
} ParsedModuleName;

static ParsedModuleName parseModuleName(const char *name) {
	ParsedModuleName parsed;
	char *sanitized = xstrdup(name);
	for (char *p = sanitized; *p; ++p) {
		if (*p == '/') {
			*p = '\\';
		}
	}
	char *sep = strrchr(sanitized, '\\');
	if (sep) {
		parsed.directory = xstrndup(sanitized, (size_t)(sep - sanitized));
		parsed.base = xstrdup(sep + 1);
	} else {
		parsed.directory = xstrdup("");
		parsed.base = xstrdup(sanitized);
	}
	free(sanitized);
	size_t baseLen = strlen(parsed.base);
	parsed.endsWithDot = baseLen > 0 && parsed.base[baseLen - 1] == '.';
	parsed.hasExtension = !parsed.endsWithDot && strchr(parsed.base, '.') != NULL;
	return parsed;
}

static void parsedModuleNameFree(ParsedModuleName *parsed) {
	free(parsed->directory);
	free(parsed->base);
}

static bool parsedModuleNameWantsDllSuffix(const ParsedModuleName *parsed) {
	return !parsed->hasExtension && !parsed->endsWithDot;
}

// Returns a malloc'd "<base>" or "<base>.dll" alias, or "" if the base name is empty.
static char *normalizedBaseKey(const ParsedModuleName *parsed) {
	if (parsed->base[0] == '\0') {
		return xstrdup("");
	}
	char *base;
	if (parsedModuleNameWantsDllSuffix(parsed)) {
		size_t len = strlen(parsed->base);
		base = xmalloc(len + sizeof(".dll"));
		memcpy(base, parsed->base, len);
		memcpy(base + len, ".dll", sizeof(".dll"));
	} else {
		base = xstrdup(parsed->base);
	}
	char *normalized = normalizeAlias(base);
	free(base);
	return normalized;
}

typedef struct {
	uint32_t StartAddressOfRawData;
	uint32_t EndAddressOfRawData;
	uint32_t AddressOfIndex;
	uint32_t AddressOfCallBacks;
	uint32_t SizeOfZeroFill;
	uint32_t Characteristics;
} ImageTlsDirectory32;

typedef struct {
	uint64_t StartAddressOfRawData;
	uint64_t EndAddressOfRawData;
	uint64_t AddressOfIndex;
	uint64_t AddressOfCallBacks;
	uint32_t SizeOfZeroFill;
	uint32_t Characteristics;
} ImageTlsDirectory64;

// Bitness-independent view of the TLS directory fields used by wibo_initializeModuleTls.
typedef struct {
	uint64_t StartAddressOfRawData;
	uint64_t EndAddressOfRawData;
	uint64_t AddressOfIndex;
	uint64_t AddressOfCallBacks;
	uint32_t SizeOfZeroFill;
	uint32_t Characteristics;
} TlsDirectoryFields;

static const size_t kMinTlsDirectorySize32 = offsetof(ImageTlsDirectory32, SizeOfZeroFill);
static const size_t kMinTlsDirectorySize64 = offsetof(ImageTlsDirectory64, SizeOfZeroFill);

static uintptr_t resolveModuleAddress(const Executable *exec, uintptr_t address) {
	if (address == 0) {
		return 0;
	}
	const uintptr_t actualBase = (uintptr_t)exec->imageBase;
	if (address >= actualBase) {
		uintptr_t offset = address - actualBase;
		if (offset < exec->imageSize) {
			return address;
		}
	}
	const uintptr_t preferredBase = exec->preferredImageBase;
	if (address >= preferredBase) {
		return actualBase + (address - preferredBase);
	}
	return (uintptr_t)((intptr_t)address + exec->relocationDelta);
}

ModuleInfo *wibo_mainModule = NULL;

static void runModuleTlsCallbacks(ModuleInfo *module, DWORD reason);

static ModuleInfo *createModuleInfo(void) {
	ModuleInfo *info = xcalloc(1, sizeof(ModuleInfo));
	info->exportNameToOrdinal = hashmap_create();
	info->threadNotificationsEnabled = true;
	info->tlsInfo.index = kInvalidTlsIndex;
	info->tlsInfo.loaderIndex = kInvalidTlsIndex;
	info->tlsInfo.threadAllocations = hashmap_create();
	vec_push(&g_allModules, info);
	return info;
}

static void pinAlias(ModuleRegistry *reg, const char *alias) {
	if (alias && *alias) {
		hashmap_put(reg->pinnedAliases, alias, strlen(alias), (void *)1);
	}
}

static void pinModuleInfo(ModuleRegistry *reg, ModuleInfo *info) {
	hashmap_put(reg->pinnedModules, &info, sizeof(info), (void *)1);
}

static void registerAlias(ModuleRegistry *reg, const char *alias, ModuleInfo *info) {
	if (!alias || !*alias || !info) {
		return;
	}
	size_t aliasLen = strlen(alias);
	void *existing = NULL;
	if (!hashmap_find(reg->modulesByAlias, alias, aliasLen, &existing)) {
		hashmap_put(reg->modulesByAlias, alias, aliasLen, info);
		return;
	}
	if (hashmap_find(reg->pinnedAliases, alias, aliasLen, NULL)) {
		return;
	}
	// Prefer externally loaded modules over built-ins when both are present.
	const ModuleInfo *current = existing;
	if (current && current->moduleStub != NULL && info->moduleStub == NULL) {
		hashmap_put(reg->modulesByAlias, alias, aliasLen, info);
	}
}

static ModuleInfo *findByAlias(ModuleRegistry *reg, const char *alias) {
	void *value = NULL;
	if (hashmap_find(reg->modulesByAlias, alias, strlen(alias), &value)) {
		return value;
	}
	return NULL;
}

// Returns a malloc'd key; the caller owns it.
static char *storageKeyForPath(const char *path) {
	char *canonical = files_canonicalPath(path);
	char *windows = files_pathToWindows(canonical ? canonical : path);
	free(canonical);
	char *key = normalizeAlias(windows ? windows : "");
	free(windows);
	return key;
}

static void registerExternalModuleAliases(ModuleRegistry *reg, const char *requestedName, const char *resolvedPath,
										  ModuleInfo *info) {
	ParsedModuleName parsed = parseModuleName(requestedName);
	char *baseAlias = normalizedBaseKey(&parsed);
	registerAlias(reg, baseAlias, info);
	free(baseAlias);
	parsedModuleNameFree(&parsed);

	char *alias = normalizeAlias(requestedName);
	registerAlias(reg, alias, info);
	free(alias);

	char *pathKey = storageKeyForPath(resolvedPath);
	registerAlias(reg, pathKey, info);
	free(pathKey);
}

static void registerBuiltinModule(ModuleRegistry *reg, const ModuleStub *module) {
	if (!module) {
		return;
	}

	ModuleInfo *info = createModuleInfo();
	info->handle = g_nextStubHandle++;
	info->moduleStub = module;
	info->executable = NULL;
	info->refCount = UINT_MAX;
	info->originalName = xstrdup(module->names[0] ? module->names[0] : "");
	ParsedModuleName parsed = parseModuleName(info->originalName);
	info->normalizedName = normalizedBaseKey(&parsed);
	parsedModuleNameFree(&parsed);
	info->exportsInitialized = false;
	hashmap_put(reg->modulesByKey, info->normalizedName, strlen(info->normalizedName), info);

	const bool pinModule = (module == &lib_lmgr);
	if (pinModule) {
		pinModuleInfo(reg, info);
	}
	for (size_t i = 0; module->names[i]; ++i) {
		char *alias = normalizeAlias(module->names[i]);
		if (pinModule) {
			pinAlias(reg, alias);
		}
		registerAlias(reg, alias, info);
		hashmap_put(reg->builtinAliasMap, alias, strlen(alias), info);

		ParsedModuleName nameParsed = parseModuleName(module->names[i]);
		char *baseAlias = normalizedBaseKey(&nameParsed);
		parsedModuleNameFree(&nameParsed);
		if (strcmp(baseAlias, alias) != 0) {
			if (pinModule) {
				pinAlias(reg, baseAlias);
			}
			registerAlias(reg, baseAlias, info);
			hashmap_put(reg->builtinAliasMap, baseAlias, strlen(baseAlias), info);
		}
		free(baseAlias);
		free(alias);
	}
}

static BOOL invokeDllMain(ModuleInfo *info, DWORD callReason, LPVOID callReserved, bool force) {
	if (!force) {
		if (callReason == DLL_PROCESS_DETACH && (!info->processAttachCalled || !info->processAttachSucceeded)) {
			return TRUE;
		}
		if ((callReason == DLL_THREAD_ATTACH || callReason == DLL_THREAD_DETACH) &&
			(!info->processAttachCalled || !info->processAttachSucceeded || !info->threadNotificationsEnabled)) {
			return TRUE;
		}
	}

	DEBUG_LOG("  callDllMain: invoking DllMain(%p, %u, %p) for %s\n",
			  (void *)(uintptr_t)toGuestPtr(info->executable->imageBase), callReason, callReserved,
			  info->normalizedName);

	BOOL result = call_DllEntryProc((DllEntryProc)info->executable->entryPoint, toGuestPtr(info->executable->imageBase),
									callReason, callReserved);
	DEBUG_LOG("  callDllMain: %s DllMain returned %d\n", info->normalizedName, result);
	return result;
}

static BOOL callDllMain(ModuleInfo *info, DWORD reason, LPVOID reserved) {
	if (info == wibo_mainModule) {
		return TRUE;
	}
	if (!info->executable || !info->executable->entryPoint) {
		return TRUE;
	}
	if (info->executable->is64Bit != GUEST_IS_64BIT) {
		DEBUG_LOG("  callDllMain: %s is %s; not loadable by this runtime\n", info->normalizedName,
		          info->executable->is64Bit ? "PE32+" : "PE32");
		kernel32_setLastError(ERROR_BAD_EXE_FORMAT);
		return FALSE;
	}

	kernel32_setLastError(ERROR_SUCCESS);

	switch (reason) {
	case DLL_PROCESS_ATTACH: {
		if (info->processAttachCalled) {
			return info->processAttachSucceeded ? TRUE : FALSE;
		}
		info->processAttachCalled = true;
		info->processAttachSucceeded = false;
		BOOL result = invokeDllMain(info, DLL_PROCESS_ATTACH, reserved, true);
		if (!result) {
			invokeDllMain(info, DLL_PROCESS_DETACH, NULL, true);
			return FALSE;
		}
		info->processAttachSucceeded = true;
		return TRUE;
	}
	case DLL_PROCESS_DETACH: {
		BOOL result = invokeDllMain(info, DLL_PROCESS_DETACH, reserved, false);
		info->processAttachSucceeded = false;
		return result;
	}
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
		return invokeDllMain(info, reason, reserved, false);
	default:
		break;
	}

	return TRUE;
}

static bool shouldDeliverThreadNotifications(const ModuleInfo *info) {
	if (info == wibo_mainModule) {
		return false;
	}
	if (!info->executable) {
		return false;
	}
	if (!info->processAttachCalled || !info->processAttachSucceeded) {
		return false;
	}
	return info->threadNotificationsEnabled;
}

static void ensureExportsInitialized(ModuleInfo *info) {
	if (info->exportsInitialized || !info->executable) {
		return;
	}
	Executable *exe = info->executable;
	if (!exe->exportDirectoryRVA || !exe->exportDirectorySize) {
		info->exportsInitialized = true;
		return;
	}

	PEExportDirectory *dir = Executable_fromRVA(exe, exe->exportDirectoryRVA);
	info->exportOrdinalBase = dir->base;
	uint32_t functionCount = dir->numberOfFunctions;
	vec_clear(&info->exportsByOrdinal);
	if (functionCount) {
		vec_reserve(&info->exportsByOrdinal, functionCount);
		info->exportsByOrdinal.len = functionCount;
		memset(info->exportsByOrdinal.data, 0, functionCount * sizeof(void *));
		uint32_t *functions = Executable_fromRVA(exe, dir->addressOfFunctions);
		for (uint32_t i = 0; i < functionCount; ++i) {
			uint32_t rva = functions[i];
			if (!rva) {
				continue;
			}
			if (rva >= exe->exportDirectoryRVA && rva < exe->exportDirectoryRVA + exe->exportDirectorySize) {
				const char *forward = Executable_fromRVA(exe, rva);
				info->exportsByOrdinal.data[i] = resolveForwardedExport(info, forward);
			} else {
				info->exportsByOrdinal.data[i] = Executable_fromRVA(exe, rva);
			}
		}
	}

	uint32_t nameCount = dir->numberOfNames;
	if (nameCount) {
		uint32_t *names = Executable_fromRVA(exe, dir->addressOfNames);
		uint16_t *ordinals = Executable_fromRVA(exe, dir->addressOfNameOrdinals);
		for (uint32_t i = 0; i < nameCount; ++i) {
			uint16_t index = ordinals[i];
			uint16_t ordinal = (uint16_t)(dir->base + index);
			if (index < info->exportsByOrdinal.len) {
				const char *namePtr = Executable_fromRVA(exe, names[i]);
				hashmap_put(info->exportNameToOrdinal, namePtr, strlen(namePtr), (void *)(uintptr_t)ordinal);
			}
		}
	}
	info->exportsInitialized = true;
}

static bool ensureModuleReady(ModuleInfo *info) {
	if (info->moduleStub && info->moduleStub->dllDataSize > 0 && !info->executable) {
		DEBUG_LOG("registerBuiltinModule: loading PE for %s\n", info->originalName);
		Executable *exec = Executable_new();
		if (!Executable_loadPEImage(exec, info->moduleStub->dllData, info->moduleStub->dllDataSize, true)) {
			DEBUG_LOG("  loadPE failed for %s\n", info->originalName);
			Executable_free(exec);
			return false;
		}
		if (exec->is64Bit != GUEST_IS_64BIT) {
			// Embedded CRT images are PE32, so a Win64 runtime keeps only the stub exports for them.
			DEBUG_LOG("  %s is not %s; using stub exports only\n", info->originalName,
			          GUEST_IS_64BIT ? "PE32+" : "PE32");
			Executable_free(exec);
		} else {
			info->executable = exec;
		}
	}
	ensureExportsInitialized(info);
	if (!info->executable) {
		return true;
	}
	if (!Executable_resolveImports(info->executable)) {
		return false;
	}
	if (!wibo_initializeModuleTls(info)) {
		return false;
	}
	if (!callDllMain(info, DLL_PROCESS_ATTACH, NULL)) {
		kernel32_setLastError(ERROR_DLL_INIT_FAILED);
		return false;
	}
	return true;
}

void CDECL entry_stubBase(SIZE_T index) {
	const char *func = g_stubFuncNames[index] && g_stubFuncNames[index][0] ? g_stubFuncNames[index] : "<unknown>";
	const char *dll = g_stubDlls[index] && g_stubDlls[index][0] ? g_stubDlls[index] : "<unknown>";
	fprintf(stderr, "wibo: call reached missing import %s from %s\n", func, dll);
	fflush(stderr);
	abort();
}

static void appendValueCallback(const void *key, size_t keylen, void *val, void *ctx) {
	(void)key;
	(void)keylen;
	vec_push((PtrVec *)ctx, val);
}

void wibo_initializeModuleRegistry(void) {
	lockRegistry();
	unlockRegistry();
}

ModuleInfo *wibo_registerProcessModule(Executable *executable, const char *resolvedPath, const char *originalName) {
	if (!executable) {
		return NULL;
	}

	const char *effectiveOriginal = originalName ? originalName : "";
	if (effectiveOriginal[0] == '\0' && resolvedPath && resolvedPath[0] != '\0') {
		const char *slash = strrchr(resolvedPath, '/');
		effectiveOriginal = slash ? slash + 1 : resolvedPath;
	}

	ParsedModuleName parsed = parseModuleName(effectiveOriginal);

	ModuleRegistry *reg = lockRegistry();
	ModuleInfo *info = createModuleInfo();
	info->handle = toGuestPtr(executable->imageBase);
	info->moduleStub = NULL;
	info->originalName = xstrdup(effectiveOriginal);
	info->normalizedName = normalizedBaseKey(&parsed);
	info->resolvedPath = xstrdup(resolvedPath ? resolvedPath : "");
	info->executable = executable;
	info->refCount = UINT_MAX;

	char *storageKey = NULL;
	if (info->resolvedPath[0] != '\0') {
		storageKey = storageKeyForPath(info->resolvedPath);
	} else if (info->normalizedName[0] != '\0') {
		storageKey = xstrdup(info->normalizedName);
	}
	if (!storageKey || storageKey[0] == '\0') {
		free(storageKey);
		storageKey = normalizeAlias(info->originalName);
	}
	hashmap_put(reg->modulesByKey, storageKey, strlen(storageKey), info);

	if (info->resolvedPath[0] != '\0') {
		registerExternalModuleAliases(reg, info->originalName, info->resolvedPath, info);
	} else {
		char *alias = normalizeAlias(info->originalName);
		registerAlias(reg, alias, info);
		free(alias);
		char *baseAlias = normalizedBaseKey(&parsed);
		if (strcmp(baseAlias, info->originalName) != 0) {
			registerAlias(reg, baseAlias, info);
		}
		free(baseAlias);
	}

	ensureExportsInitialized(info);

	pinModuleInfo(reg, info);
	pinAlias(reg, storageKey);
	char *pinnedOriginal = normalizeAlias(info->originalName);
	pinAlias(reg, pinnedOriginal);
	free(pinnedOriginal);
	// The C++ pins a normalized name that was already moved out (empty), so nothing is pinned for it.

	free(storageKey);
	parsedModuleNameFree(&parsed);
	unlockRegistry();
	return info;
}

void wibo_shutdownModuleRegistry(void) {
	ModuleRegistry *reg = lockRegistry();
	PtrVec modules = {0};
	hashmap_foreach(reg->modulesByKey, appendValueCallback, &modules);
	for (size_t i = 0; i < modules.len; ++i) {
		ModuleInfo *info = modules.data[i];
		if (!info || info->moduleStub) {
			continue;
		}
		if (info->tlsInfo.hasTls) {
			runModuleTlsCallbacks(info, TLS_PROCESS_DETACH);
		}
		if (info->processAttachCalled && info->processAttachSucceeded) {
			callDllMain(info, DLL_PROCESS_DETACH, (LPVOID)(uintptr_t)1);
		}
		wibo_releaseModuleTls(info);
	}
	vec_free(&modules);

	hashmap_free(reg->modulesByKey);
	reg->modulesByKey = hashmap_create();
	hashmap_free(reg->modulesByAlias);
	reg->modulesByAlias = hashmap_create();
	free(reg->dllDirectory);
	reg->dllDirectory = NULL;
	reg->initialized = false;
	unlockRegistry();
}

typedef struct {
	HMODULE module;
	ModuleInfo *result;
} ModuleHandleSearch;

static void findModuleByHandleCallback(const void *key, size_t keylen, void *val, void *ctx) {
	(void)key;
	(void)keylen;
	ModuleHandleSearch *search = ctx;
	ModuleInfo *info = val;
	if (search->result || !info) {
		return;
	}
	if (info->handle == search->module ||
		(info->executable && (uintptr_t)info->executable->imageBase == (uintptr_t)search->module)) {
		search->result = info;
	}
}

ModuleInfo *wibo_moduleInfoFromHandle(HMODULE module) {
	if (wibo_isMainModule(module)) {
		return wibo_mainModule;
	}
	if (!module) {
		return NULL;
	}
	ModuleRegistry *reg = lockRegistry();
	ModuleHandleSearch search = {module, NULL};
	hashmap_foreach(reg->modulesByKey, findModuleByHandleCallback, &search);
	unlockRegistry();
	return search.result;
}

void wibo_setDllDirectoryOverride(const char *path) {
	char *canonical = files_canonicalPath(path);
	ModuleRegistry *reg = lockRegistry();
	free(reg->dllDirectory);
	reg->dllDirectory = canonical;
	unlockRegistry();
}

void wibo_clearDllDirectoryOverride(void) {
	ModuleRegistry *reg = lockRegistry();
	free(reg->dllDirectory);
	reg->dllDirectory = NULL;
	unlockRegistry();
}

char *wibo_dllDirectoryOverride(void) {
	ModuleRegistry *reg = lockRegistry();
	char *copy = reg->dllDirectory ? xstrdup(reg->dllDirectory) : NULL;
	unlockRegistry();
	return copy;
}

typedef struct {
	const uint8_t *address;
	ModuleInfo *result;
} ModuleAddressSearch;

static void findModuleByAddressCallback(const void *key, size_t keylen, void *val, void *ctx) {
	(void)key;
	(void)keylen;
	ModuleAddressSearch *search = ctx;
	ModuleInfo *info = val;
	if (search->result || !info || !info->executable) {
		return;
	}
	const uint8_t *base = info->executable->imageBase;
	size_t size = info->executable->imageSize;
	if (!base || size == 0) {
		return;
	}
	if (search->address >= base && search->address < base + size) {
		search->result = info;
	}
}

ModuleInfo *wibo_moduleInfoFromAddress(void *addr) {
	if (!addr) {
		return NULL;
	}
	ModuleRegistry *reg = lockRegistry();
	ModuleAddressSearch search = {addr, NULL};
	hashmap_foreach(reg->modulesByKey, findModuleByAddressCallback, &search);
	unlockRegistry();
	return search.result;
}

static void detachThreadTlsBlock(ModuleInfo *module, TEB *tib, GUEST_PTR block) {
	ModuleTlsInfo *tls = &module->tlsInfo;
	if (tib) {
		if (wibo_tls_getTibValue(tib, tls->index) == block) {
			if (!wibo_tls_setTibValue(tib, tls->index, GUEST_NULL)) {
				DEBUG_LOG("  detachThreadTlsBlock: failed to clear TLS pointer for %s (index %u)\n",
						  module->originalName, tls->index);
			}
		}
		if (tls->loaderIndex != kInvalidTlsIndex) {
			wibo_tls_clearModulePointer(tib, tls->loaderIndex);
		}
	}
	if (block) {
		wibo_heap_guestFree(fromGuestPtr(block));
	}
}

static void detachThreadAllocationCallback(const void *key, size_t keylen, void *val, void *ctx) {
	(void)keylen;
	TEB *tib;
	memcpy(&tib, key, sizeof(tib));
	detachThreadTlsBlock(ctx, tib, (GUEST_PTR)(uintptr_t)val);
}

static void releaseThreadAllocations(ModuleInfo *module) {
	ModuleTlsInfo *tls = &module->tlsInfo;
	hashmap_foreach(tls->threadAllocations, detachThreadAllocationCallback, module);
	hashmap_free(tls->threadAllocations);
	tls->threadAllocations = hashmap_create();
}

static bool allocateModuleTlsForThread(ModuleInfo *module, TEB *tib) {
	if (!tib) {
		return true;
	}
	ModuleTlsInfo *info = &module->tlsInfo;
	if (!info->hasTls || info->index == kInvalidTlsIndex) {
		return true;
	}
	if (hashmap_find(info->threadAllocations, &tib, sizeof(tib), NULL)) {
		return true;
	}
	GUEST_PTR block = GUEST_NULL;
	const size_t allocationSize = info->allocationSize;
	if (allocationSize > 0) {
		void *ptr = wibo_heap_guestMalloc(allocationSize);
		if (!ptr) {
			DEBUG_LOG("  allocateModuleTlsForThread: failed to allocate %zu bytes for %s\n", allocationSize,
					  module->originalName);
			return false;
		}
		memset(ptr, 0, allocationSize);
		if (info->templateData && info->templateSize > 0) {
			memcpy(ptr, info->templateData, info->templateSize);
		}
		block = toGuestPtr(ptr);
	}
	hashmap_put(info->threadAllocations, &tib, sizeof(tib), (void *)(uintptr_t)block);
	if (!wibo_tls_setTibValue(tib, info->index, block)) {
		DEBUG_LOG("  allocateModuleTlsForThread: failed to publish TLS pointer for %s (index %u)\n",
				  module->originalName, info->index);
	}
	if (info->loaderIndex != kInvalidTlsIndex) {
		if (!wibo_tls_setModulePointer(tib, info->loaderIndex, block)) {
			DEBUG_LOG("  allocateModuleTlsForThread: failed to update module pointer for %s (slot %u)\n",
					  module->originalName, info->loaderIndex);
		}
	}
	return true;
}

static void freeModuleTlsForThread(ModuleInfo *module, TEB *tib) {
	if (!tib || !module->tlsInfo.hasTls) {
		return;
	}
	void *value = NULL;
	if (!hashmap_remove(module->tlsInfo.threadAllocations, &tib, sizeof(tib), &value)) {
		return;
	}
	detachThreadTlsBlock(module, tib, (GUEST_PTR)(uintptr_t)value);
}

static void runModuleTlsCallbacks(ModuleInfo *module, DWORD reason) {
	const ModuleTlsInfo *tls = &module->tlsInfo;
	if (!tls->hasTls || tls->callbacks.len == 0) {
		return;
	}
	for (size_t i = 0; i < tls->callbacks.len; ++i) {
		PIMAGE_TLS_CALLBACK callback = tls->callbacks.data[i];
		if (!callback) {
			continue;
		}
		call_PIMAGE_TLS_CALLBACK(callback, (PVOID)(uintptr_t)module->handle, reason, NULL);
	}
}

typedef struct {
	ModuleInfo *module;
	bool success;
} TlsAllocContext;

static void allocateTlsForThreadCallback(TEB *tib, void *opaque) {
	TlsAllocContext *ctx = opaque;
	if (!ctx->success) {
		return;
	}
	if (!allocateModuleTlsForThread(ctx->module, tib)) {
		ctx->success = false;
	}
}

bool wibo_initializeModuleTls(ModuleInfo *module) {
	if (module->tlsInfo.hasTls) {
		return true;
	}
	Executable *exec = module->executable;
	if (!exec) {
		return true;
	}
	const size_t minTlsDirectorySize = exec->is64Bit ? kMinTlsDirectorySize64 : kMinTlsDirectorySize32;
	if (exec->tlsDirectoryRVA == 0 || exec->tlsDirectorySize < minTlsDirectorySize) {
		return true;
	}
	const uint8_t *tlsDirectoryRaw = Executable_fromRVA(exec, exec->tlsDirectoryRVA);
	if (!tlsDirectoryRaw) {
		return false;
	}
	// The directory may be smaller than the full struct; the remaining fields stay zero.
	TlsDirectoryFields tlsDirectory = {0};
	if (exec->is64Bit) {
		ImageTlsDirectory64 raw = {0};
		memcpy(&raw, tlsDirectoryRaw, exec->tlsDirectorySize < sizeof(raw) ? exec->tlsDirectorySize : sizeof(raw));
		tlsDirectory = (TlsDirectoryFields){raw.StartAddressOfRawData, raw.EndAddressOfRawData, raw.AddressOfIndex,
		                                    raw.AddressOfCallBacks, raw.SizeOfZeroFill, raw.Characteristics};
	} else {
		ImageTlsDirectory32 raw = {0};
		memcpy(&raw, tlsDirectoryRaw, exec->tlsDirectorySize < sizeof(raw) ? exec->tlsDirectorySize : sizeof(raw));
		tlsDirectory = (TlsDirectoryFields){raw.StartAddressOfRawData, raw.EndAddressOfRawData, raw.AddressOfIndex,
		                                    raw.AddressOfCallBacks, raw.SizeOfZeroFill, raw.Characteristics};
	}

	ModuleTlsInfo *info = &module->tlsInfo;
	info->templateSize = (tlsDirectory.EndAddressOfRawData > tlsDirectory.StartAddressOfRawData)
							 ? tlsDirectory.EndAddressOfRawData - tlsDirectory.StartAddressOfRawData
							 : 0;
	info->zeroFillSize = tlsDirectory.SizeOfZeroFill;
	info->characteristics = tlsDirectory.Characteristics;
	info->templateData = (uint8_t *)resolveModuleAddress(exec, (uintptr_t)tlsDirectory.StartAddressOfRawData);
	info->indexLocation = (DWORD *)resolveModuleAddress(exec, (uintptr_t)tlsDirectory.AddressOfIndex);
	vec_clear(&info->callbacks);
	uintptr_t callbacksArray = resolveModuleAddress(exec, (uintptr_t)tlsDirectory.AddressOfCallBacks);
	if (callbacksArray) {
		const GUEST_PTR *callbackPtr = (const GUEST_PTR *)callbacksArray;
		while (*callbackPtr) {
			vec_push(&info->callbacks, (PIMAGE_TLS_CALLBACK)resolveModuleAddress(exec, *callbackPtr));
			++callbackPtr;
		}
	}
	info->allocationSize = info->templateSize + info->zeroFillSize;
	hashmap_free(info->threadAllocations);
	info->threadAllocations = hashmap_create();

	ModuleRegistry *reg = lockRegistry();
	DWORD loaderIndex = allocateModuleTlsSlot(reg, module);
	if (!wibo_tls_ensureModulePointerCapacity(reg->tlsModuleSlots.len)) {
		releaseModuleTlsSlot(reg, loaderIndex);
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		unlockRegistry();
		return false;
	}
	info->loaderIndex = loaderIndex;
	if (info->indexLocation) {
		*info->indexLocation = loaderIndex;
	}

	DWORD apiIndex = wibo_tls_reserveSlot();
	if (apiIndex == kInvalidTlsIndex) {
		releaseModuleTlsSlot(reg, loaderIndex);
		info->loaderIndex = kInvalidTlsIndex;
		if (info->indexLocation) {
			*info->indexLocation = 0;
		}
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		unlockRegistry();
		return false;
	}
	info->index = apiIndex;
	info->hasTls = true;

	TlsAllocContext ctx = {module, true};
	wibo_tls_forEachTib(allocateTlsForThreadCallback, &ctx);
	if (!ctx.success) {
		releaseThreadAllocations(module);
		wibo_tls_releaseSlot(info->index);
		info->index = kInvalidTlsIndex;
		releaseModuleTlsSlot(reg, loaderIndex);
		info->loaderIndex = kInvalidTlsIndex;
		info->hasTls = false;
		if (info->indexLocation) {
			*info->indexLocation = 0;
		}
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		unlockRegistry();
		return false;
	}
	runModuleTlsCallbacks(module, TLS_PROCESS_ATTACH);
	kernel32_setLastError(ERROR_SUCCESS);
	unlockRegistry();
	return true;
}

void wibo_releaseModuleTls(ModuleInfo *module) {
	ModuleTlsInfo *info = &module->tlsInfo;
	if (!info->hasTls) {
		return;
	}
	releaseThreadAllocations(module);
	if (info->index != kInvalidTlsIndex) {
		wibo_tls_releaseSlot(info->index);
	}
	ModuleRegistry *reg = lockRegistry();
	if (info->loaderIndex != kInvalidTlsIndex) {
		releaseModuleTlsSlot(reg, info->loaderIndex);
	}
	unlockRegistry();
	vec_free(&info->callbacks);
	hashmap_free(info->threadAllocations);
	memset(info, 0, sizeof(*info));
	info->index = kInvalidTlsIndex;
	info->loaderIndex = kInvalidTlsIndex;
	info->threadAllocations = hashmap_create();
}

static void collectThreadNotificationTargetsCallback(const void *key, size_t keylen, void *val, void *ctx) {
	(void)key;
	(void)keylen;
	ModuleInfo *info = val;
	PtrVec *targets = ctx;
	if (info && shouldDeliverThreadNotifications(info)) {
		vec_push(targets, info);
	}
}

// Must be called with the registry lock held.
static void collectThreadNotificationTargets(ModuleRegistry *reg, PtrVec *targets) {
	hashmap_foreach(reg->modulesByKey, collectThreadNotificationTargetsCallback, targets);
}

void wibo_notifyDllThreadAttach(void) {
	ModuleRegistry *reg = lockRegistry();
	PtrVec targets = {0};
	collectThreadNotificationTargets(reg, &targets);
	for (size_t i = 0; i < targets.len; ++i) {
		ModuleInfo *info = targets.data[i];
		if (info->tlsInfo.hasTls) {
			if (!allocateModuleTlsForThread(info, currentThreadTeb)) {
				DEBUG_LOG("notifyDllThreadAttach: failed to allocate TLS for %s\n", info->originalName);
			}
			runModuleTlsCallbacks(info, TLS_THREAD_ATTACH);
		}
	}
	for (size_t i = 0; i < targets.len; ++i) {
		callDllMain(targets.data[i], DLL_THREAD_ATTACH, NULL);
	}
	kernel32_setLastError(ERROR_SUCCESS);
	vec_free(&targets);
	unlockRegistry();
}

void wibo_notifyDllThreadDetach(void) {
	ModuleRegistry *reg = lockRegistry();
	PtrVec targets = {0};
	collectThreadNotificationTargets(reg, &targets);
	for (size_t i = targets.len; i-- > 0;) {
		ModuleInfo *info = targets.data[i];
		if (info->tlsInfo.hasTls) {
			runModuleTlsCallbacks(info, TLS_THREAD_DETACH);
		}
	}
	for (size_t i = targets.len; i-- > 0;) {
		callDllMain(targets.data[i], DLL_THREAD_DETACH, NULL);
	}
	for (size_t i = targets.len; i-- > 0;) {
		ModuleInfo *info = targets.data[i];
		if (info->tlsInfo.hasTls) {
			freeModuleTlsForThread(info, currentThreadTeb);
		}
	}
	kernel32_setLastError(ERROR_SUCCESS);
	vec_free(&targets);
	unlockRegistry();
}

BOOL wibo_disableThreadNotifications(ModuleInfo *info) {
	if (!info) {
		return FALSE;
	}
	if (info->tlsInfo.hasTls) {
		DEBUG_LOG("disableThreadNotifications: %s uses static TLS\n", info->originalName);
		return FALSE;
	}
	lockRegistry();
	info->threadNotificationsEnabled = false;
	unlockRegistry();
	return TRUE;
}

ModuleInfo *wibo_findLoadedModule(const char *name) {
	if (!name || *name == '\0') {
		return wibo_mainModule;
	}
	ModuleRegistry *reg = lockRegistry();
	ParsedModuleName parsed = parseModuleName(name);
	char *alias = normalizedBaseKey(&parsed);
	ModuleInfo *info = findByAlias(reg, alias);
	free(alias);
	if (!info) {
		char *fallback = normalizeAlias(name);
		info = findByAlias(reg, fallback);
		free(fallback);
	}
	parsedModuleNameFree(&parsed);
	unlockRegistry();
	return info;
}

static void freeStringPtrs(PtrVec *strings) {
	for (size_t i = 0; i < strings->len; ++i) {
		free(strings->data[i]);
	}
	vec_free(strings);
}

static void candidateModuleNames(const ParsedModuleName *parsed, PtrVec *names) {
	if (parsed->base[0] == '\0') {
		return;
	}
	vec_push(names, xstrdup(parsed->base));
	if (parsedModuleNameWantsDllSuffix(parsed)) {
		size_t baseLen = strlen(parsed->base);
		char *withSuffix = xmalloc(baseLen + sizeof(".dll"));
		memcpy(withSuffix, parsed->base, baseLen);
		memcpy(withSuffix + baseLen, ".dll", sizeof(".dll"));
		vec_push(names, withSuffix);
	}
}

static char *combineAndFind(const char *directory, const char *filename) {
	if (!filename || *filename == '\0' || !directory || *directory == '\0') {
		return NULL;
	}
	return files_findCaseInsensitiveFile(directory, filename);
}

static void addSearchDirectory(hashmap *seen, PtrVec *dirs, const char *dir) {
	if (!dir || *dir == '\0') {
		return;
	}
	char *canonical = files_canonicalPath(dir);
	struct stat st;
	if (!canonical || *canonical == '\0' || stat(canonical, &st) != 0) {
		free(canonical);
		return;
	}
	char *key = stringToLower(canonical, strlen(canonical));
	size_t keyLen = strlen(key);
	if (hashmap_find(seen, key, keyLen, NULL)) {
		free(key);
		free(canonical);
		return;
	}
	hashmap_put(seen, key, keyLen, (void *)1);
	free(key);
	vec_push(dirs, canonical);
}

static void addSearchPathList(hashmap *seen, PtrVec *dirs, const char *envVar) {
	const char *pathList = getenv(envVar);
	if (!pathList) {
		return;
	}
	const char *cursor = pathList;
	while (*cursor) {
		size_t len = strcspn(cursor, ":;");
		if (len > 0) {
			char *piece = xstrndup(cursor, len);
			char *converted = files_pathFromWindows(piece);
			addSearchDirectory(seen, dirs, converted && *converted ? converted : piece);
			free(converted);
			free(piece);
		}
		cursor += len;
		if (*cursor) {
			cursor++;
		}
	}
}

static void collectSearchDirectories(ModuleRegistry *reg, bool alteredSearchPath, PtrVec *dirs) {
	hashmap *seen = hashmap_create();

	if (wibo_guestExecutablePath && *wibo_guestExecutablePath != '\0') {
		const char *slash = strrchr(wibo_guestExecutablePath, '/');
		if (slash) {
			size_t parentLen = slash == wibo_guestExecutablePath ? 1 : (size_t)(slash - wibo_guestExecutablePath);
			char *parent = xstrndup(wibo_guestExecutablePath, parentLen);
			addSearchDirectory(seen, dirs, parent);
			free(parent);
		}
	}

	if (reg->dllDirectory) {
		addSearchDirectory(seen, dirs, reg->dllDirectory);
	}

	if (!alteredSearchPath) {
		char *cwd = getcwd(NULL, 0);
		addSearchDirectory(seen, dirs, cwd);
		free(cwd);
	}

	addSearchPathList(seen, dirs, "WIBO_PATH");
	addSearchPathList(seen, dirs, "WINEPATH"); // Wine compatibility

	hashmap_free(seen);
}

// Resolves a Windows-style directory and candidate name; returns a malloc'd canonical path or NULL.
static char *findWindowsPathOnDisk(const char *directory, const char *candidate) {
	size_t len = strlen(directory) + sizeof("\\") + strlen(candidate);
	char *combined = xmalloc(len);
	snprintf(combined, len, "%s\\%s", directory, candidate);
	char *posixPath = files_pathFromWindows(combined);
	free(combined);
	if (!posixPath || *posixPath == '\0') {
		free(posixPath);
		return NULL;
	}

	char *parent = NULL;
	const char *filename = NULL;
	const char *slash = strrchr(posixPath, '/');
	if (slash) {
		parent = xstrndup(posixPath, slash == posixPath ? 1 : (size_t)(slash - posixPath));
		filename = slash + 1;
	} else {
		parent = xstrdup("");
		filename = posixPath;
	}
	char *found = files_findCaseInsensitiveFile(parent, filename);
	free(parent);
	char *resolved = NULL;
	if (found) {
		resolved = files_canonicalPath(found);
		free(found);
	}
	free(posixPath);
	return resolved;
}

// Returns a malloc'd canonical path, or NULL if the module is not found on disk.
static char *resolveModuleOnDisk(ModuleRegistry *reg, const char *requestedName, bool alteredSearchPath) {
	ParsedModuleName parsed = parseModuleName(requestedName);
	PtrVec names = {0};
	candidateModuleNames(&parsed, &names);
	char *resolved = NULL;

	if (parsed.directory[0] != '\0') {
		for (size_t i = 0; i < names.len && !resolved; ++i) {
			resolved = findWindowsPathOnDisk(parsed.directory, names.data[i]);
		}
	} else {
		PtrVec dirs = {0};
		collectSearchDirectories(reg, alteredSearchPath, &dirs);
		for (size_t d = 0; d < dirs.len && !resolved; ++d) {
			for (size_t i = 0; i < names.len && !resolved; ++i) {
				char *found = combineAndFind(dirs.data[d], names.data[i]);
				if (found) {
					resolved = files_canonicalPath(found);
					free(found);
				}
			}
		}
		freeStringPtrs(&dirs);
	}

	freeStringPtrs(&names);
	parsedModuleNameFree(&parsed);
	return resolved;
}

static void removeAliasesToModule(ModuleRegistry *reg, const ModuleInfo *info);

typedef struct {
	const char *dllName;
	ParsedModuleName parsed;
	char *alias;
	ModuleRegistry *reg;
	DWORD diskError;
	bool locked;
} LoadContext;

static void loadLock(LoadContext *ctx) {
	lockRegistry();
	ctx->locked = true;
}

static void loadUnlock(LoadContext *ctx) {
	unlockRegistry();
	ctx->locked = false;
}

static ModuleInfo *findAliasInMap(hashmap *map, const char *alias) {
	void *value = NULL;
	if (hashmap_find(map, alias, strlen(alias), &value)) {
		return value;
	}
	return NULL;
}

static ModuleInfo *tryLoadExternal(LoadContext *ctx, const char *path) {
	ModuleRegistry *reg = ctx->reg;
	char *key = storageKeyForPath(path);
	size_t keyLen = strlen(key);

	void *existingValue = NULL;
	if (hashmap_find(reg->modulesByKey, key, keyLen, &existingValue)) {
		ModuleInfo *existing = existingValue;
		free(key);
		if (existing->refCount != UINT_MAX) {
			existing->refCount++;
		}
		char *canonical = files_canonicalPath(path);
		registerExternalModuleAliases(reg, ctx->dllName, canonical, existing);
		free(canonical);
		return existing;
	}
	loadUnlock(ctx);

	DEBUG_LOG("  loading external module from %s\n", path);
	FILE *file = wibo_openImage(path);
	if (!file) {
		perror("loadModule");
		free(key);
		loadLock(ctx);
		ctx->diskError = ERROR_MOD_NOT_FOUND;
		return NULL;
	}

	Executable *executable = Executable_new();
	if (!Executable_loadPEFile(executable, file, true)) {
		DEBUG_LOG("  loadPE failed for %s\n", path);
		fclose(file);
		Executable_free(executable);
		free(key);
		loadLock(ctx);
		ctx->diskError = ERROR_BAD_EXE_FORMAT;
		return NULL;
	}
	fclose(file);

	loadLock(ctx);
	ModuleInfo *info = createModuleInfo();
	info->handle = g_nextStubHandle++;
	info->moduleStub = NULL;
	info->originalName = xstrdup(ctx->dllName);
	info->normalizedName = normalizedBaseKey(&ctx->parsed);
	info->resolvedPath = files_canonicalPath(path);
	info->executable = executable;
	info->refCount = 1;

	hashmap_put(reg->modulesByKey, key, keyLen, info);
	registerExternalModuleAliases(reg, ctx->dllName, info->resolvedPath, info);
	if (executable->isDll) {
		loadUnlock(ctx);
		ensureExportsInitialized(info);
		if (!Executable_resolveImports(executable)) {
			DEBUG_LOG("  resolveImports failed for %s\n", info->originalName);
			loadLock(ctx);
			hashmap_remove(reg->modulesByKey, key, keyLen, NULL);
			ctx->diskError = kernel32_getLastError();
			free(key);
			return NULL;
		}
		if (!wibo_initializeModuleTls(info)) {
			DEBUG_LOG("  initializeModuleTls failed for %s\n", info->originalName);
			loadLock(ctx);
			hashmap_remove(reg->modulesByKey, key, keyLen, NULL);
			ctx->diskError = kernel32_getLastError();
			free(key);
			return NULL;
		}
		loadLock(ctx);
		if (!callDllMain(info, DLL_PROCESS_ATTACH, NULL)) {
			DEBUG_LOG("  DllMain failed for %s\n", info->originalName);
			wibo_releaseModuleTls(info);
			removeAliasesToModule(reg, info);
			hashmap_remove(reg->pinnedModules, &info, sizeof(info), NULL);
			hashmap_remove(reg->modulesByKey, key, keyLen, NULL);
			ctx->diskError = ERROR_DLL_INIT_FAILED;
			kernel32_setLastError(ERROR_DLL_INIT_FAILED);
			free(key);
			return NULL;
		}
	}
	free(key);
	return info;
}

static ModuleInfo *resolveAndLoadExternal(LoadContext *ctx) {
	char *resolvedPath = resolveModuleOnDisk(ctx->reg, ctx->dllName, false);
	if (!resolvedPath) {
		DEBUG_LOG("  module not found on disk\n");
		ctx->diskError = ERROR_MOD_NOT_FOUND;
		return NULL;
	}
	ModuleInfo *info = tryLoadExternal(ctx, resolvedPath);
	free(resolvedPath);
	return info;
}

// Expects the registry lock to be held. Returns with the lock held unless it released it before returning.
static ModuleInfo *loadModuleLocked(LoadContext *ctx) {
	ModuleRegistry *reg = ctx->reg;
	ModuleInfo *existing = findByAlias(reg, ctx->alias);
	if (!existing) {
		char *fallback = normalizeAlias(ctx->dllName);
		existing = findByAlias(reg, fallback);
		free(fallback);
	}
	if (existing) {
		DEBUG_LOG("  found existing module alias %s (builtin=%d)\n", ctx->alias, existing->moduleStub != NULL);
		if (existing->moduleStub == NULL) {
			if (existing->refCount != UINT_MAX) {
				existing->refCount++;
			}
			DEBUG_LOG("  returning existing external module %s\n", existing->originalName);
			return existing;
		}
		bool pinned = hashmap_find(reg->pinnedModules, &existing, sizeof(existing), NULL);
		if (!pinned) {
			ModuleInfo *external = resolveAndLoadExternal(ctx);
			if (external) {
				DEBUG_LOG("  replaced builtin module %s with external copy\n", ctx->dllName);
				return external;
			} else if (ctx->diskError != ERROR_MOD_NOT_FOUND) {
				kernel32_setLastError(ctx->diskError);
				return NULL;
			}
		}
		DEBUG_LOG("  returning builtin module %s\n", existing->originalName);
		loadUnlock(ctx);
		if (!ensureModuleReady(existing)) {
			return NULL;
		}
		return existing;
	}

	ModuleInfo *external = resolveAndLoadExternal(ctx);
	if (external) {
		DEBUG_LOG("  loaded external module %s\n", ctx->dllName);
		return external;
	} else if (ctx->diskError != ERROR_MOD_NOT_FOUND) {
		kernel32_setLastError(ctx->diskError);
		return NULL;
	}

	ModuleInfo *builtin = findAliasInMap(reg->builtinAliasMap, ctx->alias);
	if (!builtin) {
		char *fallback = normalizeAlias(ctx->dllName);
		builtin = findAliasInMap(reg->builtinAliasMap, fallback);
		free(fallback);
	}
	if (builtin && builtin->moduleStub != NULL) {
		DEBUG_LOG("  falling back to builtin module %s\n", builtin->originalName);
		loadUnlock(ctx);
		if (!ensureModuleReady(builtin)) {
			return NULL;
		}
		return builtin;
	}

	kernel32_setLastError((ctx->diskError != ERROR_SUCCESS) ? ctx->diskError : ERROR_MOD_NOT_FOUND);
	return NULL;
}

static ModuleInfo *loadModuleInternal(const char *dllName) {
	LoadContext ctx = {0};
	ctx.dllName = dllName;
	ctx.parsed = parseModuleName(dllName);
	ctx.alias = normalizedBaseKey(&ctx.parsed);
	ctx.reg = lockRegistry();
	ctx.locked = true;

	ModuleInfo *info = loadModuleLocked(&ctx);
	if (ctx.locked) {
		loadUnlock(&ctx);
	}
	free(ctx.alias);
	parsedModuleNameFree(&ctx.parsed);
	return info;
}

ModuleInfo *wibo_loadModule(const char *dllName) {
	if (!dllName || *dllName == '\0') {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	DEBUG_LOG("loadModule(%s)\n", dllName);
	const char *requested = dllName;

	ParsedModuleName parsed = parseModuleName(requested);
	char *normalized = normalizedBaseKey(&parsed);
	for (size_t i = 0; i < sizeof(kApiSet) / sizeof(kApiSet[0]); ++i) {
		if (strcmp(kApiSet[i].dll, normalized) == 0) {
			DEBUG_LOG("  resolved api set %s -> %s\n", kApiSet[i].dll, kApiSet[i].api);
			requested = kApiSet[i].api;
			break;
		}
	}
	free(normalized);
	parsedModuleNameFree(&parsed);

	// DWORD lastError = kernel32::getLastError();
	return loadModuleInternal(requested);

	// kernel32::setLastError(lastError);
	// for (auto &[module, fallback] : kFallbacks) {
	// 	if (module == normalized) {
	// 		DEBUG_LOG("  trying fallback %s -> %s\n", module.data(), fallback.data());
	// 		return loadModuleInternal(std::string{fallback});
	// 	}
	// }

	// kernel32::setLastError(ERROR_MOD_NOT_FOUND);
	// return nullptr;
}

void wibo_freeModule(ModuleInfo *info) {
	if (!info) {
		return;
	}
	ModuleRegistry *reg = lockRegistry();
	if (info->refCount == UINT_MAX || info->refCount == 0) {
		unlockRegistry();
		return;
	}
	info->refCount--;
	if (info->refCount == 0) {
		if (info->tlsInfo.hasTls) {
			runModuleTlsCallbacks(info, TLS_PROCESS_DETACH);
		}
		callDllMain(info, DLL_PROCESS_DETACH, NULL);
		wibo_releaseModuleTls(info);
		char *key = (info->resolvedPath && info->resolvedPath[0] != '\0') ? storageKeyForPath(info->resolvedPath)
																		  : xstrdup(info->normalizedName);
		hashmap_remove(reg->modulesByKey, key, strlen(key), NULL);
		free(key);
		removeAliasesToModule(reg, info);
	}
	unlockRegistry();
}

typedef struct {
	const ModuleInfo *info;
	StringVec *aliases;
} AliasCollector;

static void collectAliasesToModuleCallback(const void *key, size_t keylen, void *val, void *ctx) {
	AliasCollector *collector = ctx;
	if (val == collector->info) {
		vec_push(collector->aliases, xstrndup(key, keylen));
	}
}

// Erasing during hashmap_foreach is not allowed, so the matching keys are collected first.
static void removeAliasesToModule(ModuleRegistry *reg, const ModuleInfo *info) {
	StringVec aliases = {0};
	AliasCollector collector = {info, &aliases};
	hashmap_foreach(reg->modulesByAlias, collectAliasesToModuleCallback, &collector);
	for (size_t i = 0; i < aliases.len; ++i) {
		hashmap_remove(reg->modulesByAlias, aliases.data[i], strlen(aliases.data[i]), NULL);
		free(aliases.data[i]);
	}
	vec_free(&aliases);
}

void *wibo_findExportByName(ModuleInfo *info, const char *funcName) {
	if (!info || !funcName) {
		return NULL;
	}
	if (info->moduleStub && info->moduleStub->byName) {
		void *func = info->moduleStub->byName(funcName);
		if (func) {
			return func;
		}
	}
	ensureExportsInitialized(info);
	void *ordinal = NULL;
	if (hashmap_find(info->exportNameToOrdinal, funcName, strlen(funcName), &ordinal)) {
		return wibo_findExportByOrdinal(info, (uint16_t)(uintptr_t)ordinal);
	}
	return NULL;
}

void *wibo_findExportByOrdinal(ModuleInfo *info, uint16_t ordinal) {
	if (!info) {
		return NULL;
	}
	if (info->moduleStub && info->moduleStub->byOrdinal) {
		void *func = info->moduleStub->byOrdinal(ordinal);
		if (func) {
			return func;
		}
	}
	ensureExportsInitialized(info);
	if (info->exportsByOrdinal.len && ordinal >= info->exportOrdinalBase) {
		size_t index = (size_t)(ordinal - info->exportOrdinalBase);
		if (index < info->exportsByOrdinal.len) {
			void *addr = info->exportsByOrdinal.data[index];
			if (addr) {
				return addr;
			}
		}
	}
	return NULL;
}

void *wibo_resolveFuncByName(ModuleInfo *info, const char *funcName) {
	void *func = wibo_findExportByName(info, funcName);
	if (func) {
		return func;
	}
	if (info && info->moduleStub) {
		const char *safeFunc = funcName ? funcName : "";
		return (void *)resolveMissingFuncName(info->originalName, safeFunc);
	}
	return NULL;
}

void *wibo_resolveFuncByOrdinal(ModuleInfo *info, uint16_t ordinal) {
	void *func = wibo_findExportByOrdinal(info, ordinal);
	if (func) {
		return func;
	}
	if (info && info->moduleStub) {
		return (void *)resolveMissingFuncOrdinal(info->originalName, ordinal);
	}
	return NULL;
}

void *wibo_resolveMissingImportByName(const char *dllName, const char *funcName) {
	const char *safeDll = dllName ? dllName : "";
	const char *safeFunc = funcName ? funcName : "";
	lockRegistry();
	void *func = (void *)resolveMissingFuncName(safeDll, safeFunc);
	unlockRegistry();
	return func;
}

void *wibo_resolveMissingImportByOrdinal(const char *dllName, uint16_t ordinal) {
	const char *safeDll = dllName ? dllName : "";
	lockRegistry();
	void *func = (void *)resolveMissingFuncOrdinal(safeDll, ordinal);
	unlockRegistry();
	return func;
}

Executable *wibo_executableFromModule(HMODULE module) {
	ModuleInfo *info = wibo_moduleInfoFromHandle(module);
	if (!info) {
		return NULL;
	}
	if (!info->executable && info->resolvedPath && info->resolvedPath[0] != '\0') {
		FILE *file = wibo_openImage(info->resolvedPath);
		if (!file) {
			perror("executableFromModule");
			return NULL;
		}
		Executable *executable = Executable_new();
		if (!Executable_loadPEFile(executable, file, false)) {
			DEBUG_LOG("executableFromModule: failed to load %s\n", info->resolvedPath);
			fclose(file);
			Executable_free(executable);
			return NULL;
		}
		fclose(file);
		info->executable = executable;
	}
	return info->executable;
}

static void copyLoadedModuleCallback(const void *key, size_t keylen, void *val, void *ctx) {
	hashmap_put(ctx, key, keylen, val);
}

hashmap *wibo_allLoadedModules(void) {
	ModuleRegistry *reg = lockRegistry();
	hashmap *copy = hashmap_create();
	hashmap_foreach(reg->modulesByKey, copyLoadedModuleCallback, copy);
	unlockRegistry();
	return copy;
}
