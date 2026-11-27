#pragma once

#include "types.h"

typedef void (_CC_STDCALL *PFLS_CALLBACK_FUNCTION)(void *);
#define FLS_OUT_OF_INDEXES ((DWORD)0xFFFFFFFF)

DWORD WINAPI FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback);
BOOL WINAPI FlsFree(DWORD dwFlsIndex);
PVOID WINAPI FlsGetValue(DWORD dwFlsIndex);
BOOL WINAPI FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData);
