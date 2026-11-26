#include "msvcrt.h"

#include "common.h"
#include "context.h"
#include "cutil.h"
#include "files.h"
#include "heap.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "strutil.h"

#include "msvcrt_trampolines.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wctype.h>

extern char **environ;

// Guest code indexes _iob with the MS FILE size for its ABI (32 bytes on x86, 48 on x64).
#ifdef WIBO_GUEST64
#define MSVCRT_FILE_SIZE 48
#else
#define MSVCRT_FILE_SIZE 32
#endif
#define MSVCRT_IOB_COUNT 32
#define MSVCRT_ONEXIT_MAX 32
// MS setvbuf modes; these differ from glibc's values, so they are mapped
// explicitly.
#define MSVCRT_IOFBF 0x0000
#define MSVCRT_IOLBF 0x0040
#define MSVCRT_IONBF 0x0004

// The MS FILE layout is opaque to guests. Slots 0-2 are stdin/stdout/stderr;
// slots 3+ hold fopen results.
static unsigned char g_iob[MSVCRT_IOB_COUNT * MSVCRT_FILE_SIZE] __attribute__((aligned(16)));
static FILE *g_hostStreams[MSVCRT_IOB_COUNT];
static int g_commode;
static int g_fmode;
static GUEST_PTR g_initenv;
static MSVCRT_PVFV g_onexit[MSVCRT_ONEXIT_MAX];
static size_t g_onexitCount;

// On wibo32 the host errno is thread-local storage above 4 GB, so guests see a
// mirror cell instead. Like POSIX errno, the cell is per thread. Every msvcrt
// entry point copies a guest write into the host errno on entry, and copies the
// host errno back into the cell on exit. Errno changes made by non-msvcrt shims
// only reach the guest after the next msvcrt call.
#ifndef WIBO_GUEST64
static __thread int *t_errnoCell;
static __thread int t_errnoShadow;
// Used only if the per-thread cell cannot be allocated.
static int g_errnoFallback;

static int *errnoCell(void) {
	if (!t_errnoCell) {
		int *cell = wibo_heap_guestMalloc(sizeof *cell);
		if (!cell) {
			return &g_errnoFallback;
		}
		*cell = 0;
		t_errnoCell = cell;
	}
	return t_errnoCell;
}

static int errnoSyncIn(void) {
	int *cell = errnoCell();
	if (*cell != t_errnoShadow) {
		errno = *cell;
	}
	return 0;
}

static void errnoSyncOut(const int *unused) {
	(void)unused;
	int hostErrno = errno;
	int *cell = errnoCell();
	*cell = hostErrno;
	t_errnoShadow = hostErrno;
}

// Called by generated thunks before every guest-to-host shim runs (see errors.h). Refreshing the shadow here
// keeps the exit hook below from mistaking a stale shadow for a shim write.
void wibo_errnoGuestToHost(void) {
	int guestErrno = *errnoCell();
	errno = guestErrno;
	t_errnoShadow = guestErrno;
}

// Called by generated thunks after every guest-to-host shim returns (see errors.h). If the shim
// left errno at the value last synced, the guest's own write to the cell is kept.
void wibo_errnoHostToGuest(void) {
	int hostErrno = errno;
	if (hostErrno == t_errnoShadow) {
		return;
	}
	*errnoCell() = hostErrno;
	t_errnoShadow = hostErrno;
}

// Redefined for this file only so that every msvcrt entry point gets the sync.
#undef HOST_CONTEXT_GUARD
#define HOST_CONTEXT_GUARD() \
	int errnoSync_ __attribute__((cleanup(errnoSyncOut))) = errnoSyncIn()
#endif

// Guest-visible allocations come from the guest heap on wibo32 so that their
// addresses fit in 32 bits.
static void *guestAlloc(size_t size) {
#ifdef WIBO_GUEST64
	return calloc(1, size);
#else
	void *p = wibo_heap_guestMalloc(size);
	if (p) {
		memset(p, 0, size);
	}
	return p;
#endif
}

static void guestFree(void *ptr) {
#ifdef WIBO_GUEST64
	free(ptr);
#else
	wibo_heap_guestFree(ptr);
#endif
}

// Serializes _putenv's host environment change and the shared guest string
// slots. Per-thread slots need no lock.
static pthread_mutex_t g_guestStringMutex = PTHREAD_MUTEX_INITIALIZER;

#ifndef WIBO_GUEST64
static char *guestStringCopy(const char *s) {
	size_t n = strlen(s) + 1;
	char *copy = wibo_heap_guestMalloc(n);
	if (copy) {
		memcpy(copy, s, n);
	}
	return copy;
}

// A slot owns at most one guest copy. Refilling it frees the previous copy, so
// the pointer stays valid until the same slot is refilled. A NULL source leaves
// the slot untouched.
static void refillGuestSlot(char **slot, const char *s) {
	if (!s) {
		return;
	}
	char *copy = guestStringCopy(s);
	if (!copy) {
		return;
	}
	if (*slot) {
		guestFree(*slot);
	}
	*slot = copy;
}

// MS struct lconv: ten string pointers followed by eight chars, with pointers
// as wide as guest pointers.
typedef struct {
	GUEST_PTR strings[10];
	char chars[8];
} MsLconv;
#endif

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

// Formats one conversion with a host printf spec; the caller supplies arguments
// matching the spec.
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

// Variadic arguments occupy one slot per guest argument: 4 bytes for wibo32
// guests and 8 bytes for wibo64 guests. The host va_list always holds 8-byte
// slots, because gentramp widens each guest slot before calling the host.
// printf reads the host va_list that the thunk filled; vfprintf reads an MS
// va_list, a pointer to consecutive guest slots.
typedef struct {
	va_list *host;
	const GUEST_PTR *ms;
} ArgSource;

static uint64_t nextArg(ArgSource *src) { return src->host ? va_arg(*src->host, uint64_t) : *src->ms++; }

// An 8-byte argument (long long, double) takes two slots on wibo32, low word
// first.
static uint64_t nextArg64(ArgSource *src) {
#ifdef WIBO_GUEST64
	return nextArg(src);
#else
	uint64_t lo = nextArg(src);
	uint64_t hi = nextArg(src);
	return lo | hi << 32;
#endif
}

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

// size is the MS argument width in bytes; the value is truncated to it, then
// widened for the host.
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

// MS promotes float to double in variadics, so the slot holds the double's bit
// pattern.
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
	const int digits = (int)sizeof(GUEST_PTR) * 2;
	snprintf(buf, sizeof buf, "%0*llX", digits, (unsigned long long)raw);
	outAppend(o, buf, (size_t)digits);
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

// Implements MS printf conversions over guest argument slots. Wide-string input
// is narrowed to its low bytes.
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

		// size: 1 = hh, 2 = h, 4 = default/l/I32, 8 = ll/I64/z/j/t/I. A bare 'l'
		// marks wide characters and strings.
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
			emitInteger(o, flags, width < 0 ? 0 : width, prec, conv, size, size == 8 ? nextArg64(src) : nextArg(src));
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
			emitFloat(o, flags, width < 0 ? 0 : width, prec, conv, nextArg64(src));
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
	if (strcmp(name, "_iob") == 0) {
		return g_iob;
	}
	return NULL;
}

static void *msvcrtResolveByName(const char *name) {
	void *data = msvcrtDataByName(name);
	return data ? data : msvcrtThunkByName(name);
}

static void runOnexitTable(void) {
	while (g_onexitCount) {
		MSVCRT_PVFV fn = g_onexit[--g_onexitCount];
		call_MSVCRT_PVFV(fn);
	}
	fflush(NULL);
}

void *CDECL msvcrt_malloc(SIZE_T size) {
	HOST_CONTEXT_GUARD();
	return guestAlloc(size + 4);
}

void *CDECL msvcrt_calloc(SIZE_T count, SIZE_T size) {
	HOST_CONTEXT_GUARD();
	if (size && count > (SIZE_T)-5 / size) {
		return NULL;
	}
	return guestAlloc(count * size + 4);
}

void CDECL msvcrt_free(void *ptr) {
	HOST_CONTEXT_GUARD();
	guestFree(ptr);
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

static int memicmpImpl(const void *buf1, const void *buf2, SIZE_T count) {
	const unsigned char *a = buf1;
	const unsigned char *b = buf2;
	for (SIZE_T i = 0; i < count; i++) {
		int ca = tolower(a[i]);
		int cb = tolower(b[i]);
		if (ca != cb) {
			return ca - cb;
		}
	}
	return 0;
}

int CDECL msvcrt__memicmp(const void *buf1, const void *buf2, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return memicmpImpl(buf1, buf2, count);
}

int CDECL msvcrt_memicmp(const void *buf1, const void *buf2, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return memicmpImpl(buf1, buf2, count);
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

// The MS CRT names for case-insensitive compares: the underscore forms and the
// deprecated aliases are separate exports with the same behaviour.
int CDECL msvcrt__stricmp(LPCSTR a, LPCSTR b) {
	HOST_CONTEXT_GUARD();
	return strcasecmp(a, b);
}

int CDECL msvcrt_stricmp(LPCSTR a, LPCSTR b) {
	HOST_CONTEXT_GUARD();
	return strcasecmp(a, b);
}

int CDECL msvcrt_strcmpi(LPCSTR a, LPCSTR b) {
	HOST_CONTEXT_GUARD();
	return strcasecmp(a, b);
}

int CDECL msvcrt__strcmpi(LPCSTR a, LPCSTR b) {
	HOST_CONTEXT_GUARD();
	return strcasecmp(a, b);
}

int CDECL msvcrt__strnicmp(LPCSTR a, LPCSTR b, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return strncasecmp(a, b, count);
}

int CDECL msvcrt_strnicmp(LPCSTR a, LPCSTR b, SIZE_T count) {
	HOST_CONTEXT_GUARD();
	return strncasecmp(a, b, count);
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

// The copy is owned by the guest and released with free(), so it comes from the
// same allocator as malloc().
static LPSTR duplicateGuestString(LPCSTR str) {
	if (!str) {
		errno = EINVAL;
		return NULL;
	}
	SIZE_T n = strlen(str) + 1;
	char *copy = guestAlloc(n);
	if (!copy) {
		errno = ENOMEM;
		return NULL;
	}
	memcpy(copy, str, n);
	return copy;
}

LPSTR CDECL msvcrt__strdup(LPCSTR str) {
	HOST_CONTEXT_GUARD();
	return duplicateGuestString(str);
}

LPSTR CDECL msvcrt_strdup(LPCSTR str) {
	HOST_CONTEXT_GUARD();
	return duplicateGuestString(str);
}

// In-place case conversion on the guest buffer. Returns the same pointer, as the MS CRT does.
LPSTR CDECL msvcrt__strlwr(LPSTR str) {
	HOST_CONTEXT_GUARD();
	if (!str) {
		return NULL;
	}
	for (LPSTR p = str; *p; p++) {
		*p = (char)tolower((unsigned char)*p);
	}
	return str;
}

LPSTR CDECL msvcrt__strupr(LPSTR str) {
	HOST_CONTEXT_GUARD();
	if (!str) {
		return NULL;
	}
	for (LPSTR p = str; *p; p++) {
		*p = (char)toupper((unsigned char)*p);
	}
	return str;
}

// The MS CRT long is 32 bits on both guest widths, so the host's 64-bit result is clamped to that range.
static LONG strtolClamped(const char *str, char **end, int base, bool *overflow) {
	long long value = strtoll(str, end, base);
	*overflow = value > INT32_MAX || value < INT32_MIN;
	if (value > INT32_MAX) {
		return INT32_MAX;
	}
	if (value < INT32_MIN) {
		return INT32_MIN;
	}
	return (LONG)value;
}

// atoi and atol do not set errno, so the host's ERANGE from strtoll is discarded here.
int CDECL msvcrt_atoi(LPCSTR str) {
	HOST_CONTEXT_GUARD();
	int savedErrno = errno;
	bool overflow;
	LONG value = strtolClamped(str, NULL, 10, &overflow);
	errno = savedErrno;
	return value;
}

LONG CDECL msvcrt_atol(LPCSTR str) {
	HOST_CONTEXT_GUARD();
	int savedErrno = errno;
	bool overflow;
	LONG value = strtolClamped(str, NULL, 10, &overflow);
	errno = savedErrno;
	return value;
}

LONG CDECL msvcrt_strtol(LPCSTR str, GUEST_PTR *end, int base) {
	HOST_CONTEXT_GUARD();
	char *localEnd = NULL;
	bool overflow;
	LONG value = strtolClamped(str, &localEnd, base, &overflow);
	if (overflow) {
		errno = ERANGE;
	}
	if (end) {
		*end = toGuestPtr(localEnd);
	}
	return value;
}

// Radix 10 writes a minus sign for negative values. Other radices use the bit pattern, as the MS CRT does.
static char *formatInteger(unsigned long long mag, bool negative, LPSTR buffer, int radix) {
	if (radix < 2 || radix > 36) {
		errno = EINVAL;
		return NULL;
	}
	char digits[66];
	int n = 0;
	do {
		int d = (int)(mag % radix);
		digits[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
		mag /= radix;
	} while (mag);
	char *out = buffer;
	if (negative) {
		*out++ = '-';
	}
	while (n) {
		*out++ = digits[--n];
	}
	*out = '\0';
	return buffer;
}

char *CDECL msvcrt__itoa(int value, LPSTR buffer, int radix) {
	HOST_CONTEXT_GUARD();
	if (value < 0 && radix == 10) {
		return formatInteger(-(long long)value, true, buffer, radix);
	}
	return formatInteger((unsigned int)value, false, buffer, radix);
}

char *CDECL msvcrt__ltoa(LONG value, LPSTR buffer, int radix) {
	HOST_CONTEXT_GUARD();
	if (value < 0 && radix == 10) {
		return formatInteger(-(long long)value, true, buffer, radix);
	}
	return formatInteger((uint32_t)value, false, buffer, radix);
}

LPSTR CDECL msvcrt_strerror(int errnum) {
	HOST_CONTEXT_GUARD();
#ifdef WIBO_GUEST64
	return strerror(errnum);
#else
	static __thread char *t_text;
	refillGuestSlot(&t_text, strerror(errnum));
	return t_text;
#endif
}

int CDECL msvcrt_tolower(int ch) {
	HOST_CONTEXT_GUARD();
	return tolower(ch);
}

// Matches GetCurrentProcessId, which also reports the host pid.
int CDECL msvcrt__getpid(void) {
	HOST_CONTEXT_GUARD();
	return (int)getpid();
}

// MS wchar_t is 16-bit, so the host wide-string routines (32-bit wchar_t)
// cannot be reused.
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

static LPWSTR duplicateGuestWideString(LPCWSTR str) {
	if (!str) {
		errno = EINVAL;
		return NULL;
	}
	SIZE_T n = 0;
	while (str[n]) {
		n++;
	}
	n++;
	WCHAR *copy = guestAlloc(n * sizeof(WCHAR));
	if (!copy) {
		errno = ENOMEM;
		return NULL;
	}
	memcpy(copy, str, n * sizeof(WCHAR));
	return copy;
}

LPWSTR CDECL msvcrt__wcsdup(LPCWSTR str) {
	HOST_CONTEXT_GUARD();
	return duplicateGuestWideString(str);
}

LPWSTR CDECL msvcrt_wcsdup(LPCWSTR str) {
	HOST_CONTEXT_GUARD();
	return duplicateGuestWideString(str);
}

static int wideCaseCompare(LPCWSTR a, LPCWSTR b) {
	for (;; a++, b++) {
		WCHAR ca = (WCHAR)towlower(*a);
		WCHAR cb = (WCHAR)towlower(*b);
		if (ca != cb || !ca) {
			return (int)ca - (int)cb;
		}
	}
}

int CDECL msvcrt__wcsicmp(LPCWSTR a, LPCWSTR b) {
	HOST_CONTEXT_GUARD();
	return wideCaseCompare(a, b);
}

int CDECL msvcrt_wcsicmp(LPCWSTR a, LPCWSTR b) {
	HOST_CONTEXT_GUARD();
	return wideCaseCompare(a, b);
}

void *CDECL msvcrt___iob_func(void) {
	HOST_CONTEXT_GUARD();
	return g_iob;
}

int *CDECL msvcrt___p__commode(void) {
	HOST_CONTEXT_GUARD();
	return &g_commode;
}

int *CDECL msvcrt___p__fmode(void) {
	HOST_CONTEXT_GUARD();
	return &g_fmode;
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

// Maps an MS fopen mode to a host mode. Text/binary, encoding, and commit flags
// have no effect on Linux.
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

// The MS offset and origin are 32-bit; the high bits of the 64-bit slot are not
// meaningful.
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

// exit runs the _onexit table, flushes host stdio, and exits without the MS
// atexit cleanup.
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

// MS CRT signal numbers differ from the host's, so the host numbering is never
// used.
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

// The default action (and the post-handler behaviour of abort) terminates with
// status 3, like the MS CRT.
_Noreturn static void terminateDefault(void) {
	fflush(NULL);
	kernel32_exitInternal(3);
}

// Handlers are reset to SIG_DFL before being invoked, as the MS CRT does, so a
// handler that re-raises terminates.
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

void *CDECL msvcrt__onexit(MSVCRT_PVFV fn) {
	HOST_CONTEXT_GUARD();
	if (g_onexitCount >= MSVCRT_ONEXIT_MAX) {
		return NULL;
	}
	g_onexit[g_onexitCount++] = fn;
	return (void *)fn;
}

int CDECL msvcrt_atexit(MSVCRT_PVFV fn) {
	HOST_CONTEXT_GUARD();
	if (g_onexitCount >= MSVCRT_ONEXIT_MAX) {
		return -1;
	}
	g_onexit[g_onexitCount++] = fn;
	return 0;
}

#ifndef WIBO_GUEST64
// One node per variable name seen by getenv. The value copy is replaced only
// when the host value changes, so an unchanged variable keeps its pointer.
typedef struct GuestEnvNode {
	struct GuestEnvNode *next;
	char *name;
	char *value;
} GuestEnvNode;

static GuestEnvNode *g_guestEnv;

// Caller holds g_guestStringMutex.
static char *guestEnvValue(const char *name) {
	const char *host = getenv(name);
	if (!host) {
		return NULL;
	}
	for (GuestEnvNode *n = g_guestEnv; n; n = n->next) {
		if (strcmp(n->name, name) == 0) {
			if (strcmp(n->value, host) != 0) {
				refillGuestSlot(&n->value, host);
			}
			return n->value;
		}
	}
	GuestEnvNode *node = calloc(1, sizeof *node);
	if (!node) {
		return NULL;
	}
	size_t nameLen = strlen(name) + 1;
	node->name = malloc(nameLen);
	node->value = guestStringCopy(host);
	if (!node->name || !node->value) {
		free(node->name);
		if (node->value) {
			guestFree(node->value);
		}
		free(node);
		return NULL;
	}
	memcpy(node->name, name, nameLen);
	node->next = g_guestEnv;
	g_guestEnv = node;
	return node->value;
}
#endif

LPSTR CDECL msvcrt_getenv(LPCSTR name) {
	HOST_CONTEXT_GUARD();
#ifdef WIBO_GUEST64
	return getenv(name);
#else
	pthread_mutex_lock(&g_guestStringMutex);
	char *value = guestEnvValue(name);
	pthread_mutex_unlock(&g_guestStringMutex);
	return value;
#endif
}

// Like the MS CRT, an empty value removes the variable. The host environment is
// the only store, so getenv and kernel32 environment calls see the change.
static int putenvImpl(const char *envstring) {
	const char *eq = envstring ? strchr(envstring, '=') : NULL;
	if (!eq || eq == envstring) {
		errno = EINVAL;
		return -1;
	}
	size_t nameLen = (size_t)(eq - envstring);
	char *name = malloc(nameLen + 1);
	if (!name) {
		errno = ENOMEM;
		return -1;
	}
	memcpy(name, envstring, nameLen);
	name[nameLen] = '\0';
	pthread_mutex_lock(&g_guestStringMutex);
	int rc = eq[1] ? setenv(name, eq + 1, 1) : unsetenv(name);
	pthread_mutex_unlock(&g_guestStringMutex);
	free(name);
	return rc == 0 ? 0 : -1;
}

int CDECL msvcrt__putenv(LPCSTR envstring) {
	HOST_CONTEXT_GUARD();
	return putenvImpl(envstring);
}

int CDECL msvcrt__wputenv(LPCWSTR envstring) {
	HOST_CONTEXT_GUARD();
	if (!envstring) {
		errno = EINVAL;
		return -1;
	}
	char *narrow = wideStringToString(envstring, -1);
	int rc = putenvImpl(narrow);
	free(narrow);
	return rc;
}

// Unlike _putenv, the name and value are separate arguments, so an empty value
// deletes the variable instead of being parsed out of "NAME=".
int CDECL msvcrt__putenv_s(LPCSTR name, LPCSTR value) {
	HOST_CONTEXT_GUARD();
	if (!name || !value || name[0] == '\0' || strchr(name, '=')) {
		errno = EINVAL;
		return EINVAL;
	}
	pthread_mutex_lock(&g_guestStringMutex);
	int rc = value[0] ? setenv(name, value, 1) : unsetenv(name);
	int err = rc == 0 ? 0 : errno;
	pthread_mutex_unlock(&g_guestStringMutex);
	if (err != 0) {
		errno = err;
	}
	return err;
}

// Returns the required size through retLen even when the buffer is too small,
// so callers can query first with a NULL buffer and len 0.
int CDECL msvcrt_getenv_s(SIZE_T *retLen, LPSTR buffer, SIZE_T len, LPCSTR name) {
	HOST_CONTEXT_GUARD();
	if (!retLen || !name || (buffer && len == 0) || (!buffer && len != 0)) {
		errno = EINVAL;
		return EINVAL;
	}
	*retLen = 0;
	if (buffer) {
		buffer[0] = '\0';
	}
	int rc = 0;
	pthread_mutex_lock(&g_guestStringMutex);
	const char *value = getenv(name);
	if (value) {
		SIZE_T need = strlen(value) + 1;
		*retLen = need;
		if (len >= need) {
			memcpy(buffer, value, need);
		} else if (len != 0) {
			rc = ERANGE;
		}
	}
	pthread_mutex_unlock(&g_guestStringMutex);
	if (rc != 0) {
		errno = rc;
	}
	return rc;
}

// Sizes and the buffer are in UTF-16 units. The value is converted the same
// way as the other wide strings in this file: one byte per unit.
int CDECL msvcrt__wgetenv_s(SIZE_T *retLen, LPWSTR buffer, SIZE_T len, LPCWSTR name) {
	HOST_CONTEXT_GUARD();
	if (!retLen || !name || (buffer && len == 0) || (!buffer && len != 0)) {
		errno = EINVAL;
		return EINVAL;
	}
	*retLen = 0;
	if (buffer) {
		buffer[0] = 0;
	}
	char *narrowName = wideStringToString(name, -1);
	int rc = 0;
	pthread_mutex_lock(&g_guestStringMutex);
	const char *value = getenv(narrowName);
	if (value) {
		uint16_t *wide = stringToWideString(value, SIZE_MAX);
		SIZE_T need = strlen(value) + 1;
		*retLen = need;
		if (len >= need) {
			memcpy(buffer, wide, need * sizeof(uint16_t));
		} else if (len != 0) {
			rc = ERANGE;
		}
		free(wide);
	}
	pthread_mutex_unlock(&g_guestStringMutex);
	free(narrowName);
	if (rc != 0) {
		errno = rc;
	}
	return rc;
}

// Like _putenv_s, an empty value deletes the variable. Names and values are
// converted one byte per unit, as in _wgetenv_s.
int CDECL msvcrt__wputenv_s(LPCWSTR name, LPCWSTR value) {
	HOST_CONTEXT_GUARD();
	if (!name || !value) {
		errno = EINVAL;
		return EINVAL;
	}
	char *narrowName = wideStringToString(name, -1);
	char *narrowValue = wideStringToString(value, -1);
	int err = 0;
	if (narrowName[0] == '\0' || strchr(narrowName, '=')) {
		err = EINVAL;
	} else {
		pthread_mutex_lock(&g_guestStringMutex);
		int rc = narrowValue[0] ? setenv(narrowName, narrowValue, 1) : unsetenv(narrowName);
		if (rc != 0) {
			err = errno;
		}
		pthread_mutex_unlock(&g_guestStringMutex);
	}
	free(narrowName);
	free(narrowValue);
	if (err != 0) {
		errno = err;
	}
	return err;
}

int CDECL msvcrt_setvbuf(void *stream, LPSTR buf, int mode, SIZE_T size) {
	HOST_CONTEXT_GUARD();
	FILE *f = hostStream(stream);
	if (!f) {
		errno = EINVAL;
		return -1;
	}
	int hostMode;
	switch (mode) {
	case MSVCRT_IOFBF:
		hostMode = _IOFBF;
		break;
	case MSVCRT_IOLBF:
		hostMode = _IOLBF;
		break;
	case MSVCRT_IONBF:
		hostMode = _IONBF;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	return setvbuf(f, buf, hostMode, size);
}

// The table holds guest function pointers in guest slots, so its entries are
// GUEST_PTR wide.
void CDECL msvcrt__initterm(GUEST_PTR *begin, GUEST_PTR *end) {
	HOST_CONTEXT_GUARD();
	for (GUEST_PTR *fn = begin; fn < end; fn++) {
		if (*fn) {
			call_MSVCRT_PVFV((MSVCRT_PVFV)fromGuestPtr(*fn));
		}
	}
}

// argv and envp are read by the guest, so on wibo32 the arrays and their
// strings are copied into the guest heap.
static GUEST_PTR guestStringArray(char **items) {
#ifdef WIBO_GUEST64
	return toGuestPtr(items);
#else
	size_t count = 0;
	while (items[count]) {
		count++;
	}
	GUEST_PTR *out = guestAlloc((count + 1) * sizeof *out);
	if (!out) {
		return GUEST_NULL;
	}
	for (size_t i = 0; i < count; i++) {
		out[i] = toGuestPtr(guestStringCopy(items[i]));
	}
	return toGuestPtr(out);
#endif
}

int CDECL msvcrt___getmainargs(int *argc, GUEST_PTR *argv, GUEST_PTR *envp, int doWildCard, void *startInfo) {
	HOST_CONTEXT_GUARD();
	(void)doWildCard;
	(void)startInfo;
	*argc = wibo_argc;
	*argv = guestStringArray(wibo_argv);
	*envp = guestStringArray(environ);
	g_initenv = *envp;
	return 0;
}

// The app type, locks, and the usermatherr handler are only consulted by
// MS-specific runtime code we do not provide.
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
#ifdef WIBO_GUEST64
	return &errno;
#else
	return errnoCell();
#endif
}

void *CDECL msvcrt_localeconv(void) {
	HOST_CONTEXT_GUARD();
#ifdef WIBO_GUEST64
	return localeconv();
#else
	// Per-thread, and refreshed on every call so that a later setlocale() is
	// reflected. The struct lives in guest memory because the guest reads it
	// through the returned pointer.
	static __thread MsLconv *t_guestLconv;
	static __thread char *t_strings[10];
	if (!t_guestLconv) {
		t_guestLconv = guestAlloc(sizeof *t_guestLconv);
		if (!t_guestLconv) {
			return NULL;
		}
	}
	const struct lconv *l = localeconv();
	const char *strings[] = {
		l->decimal_point,	  l->thousands_sep,		l->grouping,	 l->int_curr_symbol, l->currency_symbol,
		l->mon_decimal_point, l->mon_thousands_sep, l->mon_grouping, l->positive_sign,	 l->negative_sign,
	};
	for (size_t i = 0; i < sizeof strings / sizeof strings[0]; i++) {
		if (strings[i]) {
			refillGuestSlot(&t_strings[i], strings[i]);
		}
		t_guestLconv->strings[i] = strings[i] ? toGuestPtr(t_strings[i]) : 0;
	}
	const char chars[] = {
		l->int_frac_digits, l->frac_digits,	   l->p_cs_precedes, l->p_sep_by_space,
		l->n_cs_precedes,	l->n_sep_by_space, l->p_sign_posn,	 l->n_sign_posn,
	};
	memcpy(t_guestLconv->chars, chars, sizeof chars);
	return t_guestLconv;
#endif
}

// MSVC numbers its LC_* categories differently from glibc, so translate them.
static int msvcrtLcCategory(int category) {
	switch (category) {
	case 0:
		return LC_ALL;
	case 1:
		return LC_COLLATE;
	case 2:
		return LC_CTYPE;
	case 3:
		return LC_MONETARY;
	case 4:
		return LC_NUMERIC;
	case 5:
		return LC_TIME;
	default:
		return -1;
	}
}

LPSTR CDECL msvcrt_setlocale(int category, LPCSTR locale) {
	HOST_CONTEXT_GUARD();
	int hostCategory = msvcrtLcCategory(category);
	if (hostCategory < 0) {
		errno = EINVAL;
		return NULL;
	}
#ifdef WIBO_GUEST64
	return setlocale(hostCategory, locale);
#else
	static char *s_text;
	pthread_mutex_lock(&g_guestStringMutex);
	const char *result = setlocale(hostCategory, locale);
	LPSTR out = NULL;
	if (result) {
		refillGuestSlot(&s_text, result);
		out = s_text;
	}
	pthread_mutex_unlock(&g_guestStringMutex);
	return out;
#endif
}

// Handlers run only from raise() and abort(); there is no asynchronous signal
// delivery.
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
