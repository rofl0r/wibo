#include "msvcrt.h"

#include "common.h"
#include "context.h"
#include "kernel32/internal.h"
#include "modules.h"

#include "msvcrt_trampolines.h"

#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(__x86_64__)
#error "msvcrt64.c implements the Win64 CRT and must be built for x86_64"
#endif

extern char **environ;

#define MSVCRT_FILE_SIZE 48
#define MSVCRT_ONEXIT_MAX 32

// The MS FILE layout is opaque to guests; only the addresses of stdin/stdout/stderr matter.
static unsigned char g_iob[3 * MSVCRT_FILE_SIZE] __attribute__((aligned(16)));
static int g_commode;
static int g_fmode;
static char **g_initenv;
static MSVCRT_ONEXIT_FN g_onexit[MSVCRT_ONEXIT_MAX];
static size_t g_onexitCount;

static FILE *hostStream(void *stream) {
	uintptr_t base = (uintptr_t)g_iob;
	uintptr_t p = (uintptr_t)stream;
	if (p < base || p >= base + sizeof g_iob || (p - base) % MSVCRT_FILE_SIZE) {
		return NULL;
	}
	switch ((p - base) / MSVCRT_FILE_SIZE) {
	case 0:
		return stdin;
	case 1:
		return stdout;
	case 2:
		return stderr;
	default:
		return NULL;
	}
}

// Rewrites MS printf length modifiers for the host: I64 -> ll, I32 and l -> int width, I -> z.
// The output is at most twice the input length.
static char *translateFormat(const char *fmt) {
	char *out = xmalloc(strlen(fmt) * 2 + 1);
	char *w = out;
	const char *p = fmt;
	while (*p) {
		if (*p != '%') {
			*w++ = *p++;
			continue;
		}
		*w++ = *p++;
		while (*p && strchr("-+ #0123456789.*", *p)) {
			*w++ = *p++;
		}
		if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
			p += 3;
			*w++ = 'l';
			*w++ = 'l';
		} else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') {
			p += 3;
		} else if (*p == 'I') {
			p++;
			*w++ = 'z';
		} else if (*p == 'l' && p[1] != 'l' && p[1] && strchr("diuxXo", p[1])) {
			p++;
		}
		if (*p) {
			*w++ = *p++;
		}
	}
	*w = '\0';
	return out;
}

static int printfTo(FILE *f, LPCSTR fmt, va_list ap) {
	char *translated = translateFormat(fmt);
	int n = vfprintf(f, translated, ap);
	free(translated);
	return n;
}

// The MS va_list is a char* over sequential 8-byte slots. A SysV va_list with every register area consumed
// makes va_arg read those slots from overflow_arg_area in order.
typedef struct {
	unsigned int gpOffset;
	unsigned int fpOffset;
	void *overflowArgArea;
	void *regSaveArea;
} SysVVaListTag;

static int printfToMsVaList(FILE *f, LPCSTR fmt, void *msAp) {
	va_list ap;
	SysVVaListTag *tag = (SysVVaListTag *)ap;
	tag->gpOffset = 6 * 8;
	tag->fpOffset = 6 * 8 + 8 * 16;
	tag->overflowArgArea = msAp;
	tag->regSaveArea = NULL;
	char *translated = translateFormat(fmt);
	int n = vfprintf(f, translated, ap);
	free(translated);
	return n;
}

static void *msvcrtDataByName(const char *name) {
	if (strcmp(name, "_commode") == 0) {
		return &g_commode;
	}
	if (strcmp(name, "_fmode") == 0) {
		return &g_fmode;
	}
	if (strcmp(name, "__initenv") == 0) {
		return &g_initenv;
	}
	return NULL;
}

static void *msvcrtResolveByName(const char *name) {
	void *data = msvcrtDataByName(name);
	return data ? data : msvcrtThunkByName(name);
}

static void runOnexitTable(void) {
	while (g_onexitCount) {
		MSVCRT_ONEXIT_FN fn = g_onexit[--g_onexitCount];
		call_MSVCRT_ONEXIT_FN(fn);
	}
	fflush(NULL);
}

void *CDECL msvcrt_malloc(SIZE_T size) {
	HOST_CONTEXT_GUARD();
	return malloc(size);
}

void *CDECL msvcrt_calloc(SIZE_T count, SIZE_T size) {
	HOST_CONTEXT_GUARD();
	return calloc(count, size);
}

void CDECL msvcrt_free(void *ptr) {
	HOST_CONTEXT_GUARD();
	free(ptr);
}

void *CDECL msvcrt_memcpy(void *dest, const void *src, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return memcpy(dest, src, count);
}

void *CDECL msvcrt_memset(void *dest, int ch, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return memset(dest, ch, count);
}

int CDECL msvcrt_memcmp(const void *buf1, const void *buf2, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return memcmp(buf1, buf2, count);
}

static int qsortAdapter(const void *a, const void *b, void *compare) {
	return call_MSVCRT_COMPARE_FN((MSVCRT_COMPARE_FN)compare, a, b);
}

void CDECL msvcrt_qsort(void *base, SIZE_T num, SIZE_T width, MSVCRT_COMPARE_FN compare) {
	HOST_CONTEXT_GUARD();
	qsort_r(base, num, width, qsortAdapter, (void *)compare);
}

SIZE_T CDECL msvcrt_strlen(LPCSTR str) {
	HOST_CONTEXT_GUARD();
	return strlen(str);
}

int CDECL msvcrt_strcmp(LPCSTR a, LPCSTR b) {
	HOST_CONTEXT_GUARD();
	return strcmp(a, b);
}

int CDECL msvcrt_strncmp(LPCSTR a, LPCSTR b, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return strncmp(a, b, count);
}

LPSTR CDECL msvcrt_strcpy(LPSTR dest, LPCSTR src) {
	HOST_CONTEXT_GUARD();
	return strcpy(dest, src);
}

LPSTR CDECL msvcrt_strncpy(LPSTR dest, LPCSTR src, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return strncpy(dest, src, count);
}

LPSTR CDECL msvcrt_strcat(LPSTR dest, LPCSTR src) {
	HOST_CONTEXT_GUARD();
	return strcat(dest, src);
}

LPSTR CDECL msvcrt_strchr(LPCSTR str, int ch) {
	HOST_CONTEXT_GUARD();
	return strchr(str, ch);
}

LPSTR CDECL msvcrt_strrchr(LPCSTR str, int ch) {
	HOST_CONTEXT_GUARD();
	return strrchr(str, ch);
}

LPSTR CDECL msvcrt_strerror(int errnum) {
	HOST_CONTEXT_GUARD();
	return strerror(errnum);
}

int CDECL msvcrt_tolower(int ch) {
	HOST_CONTEXT_GUARD();
	return tolower(ch);
}

// MS wchar_t is 16-bit, so the host wide-string routines (32-bit wchar_t) cannot be reused.
SIZE_T CDECL msvcrt_wcslen(LPCWSTR str) {
	HOST_CONTEXT_GUARD();
	SIZE_T n = 0;
	while (str[n]) {
		n++;
	}
	return n;
}

int CDECL msvcrt_wcscmp(LPCWSTR a, LPCWSTR b) {
	HOST_CONTEXT_GUARD();
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (int)*a - (int)*b;
}

int CDECL msvcrt_wcsncmp(LPCWSTR a, LPCWSTR b, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	for (SIZE_T i = 0; i < count; i++) {
		if (a[i] != b[i] || !a[i]) {
			return (int)a[i] - (int)b[i];
		}
	}
	return 0;
}

LPWSTR CDECL msvcrt_wcsrchr(LPCWSTR str, WCHAR ch) {
	HOST_CONTEXT_GUARD();
	LPCWSTR last = NULL;
	for (; *str; str++) {
		if (*str == ch) {
			last = str;
		}
	}
	if (ch == 0) {
		last = str;
	}
	return (LPWSTR)last;
}

void *CDECL msvcrt___iob_func(void) {
	HOST_CONTEXT_GUARD();
	return g_iob;
}

int CDECL msvcrt_fprintf(void *stream, LPCSTR format, ...) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	if (!f) {
		return -1;
	}
	va_list ap;
	va_start(ap, format);
	int n = printfTo(f, format, ap);
	va_end(ap);
	return n;
}

int CDECL msvcrt_vfprintf(void *stream, LPCSTR format, void *ap) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	if (!f) {
		return -1;
	}
	return printfToMsVaList(f, format, ap);
}

int CDECL msvcrt_printf(LPCSTR format, ...) {
	HOST_CONTEXT_GUARD();
	va_list ap;
	va_start(ap, format);
	int n = printfTo(stdout, format, ap);
	va_end(ap);
	return n;
}

int CDECL msvcrt_fputc(int ch, void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? fputc(ch, f) : EOF;
}

SIZE_T CDECL msvcrt_fwrite(const void *buf, SIZE_T size, SIZE_T count, void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? fwrite(buf, size, count, f) : 0;
}

int CDECL msvcrt_puts(LPCSTR str) {
	HOST_CONTEXT_GUARD();
	return puts(str);
}

// exit runs the _onexit table, flushes host stdio, and exits without the MS atexit cleanup.
void CDECL msvcrt_exit(int code) {
	HOST_CONTEXT_GUARD();
	runOnexitTable();
	kernel32_exitInternal((DWORD)code);
}

void CDECL msvcrt__cexit(void) {
	HOST_CONTEXT_GUARD();
	fflush(NULL);
}

void CDECL msvcrt__amsg_exit(int code) {
	HOST_CONTEXT_GUARD();
	fprintf(stderr, "runtime error R%d\n", code);
	fflush(NULL);
	kernel32_exitInternal(255);
}

void CDECL msvcrt_abort(void) {
	HOST_CONTEXT_GUARD();
	fflush(NULL);
	kernel32_exitInternal(3);
}

void *CDECL msvcrt__onexit(MSVCRT_ONEXIT_FN fn) {
	HOST_CONTEXT_GUARD();
	if (g_onexitCount >= MSVCRT_ONEXIT_MAX) {
		return NULL;
	}
	g_onexit[g_onexitCount++] = fn;
	return (void *)fn;
}

void CDECL msvcrt__initterm(MSVCRT_PVFV *begin, MSVCRT_PVFV *end) {
	HOST_CONTEXT_GUARD();
	for (MSVCRT_PVFV *fn = begin; fn < end; fn++) {
		if (*fn) {
			call_MSVCRT_PVFV(*fn);
		}
	}
}

int CDECL msvcrt___getmainargs(int *argc, char ***argv, char ***envp, int doWildCard, void *startInfo) {
	HOST_CONTEXT_GUARD();
	(void)doWildCard;
	(void)startInfo;
	*argc = wibo_argc;
	*argv = wibo_argv;
	g_initenv = environ;
	*envp = environ;
	return 0;
}

// The app type, locks, and the usermatherr handler are only consulted by MS-specific runtime code we do not provide.
void CDECL msvcrt___set_app_type(int type) {
	HOST_CONTEXT_GUARD();
	(void)type;
}

void CDECL msvcrt__lock(int locknum) {
	HOST_CONTEXT_GUARD();
	(void)locknum;
}

void CDECL msvcrt__unlock(int locknum) {
	HOST_CONTEXT_GUARD();
	(void)locknum;
}

void CDECL msvcrt___setusermatherr(void *handler) {
	HOST_CONTEXT_GUARD();
	(void)handler;
}

// EXCEPTION_CONTINUE_SEARCH: no guest __try/__except handlers are supported.
int CDECL msvcrt___C_specific_handler(void) {
	HOST_CONTEXT_GUARD();
	return 1;
}

int CDECL msvcrt____mb_cur_max_func(void) {
	HOST_CONTEXT_GUARD();
	return MB_CUR_MAX;
}

unsigned int CDECL msvcrt____lc_codepage_func(void) {
	HOST_CONTEXT_GUARD();
	return 0;
}

int *CDECL msvcrt__errno(void) {
	HOST_CONTEXT_GUARD();
	return &errno;
}

void *CDECL msvcrt_localeconv(void) {
	HOST_CONTEXT_GUARD();
	return localeconv();
}

// Handlers are not installed; returning NULL reports SIG_DFL as the previous handler.
void *CDECL msvcrt_signal(int sig, void *handler) {
	HOST_CONTEXT_GUARD();
	(void)sig;
	(void)handler;
	return NULL;
}

ModuleStub lib_msvcrt = {
	.names =
		(const char *[]){
			"msvcrt",
			NULL,
		},
	.byName = msvcrtResolveByName,
};
