#include <windows.h>

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include "test_assert.h"

int main(void) {
	TEST_CHECK_EQ(42, atoi("  +42xyz"));
	TEST_CHECK_EQ(-7, atoi("-7"));
	TEST_CHECK_EQ(0, atoi("abc"));
	TEST_CHECK_EQ(123456L, atol("123456"));

	char input[] = "0x1fZZ";
	char *end = NULL;
	errno = 0;
	long value = strtol(input, &end, 16);
	TEST_CHECK_EQ(0x1f, value);
	TEST_CHECK(end == input + 4);
	TEST_CHECK_EQ(0, errno);

	end = NULL;
	value = strtol("nope", &end, 10);
	TEST_CHECK_EQ(0, value);
	TEST_CHECK(end == NULL || *end == 'n');

	errno = 0;
	value = strtol("2147483647", NULL, 10);
	TEST_CHECK_EQ(LONG_MAX, value);
	TEST_CHECK_EQ(0, errno);

	errno = 0;
	value = strtol("2147483648", NULL, 10);
	TEST_CHECK_EQ(LONG_MAX, value);
	TEST_CHECK_EQ(ERANGE, errno);

	errno = 0;
	value = strtol("-2147483649", NULL, 10);
	TEST_CHECK_EQ(LONG_MIN, value);
	TEST_CHECK_EQ(ERANGE, errno);

	return 0;
}
