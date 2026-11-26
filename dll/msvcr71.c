#include "macros.h"
#include "modules.h"

INCLUDE_BIN(_msvcr71DllData, EMBED_PATH)

// INCLUDE_BIN_SIZE is not a constant expression in C, so the size is filled in before main.
ModuleStub lib_msvcr71 = {
	.names =
		(const char *[]){
			"msvcr71",
			"msvcr70",
			NULL,
		},
	.dllData = _msvcr71DllData,
};

__attribute__((constructor)) static void initDllDataSize(void) {
	lib_msvcr71.dllDataSize = INCLUDE_BIN_SIZE(_msvcr71DllData);
}
