#include <windows.h>

#include <stdio.h>

#include "test_assert.h"

// The printf family forwards its variadic arguments through the Win64 thunk into the host printf.
// Return values report how many characters were produced, so they confirm each argument was consumed.
int main(void) {
	int n = printf("%d %s %x\n", 42, "abc", 0xbeef);
	TEST_CHECK_EQ(12, n);

	n = fprintf(stdout, "%ld|%5.2s|%c\n", 7L, "hello", 'z');
	TEST_CHECK_EQ(10, n);

	n = fprintf(stdout, "%I64d\n", 1234567890123LL);
	TEST_CHECK_EQ(14, n);

	n = fprintf(stdout, "%d %d %d %d %d %d %d %d\n", 1, 2, 3, 4, 5, 6, 7, 8);
	TEST_CHECK_EQ(16, n);

	// Doubles travel in the same 8-byte slot bits that the host reads as integers.
	n = printf("%.2f|%8.3f|%e\n", 1.5, -2.75, 1000.0);
	TEST_CHECK_EQ(27, n);

	n = printf("%*d|%-*d|\n", 5, 42, 4, 7);
	TEST_CHECK_EQ(12, n);

	n = printf("%hhd %hu %lld\n", 300, 70000, 1LL << 40);
	TEST_CHECK_EQ(22, n);

	int pos = -1;
	n = printf("abc%n", &pos);
	TEST_CHECK_EQ(3, n);
	TEST_CHECK_EQ(3, pos);

	n = printf("%ls|%5c|\n", L"wide", 'q');
	TEST_CHECK_EQ(12, n);

	n = printf("%p\n", (void *)0x1234);
	TEST_CHECK_EQ((int)sizeof(void *) * 2 + 1, n);

	return 0;
}
