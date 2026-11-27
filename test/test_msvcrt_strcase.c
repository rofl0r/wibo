#include <windows.h>

#include <string.h>

#include "test_assert.h"

int main(void) {
	char buf[] = "MiXeD 123 _-Z";
	char *ret = _strlwr(buf);
	TEST_CHECK(ret == buf);
	TEST_CHECK_EQ(0, strcmp(buf, "mixed 123 _-z"));

	ret = _strupr(buf);
	TEST_CHECK(ret == buf);
	TEST_CHECK_EQ(0, strcmp(buf, "MIXED 123 _-Z"));

	char empty[] = "";
	TEST_CHECK(_strlwr(empty) == empty);
	TEST_CHECK_EQ('\0', empty[0]);

	TEST_CHECK(_strlwr(NULL) == NULL);
	TEST_CHECK(_strupr(NULL) == NULL);

	return 0;
}
