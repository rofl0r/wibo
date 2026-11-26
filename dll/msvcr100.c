#include "macros.h"
#include "modules.h"

INCLUDE_BIN(_msvcr100DllData, EMBED_PATH)

// INCLUDE_BIN_SIZE is not a constant expression in C, so the size is filled in before main.
ModuleStub lib_msvcr100 = {
	.names =
		(const char *[]){
			"msvcr100",
			NULL,
		},
	.dllData = _msvcr100DllData,
};

__attribute__((constructor)) static void initDllDataSize(void) {
	lib_msvcr100.dllDataSize = INCLUDE_BIN_SIZE(_msvcr100DllData);
}
