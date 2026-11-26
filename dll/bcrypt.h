#pragma once

#include "types.h"

typedef PVOID BCRYPT_ALG_HANDLE;

NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags);
BOOL WINAPI ProcessPrng(PBYTE pbData, SIZE_T cbData);
