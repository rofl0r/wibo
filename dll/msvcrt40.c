#include "macros.h"
#include "modules.h"

INCLUDE_BIN(_msvcrt40DllData, EMBED_PATH)

// INCLUDE_BIN_SIZE is not a constant expression in C, so the size is filled in before main.
ModuleStub lib_msvcrt40 = {
	.names =
		(const char *[]){
			"msvcrt40",
			NULL,
		},
	.dllData = _msvcrt40DllData,
};

__attribute__((constructor)) static void initDllDataSize(void) {
	lib_msvcrt40.dllDataSize = INCLUDE_BIN_SIZE(_msvcrt40DllData);
}
