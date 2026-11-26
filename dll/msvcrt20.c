#include "macros.h"
#include "modules.h"

INCLUDE_BIN(_msvcrt20DllData, EMBED_PATH)

// INCLUDE_BIN_SIZE is not a constant expression in C, so the size is filled in before main.
ModuleStub lib_msvcrt20 = {
	.names =
		(const char *[]){
			"msvcrt20",
			NULL,
		},
	.dllData = _msvcrt20DllData,
};

__attribute__((constructor)) static void initDllDataSize(void) {
	lib_msvcrt20.dllDataSize = INCLUDE_BIN_SIZE(_msvcrt20DllData);
}
