#include "macros.h"
#include "modules.h"

INCLUDE_BIN(_msvcirtDllData, EMBED_PATH)

// INCLUDE_BIN_SIZE is not a constant expression in C, so the size is filled in before main.
ModuleStub lib_msvcirt = {
	.names =
		(const char *[]){
			"msvcirt",
			NULL,
		},
	.dllData = _msvcirtDllData,
};

__attribute__((constructor)) static void initDllDataSize(void) {
	lib_msvcirt.dllDataSize = INCLUDE_BIN_SIZE(_msvcirtDllData);
}
