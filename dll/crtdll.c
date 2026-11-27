#include "macros.h"
#include "modules.h"

INCLUDE_BIN(_crtdllDllData, EMBED_PATH)

// INCLUDE_BIN_SIZE is not a constant expression in C, so the size is filled in before main.
ModuleStub lib_crtdll = {
	.names =
		(const char *[]){
			"crtdll",
			NULL,
		},
	.dllData = _crtdllDllData,
};

__attribute__((constructor)) static void initDllDataSize(void) {
	lib_crtdll.dllDataSize = INCLUDE_BIN_SIZE(_crtdllDllData);
}
