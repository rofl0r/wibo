#include "strutil.h"

#include "common.h"
#include "cutil.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

void toLowerInPlace(char *str) {
	for (; *str; ++str) {
		*str = (char)tolower((unsigned char)*str);
	}
}

void toUpperInPlace(char *str) {
	for (; *str; ++str) {
		*str = (char)toupper((unsigned char)*str);
	}
}

char *stringToLower(const char *str, size_t len) {
	char *result = xmalloc(len + 1);
	for (size_t i = 0; i < len; ++i) {
		result[i] = (char)tolower((unsigned char)str[i]);
	}
	result[len] = '\0';
	return result;
}

char *stringToUpper(const char *str, size_t len) {
	char *result = xmalloc(len + 1);
	for (size_t i = 0; i < len; ++i) {
		result[i] = (char)toupper((unsigned char)str[i]);
	}
	result[len] = '\0';
	return result;
}

uint16_t wcharToLower(uint16_t ch) {
	if (ch >= 'A' && ch <= 'Z') {
		return (uint16_t)(ch + ('a' - 'A'));
	}
	wint_t lowered = towlower((wint_t)ch);
	if (lowered > 0xFFFF) {
		return ch;
	}
	return (uint16_t)lowered;
}

uint16_t wcharToUpper(uint16_t ch) {
	if (ch >= 'a' && ch <= 'z') {
		return (uint16_t)(ch - ('a' - 'A'));
	}
	wint_t uppered = towupper((wint_t)ch);
	if (uppered > 0xFFFF) {
		return ch;
	}
	return (uint16_t)uppered;
}

size_t wstrlen(const uint16_t *str) {
	if (!str)
		return 0;

	size_t len = 0;
	while (str[len] != 0)
		++len;
	return len;
}

size_t wstrnlen(const uint16_t *str, size_t numberOfElements) {
	if (!str)
		return 0;
	size_t len = 0;
	while (len < numberOfElements && str[len] != 0)
		++len;
	return len;
}

int wstrncmp(const uint16_t *string1, const uint16_t *string2, size_t count) {
	const uint16_t *ptr1 = string1;
	const uint16_t *ptr2 = string2;
	for (size_t i = 0; i < count; i++) {
		uint16_t c1 = *ptr1++;
		uint16_t c2 = *ptr2++;
		if (c1 != c2) {
			return (c1 > c2) ? 1 : -1;
		}
	}
	return 0;
}

const uint16_t *wstrstr(const uint16_t *dest, const uint16_t *src) {
	if (!*src)
		return dest;

	for (; *dest != 0; dest++) {
		const uint16_t *d = dest;
		const uint16_t *s = src;

		while (*d != 0 && *s != 0 && *d == *s) {
			d++;
			s++;
		}

		if (*s == 0) {
			return dest;
		}
	}

	return NULL;
}

uint16_t *wstrchr(const uint16_t *str, uint16_t c) {
	if (!str)
		return NULL;
	for (; *str != 0; str++) {
		if (*str == c) {
			return (uint16_t *)str;
		}
	}
	// If searching for '\0', return pointer to terminator
	if (c == 0) {
		return (uint16_t *)str;
	}
	return NULL;
}

uint16_t *wstrrchr(const uint16_t *str, uint16_t c) {
	if (!str)
		return NULL;
	const uint16_t *last = NULL;
	const uint16_t *it = str;
	for (; *it != 0; ++it) {
		if (*it == c) {
			last = it;
		}
	}
	if (c == 0)
		return (uint16_t *)it;
	return (uint16_t *)last;
}

uint16_t *wstrcat(uint16_t *dest, const uint16_t *src) {
	uint16_t *d = dest;
	while (*d)
		d++;
	while ((*d++ = *src++) != 0)
		;
	return dest;
}

uint16_t *wstrncat(uint16_t *dest, const uint16_t *src, size_t count) {
	uint16_t *d = dest;
	while (*d)
		d++;
	for (size_t i = 0; i < count && src[i] != 0; i++) {
		*d++ = src[i];
	}
	*d = 0;
	return dest;
}

uint16_t *wstrcpy(uint16_t *dest, const uint16_t *src) {
	uint16_t *d = dest;
	while ((*d++ = *src++) != 0)
		;
	return dest;
}

size_t wstrncpy(uint16_t *dst, const uint16_t *src, size_t n) {
	if (!dst || !src || n == 0)
		return 0;
	size_t i = 0;
	for (; i < n && src[i] != 0; ++i) {
		dst[i] = src[i];
	}
	for (size_t j = i; j < n; ++j) {
		dst[j] = 0;
	}
	return i;
}

char *wideStringToString(const uint16_t *src, int len) {
	if (!src)
		return xstrdup("");

	size_t count;
	if (len >= 0) {
		count = (size_t)len;
	} else {
		count = wstrlen(src);
	}

#ifndef NDEBUG
	Buf hexDump;
	Buf_init(&hexDump);
	bool sawWide = false;
#endif

	char *result = xmalloc(count + 1);
	for (size_t i = 0; i < count; ++i) {
		uint16_t value = src[i];
#ifndef NDEBUG
		if (i > 0)
			Buf_appendc(&hexDump, ' ');
		char hex[16];
		snprintf(hex, sizeof(hex), "0x%x", (unsigned)value);
		Buf_appendstr(&hexDump, hex);
		if (value > 0xFF)
			sawWide = true;
#endif
		result[i] = (char)(value & 0xFF);
	}
	result[count] = '\0';

#ifndef NDEBUG
	if (sawWide) {
		size_t loggedLength = (len >= 0) ? count : wstrlen(src);
		DEBUG_LOG("wideString (%zu): %s\n", loggedLength, Buf_cstr(&hexDump));
	}
	Buf_free(&hexDump);
#endif

	return result;
}

uint16_t *stringToWideString(const char *src, size_t length) {
	if (!src) {
		uint16_t *zero = xmalloc(sizeof(uint16_t));
		zero[0] = 0;
		return zero;
	}
	size_t len = length == SIZE_MAX ? strlen(src) : length;
	uint16_t *res = xmalloc((len + 1) * sizeof(uint16_t));
	for (size_t i = 0; i < len; ++i) {
		res[i] = (uint16_t)(unsigned char)src[i];
	}
	res[len] = 0; // ensure NUL termination
	return res;
}

uint16_t *stringToUtf16(const char *src, size_t length) {
	uint16_t *result = xmalloc((length + 1) * sizeof(uint16_t));
	for (size_t i = 0; i < length; ++i) {
		result[i] = (uint16_t)(unsigned char)src[i];
	}
	result[length] = 0;
	return result;
}

long wstrtol(const uint16_t *string, uint16_t **end_ptr, int base) {
	if (!string) {
		if (end_ptr)
			*end_ptr = NULL;
		return 0;
	}

	size_t strLen = wstrlen(string);
	char *normal_str = wideStringToString(string, -1);
	char *normal_end = NULL;
	long res = strtol(normal_str, &normal_end, base);

	if (end_ptr) {
		if (normal_end && *normal_end) {
			size_t offset = (size_t)(normal_end - normal_str);
			*end_ptr = (uint16_t *)(string + offset);
		} else {
			*end_ptr = (uint16_t *)(string + strLen);
		}
	}

	free(normal_str);
	return res;
}

unsigned long wstrtoul(const uint16_t *string, uint16_t **end_ptr, int base) {
	if (!string) {
		if (end_ptr)
			*end_ptr = NULL;
		return 0;
	}

	size_t strLen = wstrlen(string);
	char *normal_str = wideStringToString(string, -1);
	char *normal_end = NULL;
	unsigned long res = strtoul(normal_str, &normal_end, base);

	if (end_ptr) {
		if (normal_end && *normal_end) {
			size_t offset = (size_t)(normal_end - normal_str);
			*end_ptr = (uint16_t *)(string + offset);
		} else {
			*end_ptr = (uint16_t *)(string + strLen);
		}
	}

	free(normal_str);
	return res;
}
