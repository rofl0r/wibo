#include "shlwapi.h"

#include "common.h"
#include "context.h"
#include "kernel32/minwinbase.h"
#include "modules.h"

#include <string.h>

LPSTR WINAPI PathAddBackslashA(LPSTR pszPath) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PathAddBackslashA(%s)\n", pszPath ? pszPath : "(null)");
	if (!pszPath) {
		return NULL;
	}

	size_t length = strlen(pszPath);
	if (length > 0 && pszPath[length - 1] == '\\') {
		return pszPath + length;
	}

	if (length + 1 >= MAX_PATH) {
		return NULL;
	}

	pszPath[length] = '\\';
	pszPath[length + 1] = '\0';
	return pszPath + length + 1;
}

#include "shlwapi_trampolines.h"

const ModuleStub lib_shlwapi = {
	.names =
		(const char *[]){
			"shlwapi",
			NULL,
		},
	.byName = shlwapiThunkByName,
};
