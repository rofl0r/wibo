#include <windows.h>

#include <stdlib.h>
#include <wchar.h>

#include "test_assert.h"

int main(void) {
	TEST_CHECK_EQ(0, _wputenv(L"WIBO_WPUTENV_TEST=hello"));
	TEST_CHECK_STR_EQ("hello", getenv("WIBO_WPUTENV_TEST"));

	// As with _putenv, an empty value deletes the variable.
	TEST_CHECK_EQ(0, _wputenv(L"WIBO_WPUTENV_TEST="));
	TEST_CHECK(getenv("WIBO_WPUTENV_TEST") == NULL);

	// A string without a name before '=' is rejected.
	TEST_CHECK_EQ(-1, _wputenv(L"=value"));
	TEST_CHECK_EQ(-1, _wputenv(L"WIBO_WPUTENV_NO_EQUALS"));
	return 0;
}
