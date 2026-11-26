#pragma once

#include "types.h"

// Exported as memcpy/memset/memcmp/memmove; the C names avoid clashing with libc's <string.h> declarations.
PVOID CDECL vcruntime_memcpy(PVOID dest, LPCVOID src, SIZE_T count);
PVOID CDECL vcruntime_memset(PVOID dest, int ch, SIZE_T count);
int CDECL vcruntime_memcmp(LPCVOID buf1, LPCVOID buf2, SIZE_T count);
PVOID CDECL vcruntime_memmove(PVOID dest, LPCVOID src, SIZE_T count);
