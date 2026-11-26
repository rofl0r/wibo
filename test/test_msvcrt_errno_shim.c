#include <windows.h>

#include <errno.h>
#include <stdio.h>

#include "test_assert.h"

int main(void) {
	// Hold the errno cell pointer and read through it, without another msvcrt call in between.
	int *cell = _errno();
	*cell = 0;

	// wibo maps the host's ENOENT from unlink() to this Windows error. Wine does not set errno here,
	// so this case checks wibo's shim-to-guest errno sync only.
	SetLastError(0);
	TEST_CHECK(!DeleteFileA("msvcrt_errno_shim_missing.txt"));
	TEST_CHECK_EQ(ERROR_FILE_NOT_FOUND, GetLastError());
	TEST_CHECK_EQ(ENOENT, *cell);

	// A guest write survives a shim that leaves errno unchanged.
	*cell = EINVAL;
	TEST_CHECK(GetCurrentProcessId() != 0);
	TEST_CHECK_EQ(EINVAL, *cell);

	printf("msvcrt errno shim ok\n");
	return 0;
}
