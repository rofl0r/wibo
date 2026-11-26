#include <windows.h>

#include <string.h>

#include "test_assert.h"

static int sign(int v) {
	return (v > 0) - (v < 0);
}

int main(void) {
	TEST_CHECK_EQ(0, _memicmp("ABCdef", "abcDEF", 6));
	TEST_CHECK_EQ(0, memicmp("ABCdef", "abcDEF", 6));

	TEST_CHECK_EQ(-1, sign(_memicmp("abc", "abd", 3)));
	TEST_CHECK_EQ(1, sign(_memicmp("abd", "ABC", 3)));

	TEST_CHECK_EQ(0, _memicmp("abX", "abY", 2));
	TEST_CHECK_EQ(0, _memicmp("abc", "xyz", 0));

	const char a[] = {'A', 0, 'b'};
	const char b[] = {'a', 0, 'B'};
	TEST_CHECK_EQ(0, _memicmp(a, b, 3));

	const char c[] = {'a', 0, 'c'};
	TEST_CHECK_EQ(-1, sign(_memicmp(a, c, 3)));

	return 0;
}
