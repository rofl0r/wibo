#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

int main(void) {
	const char *name = "WIBO_GETENV_S_TEST";
	const char *value = "hello";
	size_t need = strlen(value) + 1;
	char buf[32];
	size_t retLen = 0;

	TEST_CHECK_EQ(0, _putenv("WIBO_GETENV_S_TEST=hello"));

	// A NULL buffer with length 0 queries the required size.
	TEST_CHECK_EQ(0, getenv_s(&retLen, NULL, 0, name));
	TEST_CHECK_EQ(need, retLen);

	memset(buf, 'x', sizeof buf);
	TEST_CHECK_EQ(0, getenv_s(&retLen, buf, sizeof buf, name));
	TEST_CHECK_EQ(need, retLen);
	TEST_CHECK_STR_EQ(value, buf);

	// A buffer one byte short reports ERANGE but still returns the size.
	buf[0] = 'x';
	TEST_CHECK_EQ(ERANGE, getenv_s(&retLen, buf, need - 1, name));
	TEST_CHECK_EQ(need, retLen);

	// An unset variable yields an empty string and a zero size.
	TEST_CHECK_EQ(0, getenv_s(&retLen, buf, sizeof buf, "WIBO_GETENV_S_UNSET"));
	TEST_CHECK_EQ(0, retLen);
	TEST_CHECK_STR_EQ("", buf);

	TEST_CHECK_EQ(EINVAL, getenv_s(NULL, buf, sizeof buf, name));
	TEST_CHECK_EQ(EINVAL, getenv_s(&retLen, buf, 0, name));

	_putenv("WIBO_GETENV_S_TEST=");
	return 0;
}
