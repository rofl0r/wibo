#pragma once

#include "common.h"
#include "kernel32/internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

typedef struct {
	size_t bytesTransferred;
	int unixError;
	bool reachedEnd;
} IOResult;

void files_init(void);
// Returns a malloc'd host path, or NULL on failure.
char *files_pathFromWindows(const char *inStr);
// Returns a malloc'd Windows path.
char *files_pathToWindows(const char *path);
// offset is NULL when the current file position should be used.
IOResult files_read(FileObject *file, void *buffer, size_t bytesToRead, const off_t *offset, bool updateFilePointer);
IOResult files_write(FileObject *file, const void *buffer, size_t bytesToWrite, const off_t *offset,
					 bool updateFilePointer);
HANDLE files_getStdHandle(DWORD nStdHandle);
BOOL files_setStdHandle(DWORD nStdHandle, HANDLE hHandle);
// Returns a malloc'd path, or NULL if no match exists.
char *files_findCaseInsensitiveFile(const char *directory, const char *filename);
// Returns a malloc'd path.
char *files_canonicalPath(const char *path);
// Returns malloc'd strings.
char *files_hostPathListToWindows(const char *value);
char *files_windowsPathListToHost(const char *value);

static inline bool endsWith(const char *str, const char *suffix) {
	size_t len = strlen(str);
	size_t suffixLen = strlen(suffix);
	return len >= suffixLen && memcmp(str + len - suffixLen, suffix, suffixLen) == 0;
}
