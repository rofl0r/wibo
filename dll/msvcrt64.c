#include "msvcrt.h"

#include "common.h"
#include "context.h"
#include "cutil.h"
#include "files.h"
#include "kernel32/internal.h"
#include "modules.h"

#include "msvcrt_trampolines.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
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
#define MSVCRT_IOB_COUNT 32
#define MSVCRT_ONEXIT_MAX 32

// The MS FILE layout is opaque to guests. Slots 0-2 are stdin/stdout/stderr; slots 3+ hold fopen results.
static unsigned char g_iob[MSVCRT_IOB_COUNT * MSVCRT_FILE_SIZE] __attribute__((aligned(16)));
static FILE *g_hostStreams[MSVCRT_IOB_COUNT];
static int g_commode;
static int g_fmode;
static char **g_initenv;
static MSVCRT_ONEXIT_FN g_onexit[MSVCRT_ONEXIT_MAX];
static size_t g_onexitCount;

static long streamIndex(void *stream) {
	uintptr_t base = (uintptr_t)g_iob;
	uintptr_t p = (uintptr_t)stream;
	if (p < base || p >= base + sizeof g_iob || (p - base) % MSVCRT_FILE_SIZE) {
		return -1;
	}
	return (long)((p - base) / MSVCRT_FILE_SIZE);
}

static FILE *hostStream(void *stream) {
	long idx = streamIndex(stream);
	switch (idx) {
	case -1:
		return NULL;
	case 0:
		return stdin;
	case 1:
		return stdout;
	case 2:
		return stderr;
	default:
		return g_hostStreams[idx];
	}
}

typedef struct {
	char *data;
	size_t len;
	size_t cap;
} OutBuf;

static void outAppend(OutBuf *o, const char *s, size_t n) {
	if (o->len + n + 1 > o->cap) {
		size_t cap = o->cap ? o->cap : 128;
		while (cap < o->len + n + 1) {
			cap *= 2;
		}
		o->data = xrealloc(o->data, cap);
		o->cap = cap;
	}
	memcpy(o->data + o->len, s, n);
	o->len += n;
	o->data[o->len] = '\0';
}

// Formats one conversion with a host printf spec; the caller supplies arguments matching the spec.
static void appendHost(OutBuf *o, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	char *tmp = xmalloc((size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(tmp, (size_t)n + 1, fmt, ap);
	va_end(ap);
	outAppend(o, tmp, (size_t)n);
	free(tmp);
}

// Win64 variadic arguments are 8-byte slots. printf reads them from the host va_list that the thunk filled;
// vfprintf reads them from an MS va_list, which is a pointer to consecutive slots.
typedef struct {
	va_list *host;
	const uint64_t *ms;
} ArgSource;

static uint64_t nextArg(ArgSource *src) { return src->host ? va_arg(*src->host, uint64_t) : *src->ms++; }

static void appendRepeat(OutBuf *o, char c, size_t count) {
	for (size_t i = 0; i < count; i++) {
		outAppend(o, &c, 1);
	}
}

static void appendPadded(OutBuf *o, const char *s, size_t n, int width, int left) {
	size_t pad = width > 0 && (size_t)width > n ? (size_t)width - n : 0;
	if (!left) {
		appendRepeat(o, ' ', pad);
	}
	outAppend(o, s, n);
	if (left) {
		appendRepeat(o, ' ', pad);
	}
}

static void pushFlag(char *flags, size_t *count, char c) {
	if (*count < 7 && !strchr(flags, c)) {
		flags[(*count)++] = c;
		flags[*count] = '\0';
	}
}

// size is the MS argument width in bytes; the value is truncated to it, then widened for the host.
static void emitInteger(OutBuf *o, const char *flags, int width, int prec, char conv, int size, uint64_t raw) {
	char spec[32];
	if (conv == 'd' || conv == 'i') {
		long long v;
		switch (size) {
		case 1:
			v = (signed char)raw;
			break;
		case 2:
			v = (short)raw;
			break;
		case 4:
			v = (int)raw;
			break;
		default:
			v = (long long)raw;
			break;
		}
		snprintf(spec, sizeof spec, "%%%s*.*ll%c", flags, conv);
		appendHost(o, spec, width, prec, v);
		return;
	}
	unsigned long long v;
	switch (size) {
	case 1:
		v = (unsigned char)raw;
		break;
	case 2:
		v = (unsigned short)raw;
		break;
	case 4:
		v = (unsigned int)raw;
		break;
	default:
		v = raw;
		break;
	}
	snprintf(spec, sizeof spec, "%%%s*.*ll%c", flags, conv);
	appendHost(o, spec, width, prec, v);
}

// MS promotes float to double in variadics, so the slot holds the double's bit pattern.
static void emitFloat(OutBuf *o, const char *flags, int width, int prec, char conv, uint64_t raw) {
	double d;
	memcpy(&d, &raw, sizeof d);
	char spec[32];
	snprintf(spec, sizeof spec, "%%%s*.*%c", flags, conv);
	appendHost(o, spec, width, prec, d);
}

static void emitChar(OutBuf *o, uint64_t raw, int width, int left) {
	char c = (char)(unsigned char)raw;
	appendPadded(o, &c, 1, width, left);
}

static void emitString(OutBuf *o, uint64_t raw, int wide, int width, int prec, int left) {
	const void *ptr = (const void *)(uintptr_t)raw;
	if (!ptr) {
		ptr = "(null)";
		wide = 0;
	}
	if (!wide) {
		const char *s = ptr;
		size_t n = strnlen(s, prec < 0 ? SIZE_MAX : (size_t)prec);
		appendPadded(o, s, n, width, left);
		return;
	}
	const uint16_t *w = ptr;
	size_t n = 0;
	while ((prec < 0 || (long)n < prec) && w[n]) {
		n++;
	}
	char *narrow = wstr_to_ascii(w, n);
	appendPadded(o, narrow, n, width, left);
	free(narrow);
}

static void emitPointer(OutBuf *o, uint64_t raw) {
	char buf[24];
	snprintf(buf, sizeof buf, "%016llX", (unsigned long long)raw);
	outAppend(o, buf, 16);
}

static void storeCount(uint64_t ptr, int size, size_t count) {
	if (!ptr) {
		return;
	}
	void *p = (void *)(uintptr_t)ptr;
	switch (size) {
	case 1:
		*(signed char *)p = (signed char)count;
		break;
	case 2:
		*(short *)p = (short)count;
		break;
	case 4:
		*(int *)p = (int)count;
		break;
	default:
		*(long long *)p = (long long)count;
		break;
	}
}

static int parseDecimal(const char **p) {
	int v = 0;
	while (isdigit((unsigned char)**p)) {
		int d = **p - '0';
		if (v <= (INT_MAX - d) / 10) {
			v = v * 10 + d;
		}
		(*p)++;
	}
	return v;
}

// Implements MS printf conversions over 8-byte argument slots. Wide-string input is narrowed to its low bytes.
static void formatMs(OutBuf *o, LPCSTR fmt, ArgSource *src) {
	const char *p = fmt;
	while (*p) {
		const char *run = strchr(p, '%');
		outAppend(o, p, run ? (size_t)(run - p) : strlen(p));
		if (!run) {
			break;
		}
		const char *spec = run;
		p = run + 1;
		if (*p == '%') {
			outAppend(o, "%", 1);
			p++;
			continue;
		}

		char flags[8] = {0};
		size_t flagCount = 0;
		while (*p && strchr("-+ #0", *p)) {
			pushFlag(flags, &flagCount, *p);
			p++;
		}
		int left = strchr(flags, '-') != NULL;

		int width = -1;
		if (*p == '*') {
			p++;
			width = (int)nextArg(src);
			if (width < 0) {
				width = width == INT_MIN ? INT_MAX : -width;
				pushFlag(flags, &flagCount, '-');
				left = 1;
			}
		} else if (isdigit((unsigned char)*p)) {
			width = parseDecimal(&p);
		}

		int prec = -1;
		if (*p == '.') {
			p++;
			if (*p == '*') {
				p++;
				prec = (int)nextArg(src);
			} else {
				prec = parseDecimal(&p);
			}
			if (prec < 0) {
				prec = -1;
			}
		}

		// size: 1 = hh, 2 = h, 4 = default/l/I32, 8 = ll/I64/z/j/t/I. A bare 'l' marks wide characters and strings.
		int size = 4;
		int wide = 0;
		if (p[0] == 'h' && p[1] == 'h') {
			size = 1;
			p += 2;
		} else if (p[0] == 'h') {
			size = 2;
			p++;
		} else if (p[0] == 'l' && p[1] == 'l') {
			size = 8;
			p += 2;
		} else if (p[0] == 'l') {
			wide = 1;
			p++;
		} else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
			size = 8;
			p += 3;
		} else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') {
			p += 3;
		} else if (p[0] == 'I' || p[0] == 'j' || p[0] == 'z' || p[0] == 't') {
			size = 8;
			p++;
		} else if (p[0] == 'w' || p[0] == 'L') {
			p++;
		}

		char conv = *p;
		if (!conv) {
			outAppend(o, spec, (size_t)(p - spec));
			break;
		}
		p++;
		switch (conv) {
		case 'd':
		case 'i':
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			emitInteger(o, flags, width < 0 ? 0 : width, prec, conv, size, nextArg(src));
			break;
		case 'c':
		case 'C':
			emitChar(o, nextArg(src), width, left);
			break;
		case 's':
			emitString(o, nextArg(src), wide, width, prec, left);
			break;
		case 'S':
			emitString(o, nextArg(src), 1, width, prec, left);
			break;
		case 'p':
			emitPointer(o, nextArg(src));
			break;
		case 'n':
			storeCount(nextArg(src), size, o->len);
			break;
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
		case 'a':
		case 'A':
			emitFloat(o, flags, width < 0 ? 0 : width, prec, conv, nextArg(src));
			break;
		default:
			outAppend(o, spec, (size_t)(p - spec));
			break;
		}
	}
}

static int printToStream(FILE *f, LPCSTR fmt, ArgSource *src) {
	OutBuf o = {0};
	formatMs(&o, fmt, src);
	int n = (int)o.len;
	if (o.len && fwrite(o.data, 1, o.len, f) != o.len) {
		n = -1;
	}
	free(o.data);
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
	ArgSource src = {.host = &ap, .ms = NULL};
	int n = printToStream(f, format, &src);
	va_end(ap);
	return n;
}

int CDECL msvcrt_vfprintf(void *stream, LPCSTR format, void *ap) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	if (!f) {
		return -1;
	}
	ArgSource src = {.host = NULL, .ms = ap};
	return printToStream(f, format, &src);
}

int CDECL msvcrt_printf(LPCSTR format, ...) {
	HOST_CONTEXT_GUARD();
	va_list ap;
	va_start(ap, format);
	ArgSource src = {.host = &ap, .ms = NULL};
	int n = printToStream(stdout, format, &src);
	va_end(ap);
	return n;
}

// Maps an MS fopen mode to a host mode. Text/binary, encoding, and commit flags have no effect on Linux.
static int hostOpenMode(LPCSTR mode, char out[5]) {
	char base = mode[0];
	if (base != 'r' && base != 'w' && base != 'a') {
		return 0;
	}
	int plus = 0;
	int excl = 0;
	for (const char *p = mode + 1; *p && *p != ','; p++) {
		if (*p == '+') {
			plus = 1;
		} else if (*p == 'x') {
			excl = 1;
		}
	}
	char *w = out;
	*w++ = base;
	if (excl && base == 'w') {
		*w++ = 'x';
	}
	if (plus) {
		*w++ = '+';
	}
	*w = '\0';
	return 1;
}

void *CDECL msvcrt_fopen(LPCSTR filename, LPCSTR mode) {
	HOST_CONTEXT_GUARD();
	char hostMode[5];
	if (!filename || !mode || !hostOpenMode(mode, hostMode)) {
		errno = EINVAL;
		return NULL;
	}
	size_t idx = 3;
	while (idx < MSVCRT_IOB_COUNT && g_hostStreams[idx]) {
		idx++;
	}
	if (idx == MSVCRT_IOB_COUNT) {
		errno = EMFILE;
		return NULL;
	}
	char *path = files_pathFromWindows(filename);
	FILE *f = fopen(path, hostMode);
	free(path);
	if (!f) {
		return NULL;
	}
	g_hostStreams[idx] = f;
	return g_iob + idx * MSVCRT_FILE_SIZE;
}

// The standard streams are never closed; fclose only flushes them.
int CDECL msvcrt_fclose(void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	if (!f) {
		errno = EINVAL;
		return EOF;
	}
	long idx = streamIndex(stream);
	if (idx < 3) {
		return fflush(f);
	}
	g_hostStreams[idx] = NULL;
	return fclose(f);
}

int CDECL msvcrt_fflush(void *stream) {
	HOST_CONTEXT_GUARD();
	if (!stream) {
		return fflush(NULL);
	}
	FILE *f = hostStream(stream);
	if (!f) {
		errno = EINVAL;
		return EOF;
	}
	return fflush(f);
}

SIZE_T CDECL msvcrt_fread(void *buf, SIZE_T size, SIZE_T count, void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? fread(buf, size, count, f) : 0;
}

LPSTR CDECL msvcrt_fgets(LPSTR buf, int n, void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? fgets(buf, n, f) : NULL;
}

int CDECL msvcrt_fputs(LPCSTR str, void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? fputs(str, f) : EOF;
}

int CDECL msvcrt_fgetc(void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? fgetc(f) : EOF;
}

int CDECL msvcrt_getc(void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? fgetc(f) : EOF;
}

int CDECL msvcrt_ungetc(int ch, void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? ungetc(ch, f) : EOF;
}

// The MS offset and origin are 32-bit; the high bits of the 64-bit slot are not meaningful.
int CDECL msvcrt_fseek(void *stream, LONG offset, int origin) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? fseek(f, (long)offset, origin) : -1;
}

LONG CDECL msvcrt_ftell(void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? (LONG)ftell(f) : -1;
}

void CDECL msvcrt_rewind(void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	if (f) {
		rewind(f);
	}
}

int CDECL msvcrt_feof(void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? feof(f) : 0;
}

int CDECL msvcrt_ferror(void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	return f ? ferror(f) : 0;
}

void CDECL msvcrt_clearerr(void *stream) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	if (f) {
		clearerr(f);
	}
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

// MS CRT signal numbers differ from the host's, so the host numbering is never used.
static const int kMsSignals[] = {2, 4, 8, 11, 15, 21, 22};
#define MSVCRT_SIGNAL_COUNT (sizeof(kMsSignals) / sizeof(kMsSignals[0]))
#define MSVCRT_SIG_ABRT 22
#define MSVCRT_SIG_DFL ((MSVCRT_SIGHANDLER)NULL)
#define MSVCRT_SIG_IGN ((MSVCRT_SIGHANDLER)1)

static MSVCRT_SIGHANDLER g_sigHandlers[MSVCRT_SIGNAL_COUNT];

static int sigSlot(int sig) {
	for (size_t i = 0; i < MSVCRT_SIGNAL_COUNT; i++) {
		if (kMsSignals[i] == sig) {
			return (int)i;
		}
	}
	return -1;
}

// The default action (and the post-handler behaviour of abort) terminates with status 3, like the MS CRT.
_Noreturn static void terminateDefault(void) {
	fflush(NULL);
	kernel32_exitInternal(3);
}

// Handlers are reset to SIG_DFL before being invoked, as the MS CRT does, so a handler that re-raises terminates.
static void invokeHandler(int slot, int sig) {
	MSVCRT_SIGHANDLER handler = g_sigHandlers[slot];
	g_sigHandlers[slot] = MSVCRT_SIG_DFL;
	call_MSVCRT_SIGHANDLER(handler, sig);
}

void CDECL msvcrt_abort(void) {
	HOST_CONTEXT_GUARD();
	int slot = sigSlot(MSVCRT_SIG_ABRT);
	MSVCRT_SIGHANDLER handler = g_sigHandlers[slot];
	if (handler != MSVCRT_SIG_DFL && handler != MSVCRT_SIG_IGN) {
		invokeHandler(slot, MSVCRT_SIG_ABRT);
	}
	terminateDefault();
}

int CDECL msvcrt_raise(int sig) {
	HOST_CONTEXT_GUARD();
	int slot = sigSlot(sig);
	if (slot < 0) {
		errno = EINVAL;
		return -1;
	}
	MSVCRT_SIGHANDLER handler = g_sigHandlers[slot];
	if (handler == MSVCRT_SIG_IGN) {
		return 0;
	}
	if (handler == MSVCRT_SIG_DFL) {
		terminateDefault();
	}
	invokeHandler(slot, sig);
	return 0;
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

// Handlers run only from raise() and abort(); there is no asynchronous signal delivery.
void *CDECL msvcrt_signal(int sig, void *handler) {
	HOST_CONTEXT_GUARD();
	int slot = sigSlot(sig);
	if (slot < 0) {
		errno = EINVAL;
		return (void *)-1;
	}
	MSVCRT_SIGHANDLER previous = g_sigHandlers[slot];
	g_sigHandlers[slot] = (MSVCRT_SIGHANDLER)handler;
	return (void *)previous;
}

ModuleStub lib_msvcrt = {
	.names =
		(const char *[]){
			"msvcrt",
			NULL,
		},
	.byName = msvcrtResolveByName,
};
