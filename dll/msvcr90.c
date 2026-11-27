#include "macros.h"
#include "modules.h"

INCLUDE_BIN(_msvcr90DllData, EMBED_PATH)

// INCLUDE_BIN_SIZE is not a constant expression in C, so the size is filled in before main.
ModuleStub lib_msvcr90 = {
	.names =
		(const char *[]){
			"msvcr90",
			"msvcr80",
			NULL,
		},
	.dllData = _msvcr90DllData,
};

__attribute__((constructor)) static void initDllDataSize(void) {
	lib_msvcr90.dllDataSize = INCLUDE_BIN_SIZE(_msvcr90DllData);
}
