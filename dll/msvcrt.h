#pragma once

#include "types.h"

// Guest-ABI callbacks; gentramp emits call_<typedef> helpers for these.
typedef void(_CC_CDECL *MSVCRT_PVFV)(void);
typedef int(_CC_CDECL *MSVCRT_COMPARE_FN)(const void *, const void *);
typedef void(_CC_CDECL *MSVCRT_SIGHANDLER)(int);

// Msvcrt subset shared by wibo64 (PE32+ guests) and wibo32 (PE32 guests).
// Export names are these identifiers with the "msvcrt_" prefix stripped. FILE*
// arguments are pointers into the MS-layout iob array returned by __iob_func.
// Guest pointers and slots are GUEST_PTR wide (8 bytes on wibo64, 4 bytes on
// wibo32). Variadic arguments are read from one slot per guest argument;
// floating-point values are read from them as raw bits.
void *CDECL msvcrt_malloc(SIZE_T size);
void *CDECL msvcrt_calloc(SIZE_T count, SIZE_T size);
void CDECL msvcrt_free(void *ptr);
void *CDECL msvcrt_memcpy(void *dest, const void *src, SIZE_T count);
void *CDECL msvcrt_memset(void *dest, int ch, SIZE_T count);
int CDECL msvcrt_memcmp(const void *buf1, const void *buf2, SIZE_T count);
void CDECL msvcrt_qsort(void *base, SIZE_T num, SIZE_T width, MSVCRT_COMPARE_FN compare);

SIZE_T CDECL msvcrt_strlen(LPCSTR str);
int CDECL msvcrt_strcmp(LPCSTR a, LPCSTR b);
int CDECL msvcrt_strncmp(LPCSTR a, LPCSTR b, SIZE_T count);
LPSTR CDECL msvcrt_strcpy(LPSTR dest, LPCSTR src);
LPSTR CDECL msvcrt_strncpy(LPSTR dest, LPCSTR src, SIZE_T count);
LPSTR CDECL msvcrt_strcat(LPSTR dest, LPCSTR src);
LPSTR CDECL msvcrt_strchr(LPCSTR str, int ch);
LPSTR CDECL msvcrt_strrchr(LPCSTR str, int ch);
LPSTR CDECL msvcrt_strerror(int errnum);
int CDECL msvcrt_tolower(int ch);
SIZE_T CDECL msvcrt_wcslen(LPCWSTR str);
int CDECL msvcrt_wcscmp(LPCWSTR a, LPCWSTR b);
int CDECL msvcrt_wcsncmp(LPCWSTR a, LPCWSTR b, SIZE_T count);
LPWSTR CDECL msvcrt_wcsrchr(LPCWSTR str, WCHAR ch);

void *CDECL msvcrt___iob_func(void);
int *CDECL msvcrt___p__commode(void);
int *CDECL msvcrt___p__fmode(void);
int CDECL msvcrt_fprintf(void *stream, LPCSTR format, ...);
int CDECL msvcrt_vfprintf(void *stream, LPCSTR format, void *ap);
int CDECL msvcrt_printf(LPCSTR format, ...);
int CDECL msvcrt_fputc(int ch, void *stream);
SIZE_T CDECL msvcrt_fwrite(const void *buf, SIZE_T size, SIZE_T count, void *stream);
int CDECL msvcrt_puts(LPCSTR str);

void CDECL msvcrt_exit(int code);
void CDECL msvcrt__cexit(void);
void CDECL msvcrt__amsg_exit(int code);
void CDECL msvcrt_abort(void);
void *CDECL msvcrt__onexit(MSVCRT_PVFV fn);
int CDECL msvcrt_atexit(MSVCRT_PVFV fn);
void CDECL msvcrt__initterm(GUEST_PTR *begin, GUEST_PTR *end);
int CDECL msvcrt___getmainargs(int *argc, GUEST_PTR *argv, GUEST_PTR *envp, int doWildCard, void *startInfo);
void CDECL msvcrt___set_app_type(int type);
void CDECL msvcrt__lock(int locknum);
void CDECL msvcrt__unlock(int locknum);
void CDECL msvcrt___setusermatherr(void *handler);
int CDECL msvcrt___C_specific_handler(void);
int CDECL msvcrt____mb_cur_max_func(void);
unsigned int CDECL msvcrt____lc_codepage_func(void);
int *CDECL msvcrt__errno(void);
void *CDECL msvcrt_localeconv(void);
LPSTR CDECL msvcrt_setlocale(int category, LPCSTR locale);
void *CDECL msvcrt_signal(int sig, void *handler);
int CDECL msvcrt_raise(int sig);
LPSTR CDECL msvcrt_getenv(LPCSTR name);
int CDECL msvcrt__putenv(LPCSTR envstring);
int CDECL msvcrt_getenv_s(SIZE_T *retLen, LPSTR buffer, SIZE_T len, LPCSTR name);
void *CDECL msvcrt_fopen(LPCSTR filename, LPCSTR mode);
int CDECL msvcrt_setvbuf(void *stream, LPSTR buf, int mode, SIZE_T size);
int CDECL msvcrt_fclose(void *stream);
int CDECL msvcrt_fflush(void *stream);
SIZE_T CDECL msvcrt_fread(void *buf, SIZE_T size, SIZE_T count, void *stream);
LPSTR CDECL msvcrt_fgets(LPSTR buf, int n, void *stream);
int CDECL msvcrt_fputs(LPCSTR str, void *stream);
int CDECL msvcrt_fgetc(void *stream);
int CDECL msvcrt_getc(void *stream);
int CDECL msvcrt_ungetc(int ch, void *stream);
int CDECL msvcrt_fseek(void *stream, LONG offset, int origin);
LONG CDECL msvcrt_ftell(void *stream);
void CDECL msvcrt_rewind(void *stream);
int CDECL msvcrt_feof(void *stream);
int CDECL msvcrt_ferror(void *stream);
void CDECL msvcrt_clearerr(void *stream);
