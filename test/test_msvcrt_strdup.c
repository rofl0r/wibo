#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

int main(void) {
	const char *src = "wibo strdup";
	char *copy = _strdup(src);
	TEST_CHECK(copy != NULL);
	TEST_CHECK(copy != src);
	TEST_CHECK_STR_EQ(src, copy);
	// The copy must not alias the source, so changing it leaves src alone.
	copy[0] = 'W';
	TEST_CHECK_STR_EQ("wibo strdup", src);
	TEST_CHECK_STR_EQ("Wibo strdup", copy);
	free(copy);

	char *empty = _strdup("");
	TEST_CHECK(empty != NULL);
	TEST_CHECK_STR_EQ("", empty);
	free(empty);

	char *alias = strdup(src);
	TEST_CHECK(alias != NULL);
	TEST_CHECK_STR_EQ(src, alias);
	free(alias);

	return 0;
}
