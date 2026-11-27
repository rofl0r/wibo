#include "winnls.h"

#include "context.h"
#include "cutil.h"
#include "errors.h"
#include "heap.h"
#include "internal.h"
#include "kernel32.h"
#include "kernel32_trampolines.h"
#include "strutil.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define kNormIgnoreCase ((DWORD)0x00000001)
#define LCID_INSTALLED ((DWORD)0x00000001)
#define LCID_SUPPORTED ((DWORD)0x00000002)
#define LCID_ALTERNATE_SORTS ((DWORD)0x00000004)
#define kEnUsLcid ((LCID)0x0409)
#define kInvariantLcid ((LCID)0x007f)
#define LOCALE_ALLOW_NEUTRAL_NAMES ((DWORD)0x08000000)
#define kLocaleNoUserOverride ((DWORD)0x80000000u)
#define kLocaleReturnNumber ((DWORD)0x20000000u)
#define kLocaleFlagMask ((DWORD)0xF0000000u)

static const uint8_t kShiftJisLeadRanges[][2] = {{0x81, 0x9F}, {0xE0, 0xFC}};
static const uint8_t kDbcsLeadRanges[][2] = {{0x81, 0xFE}};

static int compareStrings(const char *a, size_t aLen, const char *b, size_t bLen, DWORD dwCmpFlags) {
	for (size_t i = 0;; ++i) {
		if (i == aLen) {
			if (i == bLen) {
				return 2; // CSTR_EQUAL
			}
			return 1; // CSTR_LESS_THAN
		}
		if (i == bLen) {
			return 3; // CSTR_GREATER_THAN
		}
		unsigned char c = (unsigned char)a[i];
		unsigned char d = (unsigned char)b[i];
		if (dwCmpFlags & kNormIgnoreCase) {
			if (c >= 'a' && c <= 'z') {
				c = (unsigned char)(c - ('a' - 'A'));
			}
			if (d >= 'a' && d <= 'z') {
				d = (unsigned char)(d - ('a' - 'A'));
			}
		}
		if (c != d) {
			return (c < d) ? 1 : 3;
		}
	}
}

static const char *localeInfoString(int LCType) {
	switch (LCType) {
	case 4100: // LOCALE_IDEFAULTANSICODEPAGE
		return "28591";
	case 4097: // LOCALE_SENGLANGUAGE
		return "Lang";
	case 4098: // LOCALE_SENGCOUNTRY
		return "Country";
	case 0x1: // LOCALE_ILANGUAGE
		return "0001";
	case 0x15: // LOCALE_SINTLSYMBOL
		return "Currency";
	case 0x14: // LOCALE_SCURRENCY
		return "sCurrency";
	case 0x16: // LOCALE_SMONDECIMALSEP
		return ".";
	case 0x17: // LOCALE_SMONTHOUSANDSEP
		return ",";
	case 0x18: // LOCALE_SMONGROUPING
		return ";";
	case 0x50: // LOCALE_SPOSITIVESIGN
		return "";
	case 0x51: // LOCALE_SNEGATIVESIGN
		return "-";
	case 0x1A: // LOCALE_IINTLCURRDIGITS
	case 0x19: // LOCALE_ICURRDIGITS
		return "2";
	default:
		DEBUG_LOG("STUB: GetLocaleInfo LCType 0x%x not implemented\n", LCType);
		return "";
	}
}

// Expects a lowercased name.
static bool isEnUsLocaleName(const char *lowered) {
	return strcmp(lowered, "en-us") == 0 || strcmp(lowered, "en_us") == 0 ||
		   strcmp(lowered, "!x-sys-default-locale") == 0;
}

static bool isInByteRanges(BYTE testChar, const uint8_t ranges[][2], size_t count) {
	for (size_t i = 0; i < count; ++i) {
		if (testChar >= ranges[i][0] && testChar <= ranges[i][1]) {
			return true;
		}
	}
	return false;
}

UINT WINAPI GetACP(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetACP() -> %u\n", 28591);
	return 28591; // Latin1 (ISO/IEC 8859-1)
}

LANGID WINAPI GetSystemDefaultLangID(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetSystemDefaultLangID()\n");
	return 0;
}

LANGID WINAPI GetUserDefaultUILanguage(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetUserDefaultUILanguage()\n");
	return 0;
}

int WINAPI GetUserDefaultLocaleName(LPWSTR lpLocaleName, int cchLocaleName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetUserDefaultLocaleName(%p, %d)\n", lpLocaleName, cchLocaleName);
	if (!lpLocaleName || cchLocaleName < 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	static const uint16_t localeName[] = u"en-US";
	const int requiredChars = (int)(sizeof(localeName) / sizeof(localeName[0]));
	if (cchLocaleName < requiredChars) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	memcpy(lpLocaleName, localeName, sizeof(localeName));
	return requiredChars;
}

LCID WINAPI LocaleNameToLCID(LPCWSTR lpName, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LocaleNameToLCID(%p, 0x%x)\n", lpName, dwFlags);
	if (dwFlags & ~LOCALE_ALLOW_NEUTRAL_NAMES) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (!lpName) {
		return kEnUsLcid;
	}

	char *localeName = wideStringToString(lpName, -1);
	if (localeName[0] == '\0') {
		free(localeName);
		return kInvariantLcid;
	}

	char *normalized = stringToLower(localeName, strlen(localeName));
	free(localeName);
	bool isEnUs = isEnUsLocaleName(normalized);
	free(normalized);
	if (isEnUs) {
		return kEnUsLcid;
	}

	kernel32_setLastError(ERROR_INVALID_PARAMETER);
	return 0;
}

BOOL WINAPI GetCPInfo(UINT CodePage, LPCPINFO lpCPInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCPInfo(%u, %p)\n", CodePage, lpCPInfo);
	(void)CodePage;

	if (!lpCPInfo) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	lpCPInfo->MaxCharSize = 1;
	memset(lpCPInfo->DefaultChar, 0, MAX_DEFAULTCHAR);
	lpCPInfo->DefaultChar[0] = '?';
	memset(lpCPInfo->LeadByte, 0, MAX_LEADBYTES);
	return TRUE;
}

int WINAPI CompareStringA(LCID Locale, DWORD dwCmpFlags, LPCSTR lpString1, int cchCount1, LPCSTR lpString2,
						  int cchCount2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CompareStringA(%u, %u, %s, %d, %s, %d)\n", Locale, dwCmpFlags, lpString1 ? lpString1 : "(null)",
			  cchCount1, lpString2 ? lpString2 : "(null)", cchCount2);
	(void)Locale;
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	if (cchCount1 < 0) {
		cchCount1 = (int)strlen(lpString1);
	}
	if (cchCount2 < 0) {
		cchCount2 = (int)strlen(lpString2);
	}

	return compareStrings(lpString1, (size_t)cchCount1, lpString2, (size_t)cchCount2, dwCmpFlags);
}

int WINAPI CompareStringW(LCID Locale, DWORD dwCmpFlags, LPCWCH lpString1, int cchCount1, LPCWCH lpString2,
						  int cchCount2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CompareStringW(%u, %u, %p, %d, %p, %d)\n", Locale, dwCmpFlags, lpString1, cchCount1, lpString2,
			  cchCount2);
	(void)Locale;
	if (!lpString1 || !lpString2) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	size_t len1 = cchCount1 >= 0 ? (size_t)cchCount1 : wstrlen(lpString1);
	size_t len2 = cchCount2 >= 0 ? (size_t)cchCount2 : wstrlen(lpString2);
	char *str1 = wideStringToString(lpString1, (int)len1);
	char *str2 = wideStringToString(lpString2, (int)len2);
	int result = compareStrings(str1, len1, str2, len2, dwCmpFlags);
	free(str1);
	free(str2);
	return result;
}

BOOL WINAPI IsValidCodePage(UINT CodePage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsValidCodePage(%u)\n", CodePage);
	(void)CodePage;
	return TRUE;
}

BOOL WINAPI IsValidLocale(LCID Locale, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsValidLocale(%u, 0x%x)\n", Locale, dwFlags);
	(void)Locale;
	if (dwFlags != 0 && (dwFlags & ~(LCID_INSTALLED | LCID_SUPPORTED | LCID_ALTERNATE_SORTS)) != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return TRUE;
}

int WINAPI GetLocaleInfoA(LCID Locale, LCTYPE LCType, LPSTR lpLCData, int cchData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLocaleInfoA(%u, %u, %p, %d)\n", Locale, LCType, lpLCData, cchData);
	(void)Locale;

	const char *value = localeInfoString((int)LCType);
	size_t required = strlen(value) + 1;

	if (cchData == 0) {
		return (int)required;
	}
	if (!lpLCData || cchData < 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if ((size_t)cchData < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	memcpy(lpLCData, value, required);
	return (int)required;
}

int WINAPI GetLocaleInfoW(LCID Locale, LCTYPE LCType, LPWSTR lpLCData, int cchData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLocaleInfoW(%u, %u, %p, %d)\n", Locale, LCType, lpLCData, cchData);
	(void)Locale;

	const char *info = localeInfoString((int)LCType);
	size_t required = strlen(info) + 1;

	if (cchData == 0) {
		return (int)required;
	}
	if (!lpLCData || cchData < 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if ((size_t)cchData < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	uint16_t *wide = stringToWideString(info, required - 1);
	memcpy(lpLCData, wide, required * sizeof(uint16_t));
	free(wide);
	return (int)required;
}

int WINAPI GetLocaleInfoEx(LPCWSTR lpLocaleName, LCTYPE LCType, LPWSTR lpLCData, int cchData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLocaleInfoEx(%p, %u, %p, %d)\n", lpLocaleName, LCType, lpLCData, cchData);

	if (cchData < 0) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	DWORD lctypeFlags = (DWORD)LCType & kLocaleFlagMask;
	const DWORD kSupportedFlags = kLocaleNoUserOverride;
	if ((lctypeFlags & kLocaleReturnNumber) != 0 || (lctypeFlags & ~kSupportedFlags) != 0) {
		kernel32_setLastError(ERROR_INVALID_FLAGS);
		return 0;
	}

	if (lpLocaleName) {
		char *localeName = wideStringToString(lpLocaleName, -1);
		char *normalized = stringToLower(localeName, strlen(localeName));
		bool supported = normalized[0] == '\0' || isEnUsLocaleName(normalized);
		if (!supported) {
			DEBUG_LOG("GetLocaleInfoEx: unsupported locale name '%s'\n", localeName);
			free(localeName);
			free(normalized);
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return 0;
		}
		free(localeName);
		free(normalized);
	}

	DWORD baseType = (DWORD)LCType & ~kLocaleFlagMask;
	const char *info = localeInfoString((int)baseType);
	size_t required = strlen(info) + 1;

	if (cchData == 0) {
		return (int)required;
	}
	if (!lpLCData) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if ((size_t)cchData < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	uint16_t *wide = stringToWideString(info, required - 1);
	memcpy(lpLCData, wide, required * sizeof(uint16_t));
	free(wide);
	return (int)required;
}

BOOL WINAPI EnumSystemLocalesA(LOCALE_ENUMPROCA lpLocaleEnumProc, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EnumSystemLocalesA(%p, 0x%x)\n", lpLocaleEnumProc, dwFlags);
	(void)dwFlags;
	if (!lpLocaleEnumProc) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	static const char kDefaultLocaleId[] = "00000409"; // en-US
	char *localeId = wibo_heap_guestMalloc(sizeof(kDefaultLocaleId));
	if (!localeId) {
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	memcpy(localeId, kDefaultLocaleId, sizeof(kDefaultLocaleId));
	BOOL ret = call_LOCALE_ENUMPROCA(lpLocaleEnumProc, localeId);
	wibo_heap_guestFree(localeId);
	return ret;
}

LCID WINAPI GetUserDefaultLCID(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetUserDefaultLCID()\n");
	return 0x0409; // en-US
}

BOOL WINAPI IsDBCSLeadByte(BYTE TestChar) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: IsDBCSLeadByte(%u)\n", TestChar);
	(void)TestChar;
	return FALSE;
}

BOOL WINAPI IsDBCSLeadByteEx(UINT CodePage, BYTE TestChar) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsDBCSLeadByteEx(%u, %u)\n", CodePage, TestChar);

	kernel32_setLastError(ERROR_SUCCESS);
	switch (CodePage) {
	case 932: // Shift-JIS
		return isInByteRanges(TestChar, kShiftJisLeadRanges, 2);
	case 936:  // GBK
	case 949:  // Korean
	case 950:  // Big5
	case 1361: // Johab
		return isInByteRanges(TestChar, kDbcsLeadRanges, 1);
	case 0: // CP_ACP
	case 1: // CP_OEMCP
	case 2: // CP_MACCP
	case 3: // CP_THREAD_ACP
	default:
		return FALSE;
	}
}

int WINAPI LCMapStringW(LCID Locale, DWORD dwMapFlags, LPCWCH lpSrcStr, int cchSrc, LPWSTR lpDestStr, int cchDest) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LCMapStringW(%u, 0x%x, %p, %d, %p, %d)\n", Locale, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest);
	(void)Locale;
	if (!lpSrcStr || cchSrc == 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	bool nullTerminated = cchSrc < 0;
	size_t srcLen = nullTerminated ? (wstrlen(lpSrcStr) + 1) : (size_t)cchSrc;
	if (srcLen == 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	if (!lpDestStr || cchDest == 0) {
		return (int)srcLen;
	}
	if (cchDest < (int)srcLen) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	if (dwMapFlags & (0x00000400u | 0x00000800u)) { // LCMAP_SORTKEY | LCMAP_BYTEREV
		DEBUG_LOG("LCMapStringW: unsupported mapping flags 0x%x\n", dwMapFlags);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	unsigned int casingFlags = dwMapFlags & (0x00000200u | 0x00000100u); // UPPERCASE | LOWERCASE
	uint16_t *buffer = xmalloc(srcLen * sizeof(uint16_t));
	for (size_t i = 0; i < srcLen; ++i) {
		uint16_t ch = lpSrcStr[i];
		if (casingFlags == 0x00000200u) {
			buffer[i] = wcharToUpper(ch);
		} else if (casingFlags == 0x00000100u) {
			buffer[i] = wcharToLower(ch);
		} else {
			buffer[i] = ch;
		}
	}

	memcpy(lpDestStr, buffer, srcLen * sizeof(uint16_t));
	free(buffer);
	return (int)srcLen;
}

BOOL WINAPI IsValidLocaleName(LPCWSTR lpLocaleName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsValidLocaleName(%p)\n", lpLocaleName);
	return lpLocaleName != NULL;
}

int WINAPI LCMapStringEx(LPCWSTR lpLocaleName, DWORD dwMapFlags, LPCWCH lpSrcStr, int cchSrc, LPWSTR lpDestStr,
						 int cchDest, LPVOID lpVersionInformation, LPVOID lpReserved, LPVOID sortHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LCMapStringEx(%p, 0x%x, %p, %d, %p, %d)\n", lpLocaleName, dwMapFlags, lpSrcStr, cchSrc, lpDestStr,
			  cchDest);
	(void)lpVersionInformation;
	(void)lpReserved;
	(void)sortHandle;
	return LCMapStringW(0, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest);
}

int WINAPI CompareStringEx(LPCWSTR lpLocaleName, DWORD dwCmpFlags, LPCWCH lpString1, int cchCount1, LPCWCH lpString2,
						   int cchCount2, LPVOID lpVersionInformation, LPVOID lpReserved, LPVOID lParam) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CompareStringEx(%p, 0x%x, %p, %d, %p, %d)\n", lpLocaleName, dwCmpFlags, lpString1, cchCount1, lpString2,
			  cchCount2);
	(void)lpVersionInformation;
	(void)lpReserved;
	(void)lParam;
	return CompareStringW(0, dwCmpFlags, lpString1, cchCount1, lpString2, cchCount2);
}

int WINAPI LCMapStringA(LCID Locale, DWORD dwMapFlags, LPCCH lpSrcStr, int cchSrc, LPSTR lpDestStr, int cchDest) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LCMapStringA(%u, 0x%x, %p, %d, %p, %d)\n", Locale, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest);
	if (!lpSrcStr) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	int length = cchSrc;
	if (length < 0) {
		length = (int)strlen(lpSrcStr) + 1;
	}

	uint16_t *wideSrc = stringToWideString(lpSrcStr, (size_t)length);
	uint16_t *wideDest = cchDest > 0 ? xmalloc((size_t)cchDest * sizeof(uint16_t)) : NULL;
	int wideResult = LCMapStringW(Locale, dwMapFlags, wideSrc, length, wideDest, cchDest);
	free(wideSrc);
	if (wideResult == 0) {
		free(wideDest);
		kernel32_setLastError(ERROR_SUCCESS);
		return 0;
	}

	if (!lpDestStr || cchDest == 0) {
		free(wideDest);
		return wideResult;
	}

	char *mapped = wideStringToString(wideDest, wideResult);
	free(wideDest);
	size_t bytesToCopy = (size_t)wideResult + 1;
	if ((size_t)cchDest < bytesToCopy) {
		free(mapped);
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	memcpy(lpDestStr, mapped, bytesToCopy);
	free(mapped);
	return wideResult;
}
