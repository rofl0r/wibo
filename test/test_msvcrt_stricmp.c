#include <windows.h>

#include <string.h>

#include "test_assert.h"

static int sign(int v) {
	return (v > 0) - (v < 0);
}

int main(void) {
	TEST_CHECK_EQ(0, _stricmp("Hello", "hELLO"));
	TEST_CHECK_EQ(-1, sign(_stricmp("abc", "ABD")));
	TEST_CHECK_EQ(1, sign(_stricmp("b", "A")));

	TEST_CHECK_EQ(0, _strnicmp("ABCdef", "abcXYZ", 3));
	TEST_CHECK_EQ(-1, sign(_strnicmp("ABCdef", "abcXYZ", 4)));
	TEST_CHECK_EQ(0, _strnicmp("anything", "ELSE", 0));

	TEST_CHECK_EQ(0, stricmp("Hello", "hELLO"));
	TEST_CHECK_EQ(0, strcmpi("Hello", "hELLO"));
	TEST_CHECK_EQ(0, strnicmp("ABCdef", "abcXYZ", 3));
	TEST_CHECK_EQ(-1, sign(strcmpi("abc", "ABD")));

	return 0;
}
