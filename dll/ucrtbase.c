#include "macros.h"
#include "modules.h"

INCLUDE_BIN(_ucrtbaseDllData, EMBED_PATH)

// INCLUDE_BIN_SIZE is not a constant expression in C, so the size is filled in before main.
ModuleStub lib_ucrtbase = {
	.names =
		(const char *[]){
			"ucrtbase",
			NULL,
		},
	.dllData = _ucrtbaseDllData,
};

__attribute__((constructor)) static void initDllDataSize(void) {
	lib_ucrtbase.dllDataSize = INCLUDE_BIN_SIZE(_ucrtbaseDllData);
}
