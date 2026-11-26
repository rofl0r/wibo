#include "macros.h"
#include "modules.h"

INCLUDE_BIN(_msvcrtDllData, EMBED_PATH)

// INCLUDE_BIN_SIZE is not a constant expression in C, so the size is filled in before main.
ModuleStub lib_msvcrt = {
	.names =
		(const char *[]){
			"msvcrt",
			NULL,
		},
	.dllData = _msvcrtDllData,
};

__attribute__((constructor)) static void initDllDataSize(void) {
	lib_msvcrt.dllDataSize = INCLUDE_BIN_SIZE(_msvcrtDllData);
}
