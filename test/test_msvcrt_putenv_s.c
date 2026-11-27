#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

int main(void) {
	const char *name = "WIBO_PUTENV_S_TEST";

	TEST_CHECK_EQ(0, _putenv_s(name, "one"));
	TEST_CHECK_STR_EQ("one", getenv(name));

	TEST_CHECK_EQ(0, _putenv_s(name, "second"));
	TEST_CHECK_STR_EQ("second", getenv(name));

	// Unlike _putenv, an empty value deletes the variable.
	TEST_CHECK_EQ(0, _putenv_s(name, ""));
	TEST_CHECK(getenv(name) == NULL);

	TEST_CHECK_EQ(EINVAL, _putenv_s(NULL, "value"));
	TEST_CHECK_EQ(EINVAL, _putenv_s(name, NULL));
	TEST_CHECK(getenv(name) == NULL);

	return 0;
}
