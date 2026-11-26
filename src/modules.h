#pragma once

#include "common.h"
#include "cutil.h"
#include "entry.h"
#include "resources.h"
#include "tls.h"
#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

typedef void *(*ResolveByName)(const char *);
typedef void *(*ResolveByOrdinal)(uint16_t);

// Zero-initialize, then set the fields a built-in module provides.
typedef struct {
	const char **names;
	ResolveByName byName;
	ResolveByOrdinal byOrdinal;
	const uint8_t *dllData;
	size_t dllDataSize;
} ModuleStub;

typedef struct Executable Executable;

typedef struct Executable_SectionInfo {
	uintptr_t base;
	size_t size;
	DWORD protect;
	DWORD characteristics;
} Executable_SectionInfo;

typedef VEC(Executable_SectionInfo) SectionInfoVec;

struct Executable {
	void *imageBase;
	size_t imageSize;
	void *entryPoint;
	void *rsrcBase;
	uint32_t rsrcSize;
	uintptr_t preferredImageBase;
	intptr_t relocationDelta;
	bool is64Bit;
	uint32_t exportDirectoryRVA;
	uint32_t exportDirectorySize;
	uint32_t relocationDirectoryRVA;
	uint32_t relocationDirectorySize;
	uint32_t importDirectoryRVA;
	uint32_t importDirectorySize;
	uint32_t delayImportDirectoryRVA;
	uint32_t delayImportDirectorySize;
	uint32_t tlsDirectoryRVA;
	uint32_t tlsDirectorySize;
	bool execMapped;
	bool importsResolved;
	bool importsResolving;
	bool sectionsProtected;
	bool isDll;
	SectionInfoVec sections;
};

// Returns a zeroed executable with sections initialized; release with Executable_free.
Executable *Executable_new(void);
void Executable_free(Executable *self);
bool Executable_loadPEFile(Executable *self, FILE *file, bool exec);
bool Executable_loadPEImage(Executable *self, const uint8_t *image, size_t imageSize, bool exec);
bool Executable_resolveImports(Executable *self);
// language is NULL to match any language.
bool Executable_findResource(const Executable *self, const ResourceIdentifier *type, const ResourceIdentifier *name,
							 const uint16_t *language, ResourceLocation *out);

static inline void *Executable_fromRVA(const Executable *self, uint32_t rva) {
	return (uint8_t *)self->imageBase + rva;
}

typedef VEC(PIMAGE_TLS_CALLBACK) TlsCallbackVec;

typedef struct {
	bool hasTls;
	DWORD index;
	DWORD loaderIndex;
	DWORD *indexLocation;
	uint8_t *templateData;
	size_t templateSize;
	size_t zeroFillSize;
	uint32_t characteristics;
	size_t allocationSize;
	TlsCallbackVec callbacks;
	// Keyed by the raw bytes of a TEB *; values hold a GUEST_PTR cast to void *.
	hashmap *threadAllocations;
} ModuleTlsInfo;

typedef struct {
	// Windows-style handle to the module. For the main module, this is the image base.
	// For other modules, this is a pointer to the ModuleInfo structure.
	HMODULE handle;
	// Original name used to load the module (owned).
	char *originalName;
	// Normalized module name (owned).
	char *normalizedName;
	// Full path to the loaded module (owned).
	char *resolvedPath;
	// Pointer to the built-in module, NULL if loaded from file
	const ModuleStub *moduleStub;
	// Loaded PE executable (owned), NULL for built-in modules
	Executable *executable;
	// Reference count, or UINT_MAX for built-in modules
	unsigned int refCount;
	bool processAttachCalled;
	bool processAttachSucceeded;
	bool threadNotificationsEnabled;
	uint32_t exportOrdinalBase;
	PtrVec exportsByOrdinal;
	// Keyed by export name bytes (strlen); values hold a uint16_t cast to void *.
	hashmap *exportNameToOrdinal;
	bool exportsInitialized;
	ModuleTlsInfo tlsInfo;
} ModuleInfo;

extern ModuleInfo *wibo_mainModule;

void wibo_initializeModuleRegistry(void);
void wibo_shutdownModuleRegistry(void);
ModuleInfo *wibo_moduleInfoFromHandle(HMODULE module);
// The path is copied.
void wibo_setDllDirectoryOverride(const char *path);
void wibo_clearDllDirectoryOverride(void);
// Returns a malloc'd path, or NULL if no override is set.
char *wibo_dllDirectoryOverride(void);
ModuleInfo *wibo_findLoadedModule(const char *name);
void wibo_notifyDllThreadAttach(void);
void wibo_notifyDllThreadDetach(void);
BOOL wibo_disableThreadNotifications(ModuleInfo *info);
// Returns a malloc'd name-to-ModuleInfo* snapshot; the modules are not retained, so free only the map.
hashmap *wibo_allLoadedModules(void);
bool wibo_initializeModuleTls(ModuleInfo *module);
void wibo_releaseModuleTls(ModuleInfo *module);

ModuleInfo *wibo_loadModule(const char *name);
void wibo_freeModule(ModuleInfo *info);
void *wibo_findExportByName(ModuleInfo *info, const char *funcName);
void *wibo_findExportByOrdinal(ModuleInfo *info, uint16_t ordinal);
void *wibo_resolveFuncByName(ModuleInfo *info, const char *funcName);
void *wibo_resolveFuncByOrdinal(ModuleInfo *info, uint16_t ordinal);
void *wibo_resolveMissingImportByName(const char *dllName, const char *funcName);
void *wibo_resolveMissingImportByOrdinal(const char *dllName, uint16_t ordinal);

// Takes ownership of executable; resolvedPath and originalName are copied.
ModuleInfo *wibo_registerProcessModule(Executable *executable, const char *resolvedPath, const char *originalName);
Executable *wibo_executableFromModule(HMODULE module);
ModuleInfo *wibo_moduleInfoFromAddress(void *addr);

// HMODULE will be 0 or `wibo_mainModule->executable->imageBase` if it's the main module,
// otherwise it will be a pointer to a `ModuleInfo`.
static inline bool wibo_isMainModule(HMODULE hModule) {
	return hModule == NO_HANDLE ||
		   (wibo_mainModule && wibo_mainModule->executable &&
			(void *)(uintptr_t)hModule == wibo_mainModule->executable->imageBase);
}
