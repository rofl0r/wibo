#include <windows.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

int main(void) {
	char buf[40];

	TEST_CHECK(_itoa(42, buf, 10) == buf);
	TEST_CHECK_EQ(0, strcmp(buf, "42"));

	_itoa(-42, buf, 10);
	TEST_CHECK_EQ(0, strcmp(buf, "-42"));

	_itoa(255, buf, 16);
	TEST_CHECK_EQ(0, strcmp(buf, "ff"));

	_itoa(-1, buf, 16);
	TEST_CHECK_EQ(0, strcmp(buf, "ffffffff"));

	_itoa(0, buf, 2);
	TEST_CHECK_EQ(0, strcmp(buf, "0"));

	_itoa(35, buf, 36);
	TEST_CHECK_EQ(0, strcmp(buf, "z"));

	_itoa(INT_MIN, buf, 10);
	TEST_CHECK_EQ(0, strcmp(buf, "-2147483648"));

	_ltoa(LONG_MIN, buf, 10);
	TEST_CHECK_EQ(0, strcmp(buf, "-2147483648"));

	_ltoa(-16, buf, 16);
	TEST_CHECK_EQ(0, strcmp(buf, "fffffff0"));

	return 0;
}
