#include "macros.h"
#include "modules.h"

INCLUDE_BIN(_msvcr120DllData, EMBED_PATH)

// INCLUDE_BIN_SIZE is not a constant expression in C, so the size is filled in before main.
ModuleStub lib_msvcr120 = {
	.names =
		(const char *[]){
			"msvcr120",
			"msvcr110",
			NULL,
		},
	.dllData = _msvcr120DllData,
};

__attribute__((constructor)) static void initDllDataSize(void) {
	lib_msvcr120.dllDataSize = INCLUDE_BIN_SIZE(_msvcr120DllData);
}
