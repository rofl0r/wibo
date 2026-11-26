#include "stringapiset.h"

#include "common.h"
#include "context.h"
#include "cutil.h"
#include "errors.h"
#include "internal.h"
#include "strutil.h"

#include <assert.h>
#include <string.h>
#include <wctype.h>

int WINAPI WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWCH lpWideCharStr, int cchWideChar,
							   LPSTR lpMultiByteStr, int cbMultiByte, LPCCH lpDefaultChar, LPBOOL lpUsedDefaultChar) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("WideCharToMultiByte(%u, %u, %p, %d, %p, %d, %p, %p)\n", CodePage, dwFlags, lpWideCharStr, cchWideChar,
				lpMultiByteStr, cbMultiByte, lpDefaultChar, lpUsedDefaultChar);

	(void)CodePage;
	(void)dwFlags;
	(void)lpDefaultChar;

	if (!lpWideCharStr || cchWideChar == 0 || cchWideChar < -1 || cbMultiByte < 0 ||
		(cbMultiByte > 0 && !lpMultiByteStr)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (lpUsedDefaultChar) {
		*lpUsedDefaultChar = FALSE;
	}
	if (cchWideChar == -1) {
		cchWideChar = (int)wstrlen(lpWideCharStr) + 1;
	}
	if (cbMultiByte == 0) {
		return cchWideChar;
	}
	if (cbMultiByte < cchWideChar) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	for (int i = 0; i < cchWideChar; i++) {
		lpMultiByteStr[i] = (char)(lpWideCharStr[i] & 0xFF);
	}

	if (wibo_debugEnabled) {
		VERBOSE_LOG("Converted string: [%.*s] (len %d)\n", cchWideChar, lpMultiByteStr, cchWideChar);
	}

	return cchWideChar;
}

int WINAPI MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCCH lpMultiByteStr, int cbMultiByte,
							   LPWSTR lpWideCharStr, int cchWideChar) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("MultiByteToWideChar(%u, %u, %d, %d)\n", CodePage, dwFlags, cbMultiByte, cchWideChar);

	(void)CodePage;
	(void)dwFlags;

	if (!lpMultiByteStr || cbMultiByte == 0 || cbMultiByte < -1 || cchWideChar < 0 ||
		(cchWideChar > 0 && !lpWideCharStr)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (cbMultiByte == -1) {
		cbMultiByte = (int)strlen(lpMultiByteStr) + 1;
	}

	if (cchWideChar == 0) {
		return cbMultiByte;
	}
	if (cchWideChar < cbMultiByte) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	if (wibo_debugEnabled) {
		VERBOSE_LOG("Converting string: [%.*s] (len %d)\n", cbMultiByte, lpMultiByteStr, cbMultiByte);
	}

	for (int i = 0; i < cbMultiByte; i++) {
		lpWideCharStr[i] = (uint16_t)(lpMultiByteStr[i] & 0xFF);
	}
	return cbMultiByte;
}

BOOL WINAPI GetStringTypeW(DWORD dwInfoType, LPCWCH lpSrcStr, int cchSrc, LPWORD lpCharType) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetStringTypeW(%u, %p, %i, %p)\n", dwInfoType, lpSrcStr, cchSrc, lpCharType);

	assert(dwInfoType == 1); // CT_CTYPE1

	if (!lpSrcStr || !lpCharType) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (cchSrc < 0) {
		cchSrc = (int)wstrlen(lpSrcStr);
	}

	for (int i = 0; i < cchSrc; i++) {
		wint_t c = lpSrcStr[i];
		bool upper = iswupper(c);
		bool lower = iswlower(c);
		bool alpha = iswalpha(c);
		bool digit = iswdigit(c);
		bool space = iswspace(c);
		bool blank = (c == L' ' || c == L'\t');
		bool hex = iswxdigit(c);
		bool cntrl = iswcntrl(c);
		bool punct = iswpunct(c);
		lpCharType[i] = (upper ? 1 : 0) | (lower ? 2 : 0) | (digit ? 4 : 0) | (space ? 8 : 0) | (punct ? 0x10 : 0) |
						(cntrl ? 0x20 : 0) | (blank ? 0x40 : 0) | (hex ? 0x80 : 0) | (alpha ? 0x100 : 0);
	}

	return TRUE;
}

BOOL WINAPI GetStringTypeA(LCID Locale, DWORD dwInfoType, LPCSTR lpSrcStr, int cchSrc, LPWORD lpCharType) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetStringTypeA(%u, %u, %p, %d, %p)\n", Locale, dwInfoType, lpSrcStr, cchSrc, lpCharType);
	(void)Locale;

	if (!lpSrcStr || !lpCharType) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (dwInfoType != 1) {
		kernel32_setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}

	int length = cchSrc;
	if (length < 0) {
		length = (int)strlen(lpSrcStr);
	}
	if (length < 0) {
		length = 0;
	}
	if (length == 0) {
		return TRUE;
	}

	uint16_t *wide = xmalloc(sizeof(uint16_t) * (size_t)length);
	for (int i = 0; i < length; ++i) {
		wide[i] = (unsigned char)lpSrcStr[i];
	}

	BOOL result = GetStringTypeW(dwInfoType, wide, length, lpCharType);
	free(wide);
	return result;
}
