#include "modules.h"

#include "advapi32_trampolines.h"

const ModuleStub lib_advapi32 = {
	.names =
		(const char *[]){
			"advapi32",
			NULL,
		},
	.byName = advapi32ThunkByName,
};
