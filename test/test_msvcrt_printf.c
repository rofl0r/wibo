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

    return 0;
}
