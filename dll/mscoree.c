#include "mscoree.h"

#include "common.h"
#include "context.h"
#include "kernel32/internal.h"
#include "modules.h"

VOID WINAPI CorExitProcess(int exitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CorExitProcess(%i)\n", exitCode);
	kernel32_exitInternal(exitCode);
}

#include "mscoree_trampolines.h"

const ModuleStub lib_mscoree = {
	.names =
		(const char *[]){
			"mscoree",
			NULL,
		},
	.byName = mscoreeThunkByName,
};
