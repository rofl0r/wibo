#include <windows.h>

#include <stdlib.h>

#include "test_assert.h"

int main(void) {
	const char *name = "WIBO_PUTENV_TEST";

	TEST_CHECK_EQ(0, _putenv("WIBO_PUTENV_TEST=one"));
	TEST_CHECK_STR_EQ("one", getenv(name));

	TEST_CHECK_EQ(0, _putenv("WIBO_PUTENV_TEST=second"));
	TEST_CHECK_STR_EQ("second", getenv(name));

	TEST_CHECK_EQ(0, _putenv("WIBO_PUTENV_TEST="));
	TEST_CHECK(getenv(name) == NULL);

	// Wine does not set errno for these, so only the return value is checked.
	TEST_CHECK_EQ(-1, _putenv("WIBO_PUTENV_TEST"));
	TEST_CHECK_EQ(-1, _putenv("=value"));
	TEST_CHECK(getenv(name) == NULL);

	return 0;
}
