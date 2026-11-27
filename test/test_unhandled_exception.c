#include "test_assert.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

static LONG WINAPI exitFilter(EXCEPTION_POINTERS *info) {
	(void)info;
	ExitProcess(77);
}

static int child_main(int argc, char **argv) {
	if (argc > 2 && strcmp(argv[2], "filter") == 0) {
		SetUnhandledExceptionFilter(exitFilter);
	}
	volatile int *nullPtr = NULL;
	if (argc > 0) {
		*nullPtr = 1;
	}
	return 0;
}

static DWORD run_child(const char *mode) {
	char modulePath[MAX_PATH];
	TEST_CHECK(GetModuleFileNameA(NULL, modulePath, sizeof(modulePath)) > 0);

	char commandLine[MAX_PATH + 32];
	snprintf(commandLine, sizeof(commandLine), "\"%s\" child %s", modulePath, mode);

	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	TEST_CHECK(CreateProcessA(modulePath, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(pi.hProcess, 10000));

	DWORD exitCode = 0;
	TEST_CHECK(GetExitCodeProcess(pi.hProcess, &exitCode));
	TEST_CHECK(CloseHandle(pi.hThread));
	TEST_CHECK(CloseHandle(pi.hProcess));
	return exitCode;
}

int main(int argc, char **argv) {
	if (argc > 1 && strcmp(argv[1], "child") == 0) {
		return child_main(argc, argv);
	}

	DWORD plainExit = run_child("plain");
	/* Wine reports the full NTSTATUS; wibo's exit status is 8 bits wide, so compare the low byte. */
	TEST_CHECK_EQ((DWORD)5, plainExit & 0xFF);

	DWORD filterExit = run_child("filter");
	TEST_CHECK_EQ((DWORD)77, filterExit);
	return 0;
}
