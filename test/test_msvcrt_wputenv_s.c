#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "test_assert.h"

int main(void) {
	TEST_CHECK_EQ(0, _wputenv_s(L"WIBO_WPUTENV_S_TEST", L"hello"));
	TEST_CHECK_STR_EQ("hello", getenv("WIBO_WPUTENV_S_TEST"));

	// An empty value deletes the variable, as _putenv_s does.
	TEST_CHECK_EQ(0, _wputenv_s(L"WIBO_WPUTENV_S_TEST", L""));
	TEST_CHECK(getenv("WIBO_WPUTENV_S_TEST") == NULL);

	TEST_CHECK_EQ(EINVAL, _wputenv_s(NULL, L"value"));
	TEST_CHECK_EQ(EINVAL, _wputenv_s(L"WIBO_WPUTENV_S_TEST", NULL));
	TEST_CHECK_EQ(EINVAL, _wputenv_s(L"", L"value"));
	TEST_CHECK(getenv("WIBO_WPUTENV_S_TEST") == NULL);
	return 0;
}
