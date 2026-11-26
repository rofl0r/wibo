#include <windows.h>

#include <stdlib.h>
#include <wchar.h>

#include "test_assert.h"

int main(void) {
	TEST_CHECK(_wgetenv(L"WIBO_WGETENV_MISSING") == NULL);

	_wputenv(L"WIBO_WGETENV_TEST=first");
	WCHAR *first = _wgetenv(L"WIBO_WGETENV_TEST");
	TEST_CHECK(first != NULL);
	TEST_CHECK(first && wcscmp(first, L"first") == 0);

	// An unchanged value keeps the same copy.
	TEST_CHECK(_wgetenv(L"WIBO_WGETENV_TEST") == first);

	_wputenv(L"WIBO_WGETENV_TEST=second");
	WCHAR *second = _wgetenv(L"WIBO_WGETENV_TEST");
	TEST_CHECK(second != NULL);
	TEST_CHECK(second && wcscmp(second, L"second") == 0);

	_wputenv(L"WIBO_WGETENV_TEST=");
	TEST_CHECK(_wgetenv(L"WIBO_WGETENV_TEST") == NULL);
	return 0;
}
