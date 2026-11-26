#include <windows.h>

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

int main(void) {
	// getenv results must stay valid across later getenv calls and heap reuse.
	const char *path = getenv("PATH");
	TEST_CHECK(path != NULL);
	size_t pathLen = strlen(path);
	char *saved = malloc(pathLen + 1);
	TEST_CHECK(saved != NULL);
	strcpy(saved, path);
	for (int i = 0; i < 64; i++) {
		getenv("HOME");
		getenv("WIBO_NO_SUCH_VARIABLE");
	}
	for (size_t n = 1; n <= pathLen + 64; n++) {
		char *p = malloc(n);
		if (p) {
			memset(p, 'X', n);
		}
	}
	TEST_CHECK_STR_EQ(saved, path);
	free(saved);

	// localeconv must follow setlocale. Skip when the host has no such locale.
	TEST_CHECK(setlocale(LC_NUMERIC, "C") != NULL);
	TEST_CHECK_STR_EQ("", localeconv()->thousands_sep);
	if (setlocale(LC_NUMERIC, "en_US.utf8")) {
		TEST_CHECK_STR_EQ(",", localeconv()->thousands_sep);
	}
	TEST_CHECK(setlocale(LC_NUMERIC, "C") != NULL);
	TEST_CHECK_STR_EQ("", localeconv()->thousands_sep);

	// A guest errno write must not be overwritten by a later failing msvcrt call.
	errno = 7;
	FILE *f = fopen("msvcrt_errno_missing_dir/none.txt", "rb");
	TEST_CHECK(f == NULL);
	TEST_CHECK_EQ(ENOENT, errno);

	return 0;
}
