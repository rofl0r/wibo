#include "vcruntime.h"

#include "common.h"
#include "context.h"
#include "modules.h"

#include <string.h>

PVOID CDECL vcruntime_memcpy(PVOID dest, LPCVOID src, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return memcpy(dest, src, count);
}

PVOID CDECL vcruntime_memset(PVOID dest, int ch, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return memset(dest, ch, count);
}

int CDECL vcruntime_memcmp(LPCVOID buf1, LPCVOID buf2, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return memcmp(buf1, buf2, count);
}

PVOID CDECL vcruntime_memmove(PVOID dest, LPCVOID src, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return memmove(dest, src, count);
}

#include "vcruntime_trampolines.h"

const ModuleStub lib_vcruntime = {
	.names =
		(const char *[]){
			"vcruntime140",
			NULL,
		},
	.byName = vcruntimeThunkByName,
};
