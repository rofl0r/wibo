#include <windows.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "test_assert.h"

int main(void) {
	const wchar_t *name = L"WIBO_WGETENV_S_TEST";
	const wchar_t *expected = L"hello";
	size_t need = wcslen(expected) + 1;
	wchar_t buf[32];
	size_t retLen = 0;

	TEST_CHECK_EQ(0, _putenv("WIBO_WGETENV_S_TEST=hello"));

	// A NULL buffer with length 0 queries the required size in wide units.
	TEST_CHECK_EQ(0, _wgetenv_s(&retLen, NULL, 0, name));
	TEST_CHECK_EQ(need, retLen);

	wmemset(buf, L'x', sizeof buf / sizeof buf[0]);
	TEST_CHECK_EQ(0, _wgetenv_s(&retLen, buf, sizeof buf / sizeof buf[0], name));
	TEST_CHECK_EQ(need, retLen);
	TEST_CHECK(wcscmp(expected, buf) == 0);

	// A buffer one unit short reports ERANGE but still returns the size.
	buf[0] = L'x';
	TEST_CHECK_EQ(ERANGE, _wgetenv_s(&retLen, buf, need - 1, name));
	TEST_CHECK_EQ(need, retLen);

	// An unset variable yields an empty string and a zero size.
	TEST_CHECK_EQ(0, _wgetenv_s(&retLen, buf, 32, L"WIBO_WGETENV_S_UNSET"));
	TEST_CHECK_EQ(0, retLen);
	TEST_CHECK(buf[0] == 0);

	TEST_CHECK_EQ(EINVAL, _wgetenv_s(NULL, buf, 32, name));
	TEST_CHECK_EQ(EINVAL, _wgetenv_s(&retLen, buf, 0, name));

	_putenv("WIBO_WGETENV_S_TEST=");
	return 0;
}
