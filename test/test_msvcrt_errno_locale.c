#include <windows.h>

#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

static int g_workerErrno;

static DWORD WINAPI errnoWorker(LPVOID unused) {
	(void)unused;
	FILE *f = fopen("msvcrt_errno_missing_dir/none.txt", "rb");
	if (f) {
		fclose(f);
	}
	g_workerErrno = errno;
	return 0;
}

#define STRESS_THREADS 4
#define STRESS_ITERATIONS 500

static char *g_expectedPath;
static char *g_expectedErr;

static char *copyOf(const char *s) {
	size_t n = strlen(s) + 1;
	char *out = malloc(n);
	TEST_CHECK(out != NULL);
	memcpy(out, s, n);
	return out;
}

static DWORD WINAPI strerrorWorker(LPVOID unused) {
	(void)unused;
	strerror(EACCES);
	return 0;
}

static DWORD WINAPI stressWorker(LPVOID unused) {
	(void)unused;
	DWORD failures = 0;
	for (int i = 0; i < STRESS_ITERATIONS; i++) {
		const char *p = getenv("PATH");
		const char *e = strerror(ENOENT);
		const char *d = localeconv()->decimal_point;
		if (!p || strcmp(p, g_expectedPath) != 0) {
			failures++;
		}
		if (!e || strcmp(e, g_expectedErr) != 0) {
			failures++;
		}
		if (strcmp(d, ".") != 0) {
			failures++;
		}
	}
	return failures;
}

int main(void) {
	// getenv results must stay valid across later getenv calls and heap reuse.
	const char *path = getenv("PATH");
	TEST_CHECK(path != NULL);
	size_t pathLen = strlen(path);
	char *saved = malloc(pathLen + 1);
	TEST_CHECK(saved != NULL);
	strcpy(saved, path);
	for (int i = 0; i < 64; i++) {
		getenv("HOME");
		getenv("WIBO_NO_SUCH_VARIABLE");
	}
	for (size_t n = 1; n <= pathLen + 64; n++) {
		char *p = malloc(n);
		if (p) {
			memset(p, 'X', n);
		}
	}
	TEST_CHECK_STR_EQ(saved, path);
	TEST_CHECK(getenv("PATH") == path);
	free(saved);


	// localeconv must follow setlocale. Skip when the host has no such locale.
	TEST_CHECK(setlocale(LC_NUMERIC, "C") != NULL);
	TEST_CHECK_STR_EQ("", localeconv()->thousands_sep);
	if (setlocale(LC_NUMERIC, "en_US.utf8")) {
		TEST_CHECK_STR_EQ(",", localeconv()->thousands_sep);
	}
	TEST_CHECK(setlocale(LC_NUMERIC, "C") != NULL);
	TEST_CHECK_STR_EQ("", localeconv()->thousands_sep);

	// A guest errno write must not be overwritten by a later failing msvcrt call.
	errno = 7;
	FILE *f = fopen("msvcrt_errno_missing_dir/none.txt", "rb");
	TEST_CHECK(f == NULL);
	TEST_CHECK_EQ(ENOENT, errno);

	// errno is per thread: a worker's failing call must not change ours.
	errno = 7;
	HANDLE thread = CreateThread(NULL, 0, errnoWorker, NULL, 0, NULL);
	TEST_CHECK(thread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(thread, 5000));
	CloseHandle(thread);
	TEST_CHECK_EQ(ENOENT, g_workerErrno);
	// strerror is per thread: another thread's call must not overwrite ours.
	char *expectedErr = copyOf(strerror(ENOENT));
	const char *mainErr = strerror(ENOENT);
	HANDLE strerrThread = CreateThread(NULL, 0, strerrorWorker, NULL, 0, NULL);
	TEST_CHECK(strerrThread != NULL);
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(strerrThread, 5000));
	CloseHandle(strerrThread);
	TEST_CHECK_STR_EQ(expectedErr, mainErr);

	// Concurrent getenv, strerror and localeconv calls must all return the right text.
	g_expectedPath = copyOf(path);
	g_expectedErr = expectedErr;
	HANDLE stress[STRESS_THREADS];
	for (int i = 0; i < STRESS_THREADS; i++) {
		stress[i] = CreateThread(NULL, 0, stressWorker, NULL, 0, NULL);
		TEST_CHECK(stress[i] != NULL);
	}
	for (int i = 0; i < STRESS_THREADS; i++) {
		TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(stress[i], 30000));
		DWORD failures = 1;
		TEST_CHECK(GetExitCodeThread(stress[i], &failures));
		TEST_CHECK_EQ(0, failures);
		CloseHandle(stress[i]);
	}

	return 0;
}
