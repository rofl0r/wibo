#include <windows.h>

#include <process.h>

#include "test_assert.h"

int main(void) {
	int pid = _getpid();
	TEST_CHECK(pid > 0);
	TEST_CHECK_EQ((int)GetCurrentProcessId(), pid);
	return 0;
}
