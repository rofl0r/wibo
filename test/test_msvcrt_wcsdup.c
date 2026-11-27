#include <windows.h>

#include <stdlib.h>
#include <wchar.h>

#include "test_assert.h"

int main(void) {
	WCHAR src[] = L"Wide dup";
	WCHAR *copy = _wcsdup(src);
	TEST_CHECK(copy != NULL);
	TEST_CHECK(copy != src);
	TEST_CHECK_EQ(0, wcscmp(copy, src));
	copy[0] = L'w';
	TEST_CHECK_EQ(L'W', src[0]);
	free(copy);

	copy = wcsdup(src);
	TEST_CHECK(copy != NULL);
	TEST_CHECK_EQ(0, wcscmp(copy, src));
	free(copy);

	TEST_CHECK_EQ(0, _wcsicmp(L"ABCdef", L"abcDEF"));
	TEST_CHECK_EQ(0, wcsicmp(L"ABCdef", L"abcDEF"));
	TEST_CHECK(_wcsicmp(L"abc", L"abd") < 0);
	TEST_CHECK(_wcsicmp(L"abd", L"ABC") > 0);
	TEST_CHECK(_wcsicmp(L"abc", L"abcd") < 0);
	TEST_CHECK_EQ(0, _wcsicmp(L"", L""));

	return 0;
}
