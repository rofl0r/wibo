#include <windows.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "test_assert.h"

static int g_sigCount;
static int g_sigArg;

static void onSignal(int sig) {
	g_sigCount++;
	g_sigArg = sig;
}

int main(void) {
	const char *path = "msvcrt_stdio_test.tmp";
	char buf[64];

	FILE *f = fopen(path, "wb");
	TEST_CHECK(f != NULL);
	TEST_CHECK(fputs("line one\n", f) >= 0);
	TEST_CHECK_EQ(4, fprintf(f, "%d:%s\n", 3, "x"));
	TEST_CHECK_EQ(0, fclose(f));

	f = fopen(path, "rb");
	TEST_CHECK(f != NULL);
	TEST_CHECK(fgets(buf, sizeof buf, f) != NULL);
	TEST_CHECK_STR_EQ("line one\n", buf);
	TEST_CHECK_EQ('3', fgetc(f));
	TEST_CHECK_EQ('Q', ungetc('Q', f));
	TEST_CHECK_EQ('Q', fgetc(f));
	TEST_CHECK_EQ(':', fgetc(f));
	TEST_CHECK_EQ(0, fclose(f));

	f = fopen(path, "rb");
	TEST_CHECK(f != NULL);
	TEST_CHECK_EQ(0, fseek(f, 0, SEEK_END));
	TEST_CHECK_EQ(13, ftell(f));

	rewind(f);
	TEST_CHECK_EQ(4, fread(buf, 1, 4, f));
	buf[4] = '\0';
	TEST_CHECK_STR_EQ("line", buf);
	TEST_CHECK(!feof(f));

	TEST_CHECK_EQ(0, fseek(f, 0, SEEK_END));
	TEST_CHECK_EQ(EOF, fgetc(f));
	TEST_CHECK(feof(f));
	TEST_CHECK_EQ(0, ferror(f));
	clearerr(f);
	TEST_CHECK(!feof(f));
	TEST_CHECK_EQ(0, fclose(f));

	f = fopen(path, "ab");
	TEST_CHECK(f != NULL);
	TEST_CHECK_EQ(4, fwrite("end\n", 1, 4, f));
	TEST_CHECK_EQ(0, fclose(f));

	f = fopen(path, "rb");
	TEST_CHECK(f != NULL);
	TEST_CHECK_EQ(0, fseek(f, 13, SEEK_SET));
	TEST_CHECK(fgets(buf, sizeof buf, f) != NULL);
	TEST_CHECK_STR_EQ("end\n", buf);
	TEST_CHECK(fgets(buf, sizeof buf, f) == NULL);
	TEST_CHECK_EQ(0, fclose(f));

	TEST_CHECK(fopen("msvcrt_stdio_missing.tmp", "r") == NULL);
	TEST_CHECK(DeleteFileA(path));

	void (*prev)(int) = signal(SIGINT, onSignal);
	TEST_CHECK(prev == SIG_DFL);
	TEST_CHECK_EQ(0, raise(SIGINT));
	TEST_CHECK_EQ(1, g_sigCount);
	TEST_CHECK_EQ(SIGINT, g_sigArg);

	// The handler is reset to SIG_DFL before it runs, so the next signal() call sees the default.
	prev = signal(SIGINT, SIG_IGN);
	TEST_CHECK(prev == SIG_DFL);
	TEST_CHECK_EQ(0, raise(SIGINT));
	TEST_CHECK_EQ(1, g_sigCount);

	TEST_CHECK(signal(99, onSignal) == SIG_ERR);
	return 0;
}
