#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "test_assert.h"

// Guest code scans freshly malloc'd memory for a NUL terminator. Wine's heap
// does not zero reused blocks, so only fresh allocations are checked here.
static void test_malloc_zero_filled(void) {
	for (size_t size = 1; size <= 256; size++) {
		unsigned char *p = malloc(size);
		TEST_CHECK(p != NULL);
		for (size_t i = 0; i < size; i++) {
			TEST_CHECK_EQ(0, p[i]);
		}
	}
}

static void test_malloc_scan_for_nul(void) {
	volatile char *s = malloc(16);
	TEST_CHECK(s != NULL);
	size_t len = 0;
	while (len < 16 && s[len] != '\0') {
		len++;
	}
	TEST_CHECK_EQ(0, len);
}

static void test_calloc_zero_filled(void) {
	unsigned char *p = calloc(8, 4);
	TEST_CHECK(p != NULL);
	for (size_t i = 0; i < 32; i++) {
		TEST_CHECK_EQ(0, p[i]);
	}
	free(p);
	free(NULL);
}

static int compareInts(const void *a, const void *b) {
	int x = *(const int *)a;
	int y = *(const int *)b;
	return (x > y) - (x < y);
}

static void test_qsort(void) {
	int values[] = {5, -3, 9, 0, 2, 2, -7};
	const size_t count = sizeof values / sizeof values[0];
	qsort(values, count, sizeof values[0], compareInts);
	for (size_t i = 1; i < count; i++) {
		TEST_CHECK(values[i - 1] <= values[i]);
	}
	TEST_CHECK_EQ(-7, values[0]);
	TEST_CHECK_EQ(9, values[count - 1]);
}

static void test_memory_ops(void) {
	char buf[16];
	memset(buf, 'a', sizeof buf);
	TEST_CHECK(memcpy(buf, "xyz", 3) == buf);
	TEST_CHECK_EQ('x', buf[0]);
	TEST_CHECK_EQ('a', buf[3]);
	TEST_CHECK(memset(buf, 0, 4) == buf);
	TEST_CHECK_EQ(0, buf[3]);
	TEST_CHECK_EQ(0, memcmp("abc", "abc", 3));
	TEST_CHECK(memcmp("abc", "abd", 3) < 0);
	TEST_CHECK(memcmp("abd", "abc", 3) > 0);
}

static void test_string_ops(void) {
	char buf[32];
	TEST_CHECK_EQ(5, strlen("hello"));
	TEST_CHECK(strcmp("abc", "abc") == 0);
	TEST_CHECK(strcmp("abc", "abd") < 0);
	TEST_CHECK(strncmp("abcX", "abcY", 3) == 0);
	TEST_CHECK(strncmp("abcX", "abcY", 4) < 0);

	TEST_CHECK(strcpy(buf, "foo") == buf);
	TEST_CHECK(strcat(buf, "bar") == buf);
	TEST_CHECK_STR_EQ("foobar", buf);

	memset(buf, 'z', sizeof buf);
	TEST_CHECK(strncpy(buf, "ab", 4) == buf);
	TEST_CHECK_EQ('a', buf[0]);
	TEST_CHECK_EQ(0, buf[2]);
	TEST_CHECK_EQ(0, buf[3]);

	TEST_CHECK(strchr("hello", 'l') == strchr("hello", 'l'));
	TEST_CHECK_EQ(2, strchr("hello", 'l') - "hello");
	TEST_CHECK(strchr("hello", 'q') == NULL);
	TEST_CHECK(strchr("hello", '\0') == "hello" + 5);
	TEST_CHECK_EQ(3, strrchr("a/b/c", '/') - "a/b/c");
	TEST_CHECK(strrchr("abc", 'q') == NULL);
	TEST_CHECK_EQ('a', tolower('A'));
	TEST_CHECK_EQ('1', tolower('1'));
}

static void test_wide_string_ops(void) {
	TEST_CHECK_EQ(4, wcslen(L"wide"));
	TEST_CHECK(wcscmp(L"abc", L"abc") == 0);
	TEST_CHECK(wcscmp(L"abc", L"abd") < 0);
	TEST_CHECK(wcsncmp(L"abcX", L"abcY", 3) == 0);
	TEST_CHECK(wcsncmp(L"abcX", L"abcY", 4) < 0);
	TEST_CHECK_EQ(3, wcsrchr(L"a/b/c", L'/') - L"a/b/c");
	TEST_CHECK(wcsrchr(L"abc", L'q') == NULL);
}

int main(void) {
	test_malloc_zero_filled();
	test_malloc_scan_for_nul();
	test_calloc_zero_filled();
	test_qsort();
	test_memory_ops();
	test_string_ops();
	test_wide_string_ops();
	return EXIT_SUCCESS;
}
