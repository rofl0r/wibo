#pragma once

#include "types.h"

#include <stddef.h>
#include <stdint.h>

size_t wstrlen(const uint16_t *str);
size_t wstrnlen(const uint16_t *str, size_t numberOfElements);
int wstrncmp(const uint16_t *string1, const uint16_t *string2, size_t count);
const uint16_t *wstrstr(const uint16_t *dest, const uint16_t *src);
uint16_t *wstrchr(const uint16_t *str, uint16_t c);
uint16_t *wstrrchr(const uint16_t *str, uint16_t c);
uint16_t *wstrcat(uint16_t *dest, const uint16_t *src);
uint16_t *wstrncat(uint16_t *dest, const uint16_t *src, size_t count);
uint16_t *wstrcpy(uint16_t *dest, const uint16_t *src);
size_t wstrncpy(uint16_t *dst, const uint16_t *src, size_t n);
// Returns a malloc'd NUL-terminated string (may contain embedded NULs if len is given); NULL src yields "".
char *wideStringToString(const uint16_t *src, int len);
// Returns a malloc'd NUL-terminated UTF-16 buffer of the copied units; length SIZE_MAX means strlen(src).
// NULL src yields {0}. Use wstrlen to recover the length.
uint16_t *stringToWideString(const char *src, size_t length);
// Returns a malloc'd NUL-terminated UTF-16 copy of the first length bytes (zero-extended).
uint16_t *stringToUtf16(const char *src, size_t length);
long wstrtol(const uint16_t *string, uint16_t **end_ptr, int base);
unsigned long wstrtoul(const uint16_t *string, uint16_t **end_ptr, int base);
void toLowerInPlace(char *str);
void toUpperInPlace(char *str);
// Returns a malloc'd NUL-terminated copy of the first len bytes, lowercased.
char *stringToLower(const char *str, size_t len);
// Returns a malloc'd NUL-terminated copy of the first len bytes, uppercased.
char *stringToUpper(const char *str, size_t len);
uint16_t wcharToLower(uint16_t ch);
uint16_t wcharToUpper(uint16_t ch);
