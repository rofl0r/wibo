#pragma once

#include "types.h"

typedef DWORD ALG_ID;
typedef ULONG_PTR HCRYPTPROV;
typedef ULONG_PTR HCRYPTKEY;
typedef ULONG_PTR HCRYPTHASH;

#define CALG_MD5 ((ALG_ID)0x00008003)
#define CALG_SHA1 ((ALG_ID)0x00008004)

#define HP_ALGID ((DWORD)0x00000001)
#define HP_HASHVAL ((DWORD)0x00000002)
#define HP_HASHSIZE ((DWORD)0x00000004)

BOOL WINAPI CryptReleaseContext(HCRYPTPROV hProv, DWORD dwFlags);
BOOL WINAPI CryptAcquireContextW(HCRYPTPROV *phProv, LPCWSTR pszContainer, LPCWSTR pszProvider, DWORD dwProvType,
								 DWORD dwFlags);
BOOL WINAPI CryptGenRandom(HCRYPTPROV hProv, DWORD dwLen, BYTE *pbBuffer);
BOOL WINAPI CryptCreateHash(HCRYPTPROV hProv, ALG_ID Algid, HCRYPTKEY hKey, DWORD dwFlags, HCRYPTHASH *phHash);
BOOL WINAPI CryptHashData(HCRYPTHASH hHash, const BYTE *pbData, DWORD dwDataLen, DWORD dwFlags);
BOOL WINAPI CryptGetHashParam(HCRYPTHASH hHash, DWORD dwParam, BYTE *pbData, DWORD *pdwDataLen, DWORD dwFlags);
BOOL WINAPI CryptDestroyHash(HCRYPTHASH hHash);
