#pragma once

#include "types.h"

#define MAX_DEFAULTCHAR ((UINT)2)
#define MAX_LEADBYTES ((UINT)12)

typedef struct CPINFO {
	UINT MaxCharSize;
	BYTE DefaultChar[MAX_DEFAULTCHAR];
	BYTE LeadByte[MAX_LEADBYTES];
} CPINFO;

typedef CPINFO *LPCPINFO;
typedef BOOL(_CC_STDCALL *LOCALE_ENUMPROCA)(LPSTR);

UINT WINAPI GetACP(void);
LANGID WINAPI GetSystemDefaultLangID(void);
LANGID WINAPI GetUserDefaultUILanguage(void);
int WINAPI GetUserDefaultLocaleName(LPWSTR lpLocaleName, int cchLocaleName);
LCID WINAPI LocaleNameToLCID(LPCWSTR lpName, DWORD dwFlags);
BOOL WINAPI GetCPInfo(UINT CodePage, LPCPINFO lpCPInfo);
int WINAPI CompareStringA(LCID Locale, DWORD dwCmpFlags, LPCSTR lpString1, int cchCount1, LPCSTR lpString2,
						  int cchCount2);
int WINAPI CompareStringW(LCID Locale, DWORD dwCmpFlags, LPCWCH lpString1, int cchCount1, LPCWCH lpString2,
						  int cchCount2);
BOOL WINAPI IsValidCodePage(UINT CodePage);
BOOL WINAPI IsValidLocale(LCID Locale, DWORD dwFlags);
int WINAPI GetLocaleInfoA(LCID Locale, LCTYPE LCType, LPSTR lpLCData, int cchData);
int WINAPI GetLocaleInfoW(LCID Locale, LCTYPE LCType, LPWSTR lpLCData, int cchData);
int WINAPI GetLocaleInfoEx(LPCWSTR lpLocaleName, LCTYPE LCType, LPWSTR lpLCData, int cchData);
BOOL WINAPI EnumSystemLocalesA(LOCALE_ENUMPROCA lpLocaleEnumProc, DWORD dwFlags);
LCID WINAPI GetUserDefaultLCID(void);
BOOL WINAPI IsDBCSLeadByte(BYTE TestChar);
BOOL WINAPI IsDBCSLeadByteEx(UINT CodePage, BYTE TestChar);
int WINAPI LCMapStringW(LCID Locale, DWORD dwMapFlags, LPCWCH lpSrcStr, int cchSrc, LPWSTR lpDestStr, int cchDest);
int WINAPI LCMapStringA(LCID Locale, DWORD dwMapFlags, LPCCH lpSrcStr, int cchSrc, LPSTR lpDestStr, int cchDest);
int WINAPI LCMapStringEx(LPCWSTR lpLocaleName, DWORD dwMapFlags, LPCWCH lpSrcStr, int cchSrc, LPWSTR lpDestStr,
						 int cchDest, LPVOID lpVersionInformation, LPVOID lpReserved, LPVOID sortHandle);
BOOL WINAPI IsValidLocaleName(LPCWSTR lpLocaleName);
int WINAPI CompareStringEx(LPCWSTR lpLocaleName, DWORD dwCmpFlags, LPCWCH lpString1, int cchCount1, LPCWCH lpString2,
						   int cchCount2, LPVOID lpVersionInformation, LPVOID lpReserved, LPVOID lParam);
