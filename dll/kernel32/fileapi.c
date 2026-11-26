#include "fileapi.h"

#include "access.h"
#include "async_io.h"
#include "common.h"
#include "context.h"
#include "errors.h"
#include "files.h"
#include "handleapi.h"
#include "handles.h"
#include "internal.h"
#include "namedpipeapi.h"
#include "overlapped_util.h"
#include "strutil.h"
#include "timeutil.h"
#include "types.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifndef __APPLE__
#include <sys/random.h>
#endif
#include <unistd.h>

static const uint64_t kWindowsTicksPerSecond = 10000000ULL;
static const uint64_t kSecondsBetween1601And1970 = 11644473600ULL;
static const FILETIME kDefaultFindFileTime = {
	(DWORD)((11644473600ULL * 10000000ULL) & 0xFFFFFFFFULL),
	(DWORD)((11644473600ULL * 10000000ULL) >> 32)};

static const FILETIME kDefaultFileInformationTime = {(DWORD)(UNIX_TIME_ZERO & 0xFFFFFFFFULL),
													 (DWORD)(UNIX_TIME_ZERO >> 32)};

static const uint32_t kFileReadMask = FILE_READ_DATA | FILE_READ_EA | FILE_READ_ATTRIBUTES;
static const uint32_t kDirectoryReadMask = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_EA | FILE_READ_ATTRIBUTES;
static const uint32_t kFileWriteMask = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
static const uint32_t kDirectoryWriteMask =
	FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;

static const size_t kNoFilePart = SIZE_MAX;

static struct timespec accessTimespec(const struct stat *st) {
#if defined(__APPLE__)
	return st->st_atimespec;
#elif defined(__linux__)
	return st->st_atim;
#else
	struct timespec ts = {0};
	ts.tv_sec = st->st_atime;
	ts.tv_nsec = 0;
	return ts;
#endif
}

static struct timespec modifyTimespec(const struct stat *st) {
#if defined(__APPLE__)
	return st->st_mtimespec;
#elif defined(__linux__)
	return st->st_mtim;
#else
	struct timespec ts = {0};
	ts.tv_sec = st->st_mtime;
	ts.tv_nsec = 0;
	return ts;
#endif
}

static struct timespec changeTimespec(const struct stat *st) {
#if defined(__APPLE__)
	return st->st_ctimespec;
#elif defined(__linux__)
	return st->st_ctim;
#else
	struct timespec ts = {0};
	ts.tv_sec = st->st_ctime;
	ts.tv_nsec = 0;
	return ts;
#endif
}

// Appends a backslash unless s already ends with one (an empty s also gets one).
static char *appendBackslashIfMissing(char *s) {
	size_t len = strlen(s);
	if (len > 0 && s[len - 1] == '\\') {
		return s;
	}
	s = xrealloc(s, len + 2);
	s[len] = '\\';
	s[len + 1] = '\0';
	return s;
}

// Returns a newly allocated Windows-style temp directory path ending in a backslash.
static char *getTempPath(void) {
	const char *tmpDirEnv = getenv("TMPDIR");
	char *pathStr = NULL;
	if (tmpDirEnv) {
		char *canonical = realpath(tmpDirEnv, NULL);
		if (canonical) {
			pathStr = files_pathToWindows(canonical);
			free(canonical);
		}
	}
	if (!pathStr) {
		pathStr = xstrdup("Z:\\tmp\\");
	}
	if (pathStr[0] != '\0') {
		pathStr = appendBackslashIfMissing(pathStr);
	}
	return pathStr;
}

typedef struct {
	char *path;
	size_t filePartOffset;
} FullPathInfo;

static bool isDriveLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

static size_t windowsRootLength(const char *path) {
	size_t len = strlen(path);
	if (len >= 3 && isDriveLetter(path[0]) && path[1] == ':' && path[2] == '\\') {
		return 3;
	}
	if (len >= 4 && path[0] == '\\' && path[1] == '\\' && (path[2] == '.' || path[2] == '?') && path[3] == '\\') {
		return 4;
	}
	if (len >= 2 && path[0] == '\\' && path[1] == '\\') {
		const char *server = strchr(path + 2, '\\');
		if (!server) {
			return len;
		}
		const char *share = strchr(server + 1, '\\');
		return share ? (size_t)(share - path) + 1 : len;
	}
	return 0;
}

static char *bufTake(Buf *b) {
	char *data = Buf_cstr(b);
	Buf_init(b);
	return data;
}

static bool computeFullPath(const char *input, FullPathInfo *outInfo) {
	if (input[strspn(input, " ")] == '\0') {
		return false;
	}

	char *path = xstrdup(input);
	for (char *p = path; *p; ++p) {
		if (*p == '/') {
			*p = '\\';
		}
	}

	if (windowsRootLength(path) == 0) {
		char *hostCwd = getcwd(NULL, 0);
		if (!hostCwd) {
			kernel32_setLastError(wibo_winErrorFromErrno(errno));
			free(path);
			return false;
		}
		char *cwd = files_pathToWindows(hostCwd);
		free(hostCwd);
		cwd = appendBackslashIfMissing(cwd);

		Buf next;
		Buf_init(&next);
		if (strlen(path) >= 2 && isDriveLetter(path[0]) && path[1] == ':') {
			bool sameDrive = strlen(cwd) >= 2 && cwd[1] == ':' &&
							 toupper((unsigned char)cwd[0]) == toupper((unsigned char)path[0]);
			if (sameDrive) {
				Buf_appendstr(&next, cwd);
			} else {
				Buf_append(&next, path, 2);
				Buf_appendstr(&next, "\\");
			}
			Buf_appendstr(&next, path + 2);
		} else if (path[0] == '\\') {
			size_t rootLen = windowsRootLength(cwd);
			if (rootLen > 0 && cwd[rootLen - 1] == '\\') {
				rootLen--;
			}
			Buf_append(&next, cwd, rootLen);
			Buf_appendstr(&next, path);
		} else {
			Buf_appendstr(&next, cwd);
			Buf_appendstr(&next, path);
		}
		free(cwd);
		free(path);
		path = bufTake(&next);
	}

	size_t rootLen = windowsRootLength(path);
	const size_t mark = rootLen;
	bool endsWithSeparator = false;
	StringVec components = {0};
	const size_t total = strlen(path);
	size_t pos = rootLen;
	while (pos <= total) {
		const char *sep = strchr(path + pos, '\\');
		const bool isLast = sep == NULL;
		const size_t start = pos;
		const size_t end = isLast ? total : (size_t)(sep - path);
		size_t len = end - start;
		pos = isLast ? total + 1 : end + 1;

		if (len == 0) {
			endsWithSeparator = isLast;
			continue;
		}
		if (len == 1 && path[start] == '.') {
			continue;
		}
		if (len == 2 && path[start] == '.' && path[start + 1] == '.') {
			if (components.len > 0) {
				free(vec_pop(&components));
			}
			continue;
		}
		if (!isLast && path[end - 1] == '.') {
			len--;
		}
		vec_push(&components, xstrndup(path + start, len));
	}

	Buf out;
	Buf_init(&out);
	Buf_append(&out, path, rootLen);
	for (size_t i = 0; i < components.len; ++i) {
		if (out.len > 0 && out.data[out.len - 1] != '\\') {
			Buf_appendc(&out, '\\');
		}
		Buf_appendstr(&out, components.data[i]);
		free(components.data[i]);
	}
	vec_free(&components);
	free(path);

	if (endsWithSeparator && out.len > mark) {
		Buf_appendc(&out, '\\');
	}
	while (out.len > mark && (out.data[out.len - 1] == '.' || out.data[out.len - 1] == ' ')) {
		out.len--;
	}

	char *result = bufTake(&out);
	const size_t resultLen = strlen(result);
	const char *lastSlash = strrchr(result, '\\');
	if (lastSlash && (size_t)(lastSlash - result) >= 2 && (size_t)(lastSlash - result) + 1 < resultLen) {
		outInfo->filePartOffset = (size_t)(lastSlash - result) + 1;
	} else {
		outInfo->filePartOffset = kNoFilePart;
	}
	outInfo->path = result;
	return true;
}

typedef struct {
	char *name;
	char *fullPath; // host path; NULL when unknown
	bool isDirectory;
} FindSearchEntry;

typedef VEC(FindSearchEntry) FindSearchEntryVec;

typedef struct {
	bool singleResult;
	FindSearchEntryVec entries;
	size_t nextIndex;
} FindSearchHandle;

static pthread_mutex_t g_findHandleMutex = PTHREAD_MUTEX_INITIALIZER;
static HANDLE g_nextFindHandle = 1;
static hashmap *g_findHandles = NULL;

static void freeFindSearchHandle(FindSearchHandle *handle) {
	for (size_t i = 0; i < handle->entries.len; ++i) {
		free(handle->entries.data[i].name);
		free(handle->entries.data[i].fullPath);
	}
	vec_free(&handle->entries);
	free(handle);
}

static HANDLE registerFindHandle(FindSearchHandle *handle) {
	if (!handle) {
		return INVALID_HANDLE_VALUE;
	}
	pthread_mutex_lock(&g_findHandleMutex);
	if (!g_findHandles) {
		g_findHandles = hashmap_create();
	}
	HANDLE raw = g_nextFindHandle++;
	hashmap_put(g_findHandles, &raw, sizeof(raw), handle);
	pthread_mutex_unlock(&g_findHandleMutex);
	return raw;
}

// Caller must hold g_findHandleMutex.
static FindSearchHandle *lookupFindHandleLocked(HANDLE handle) {
	if (handle == NO_HANDLE || !g_findHandles) {
		return NULL;
	}
	void *value = NULL;
	if (!hashmap_find(g_findHandles, &handle, sizeof(handle), &value)) {
		return NULL;
	}
	return (FindSearchHandle *)value;
}

// Removes the handle from the registry and transfers ownership to the caller.
static FindSearchHandle *detachFindHandle(HANDLE handle) {
	pthread_mutex_lock(&g_findHandleMutex);
	void *value = NULL;
	if (!g_findHandles || !hashmap_remove(g_findHandles, &handle, sizeof(handle), &value)) {
		value = NULL;
	}
	pthread_mutex_unlock(&g_findHandleMutex);
	return (FindSearchHandle *)value;
}

static bool containsWildcard(const char *value) { return strpbrk(value, "*?") != NULL; }

static bool containsWildcardOutsideExtendedPrefix(const char *value) {
	if (strncmp(value, "\\\\?\\", 4) == 0) {
		value += 4;
	}
	return containsWildcard(value);
}

static bool equalsIgnoreCase(char a, char b) {
	return tolower((unsigned char)a) == tolower((unsigned char)b);
}

static bool wildcardMatchInsensitive(const char *pattern, const char *text) {
	const size_t patternLen = strlen(pattern);
	const size_t textLen = strlen(text);
	size_t p = 0;
	size_t t = 0;
	size_t star = SIZE_MAX;
	size_t match = 0;

	while (t < textLen) {
		if (p < patternLen) {
			char pc = pattern[p];
			if (pc == '?') {
				++p;
				++t;
				continue;
			}
			if (pc == '*') {
				star = p++;
				match = t;
				continue;
			}
			if (equalsIgnoreCase(pc, text[t])) {
				++p;
				++t;
				continue;
			}
		}
		if (star != SIZE_MAX) {
			p = star + 1;
			t = ++match;
			continue;
		}
		return false;
	}

	while (p < patternLen && pattern[p] == '*') {
		++p;
	}
	return p == patternLen;
}

static void toFileTime(struct timespec ts, FILETIME *out) {
	int64_t seconds = (int64_t)ts.tv_sec + (int64_t)kSecondsBetween1601And1970;
	if (seconds < 0) {
		seconds = 0;
	}
	uint64_t ticks = (uint64_t)seconds * kWindowsTicksPerSecond;
	ticks += (uint64_t)(ts.tv_nsec > 0 ? ts.tv_nsec / 100 : 0);
	out->dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFULL);
	out->dwHighDateTime = (DWORD)(ticks >> 32);
}

static void assignFileNameA(WIN32_FIND_DATAA *data, const char *name) {
	size_t count = strlen(name);
	if (count > MAX_PATH - 1) {
		count = MAX_PATH - 1;
	}
	memcpy(data->cFileName, name, count);
	data->cFileName[count] = '\0';
}

static void assignFileNameW(WIN32_FIND_DATAW *data, const char *name) {
	uint16_t *wide = stringToWideString(name, strlen(name));
	size_t length = wstrlen(wide);
	if (length > MAX_PATH - 1) {
		length = MAX_PATH - 1;
	}
	wstrncpy(data->cFileName, wide, length);
	data->cFileName[length] = 0;
	free(wide);
}

static DWORD buildFileAttributes(const struct stat *st, bool isDirectory) {
	DWORD attributes = 0;
	mode_t mode = st->st_mode;
	if (S_ISDIR(mode) || isDirectory) {
		attributes |= FILE_ATTRIBUTE_DIRECTORY;
	}
	if (S_ISREG(mode) && !isDirectory) {
		attributes |= FILE_ATTRIBUTE_ARCHIVE;
	}
	if ((mode & S_IWUSR) == 0) {
		attributes |= FILE_ATTRIBUTE_READONLY;
	}
	if (attributes == 0) {
		attributes = FILE_ATTRIBUTE_NORMAL;
	}
	return attributes;
}

static void populateAttributeDataFromStat(const struct stat *st, bool isDirectory, WIN32_FILE_ATTRIBUTE_DATA *out) {
	out->dwFileAttributes = buildFileAttributes(st, isDirectory);
	toFileTime(changeTimespec(st), &out->ftCreationTime);
	toFileTime(accessTimespec(st), &out->ftLastAccessTime);
	toFileTime(modifyTimespec(st), &out->ftLastWriteTime);
	uint64_t fileSize = (isDirectory || !S_ISREG(st->st_mode)) ? 0ULL : (uint64_t)st->st_size;
	out->nFileSizeHigh = (DWORD)(fileSize >> 32);
	out->nFileSizeLow = (DWORD)(fileSize & 0xFFFFFFFFULL);
}

static void findEntryAttributes(const FindSearchEntry *entry, WIN32_FILE_ATTRIBUTE_DATA *out) {
	struct stat st;
	if (entry->fullPath && entry->fullPath[0] != '\0' && stat(entry->fullPath, &st) == 0) {
		populateAttributeDataFromStat(&st, entry->isDirectory, out);
		return;
	}
	memset(out, 0, sizeof(*out));
	out->dwFileAttributes = entry->isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	out->ftCreationTime = kDefaultFindFileTime;
	out->ftLastAccessTime = kDefaultFindFileTime;
	out->ftLastWriteTime = kDefaultFindFileTime;
}

static void populateFindDataA(const FindSearchEntry *entry, WIN32_FIND_DATAA *out) {
	WIN32_FILE_ATTRIBUTE_DATA attrs;
	findEntryAttributes(entry, &attrs);
	memset(out, 0, sizeof(*out));
	out->dwFileAttributes = attrs.dwFileAttributes;
	out->ftCreationTime = attrs.ftCreationTime;
	out->ftLastAccessTime = attrs.ftLastAccessTime;
	out->ftLastWriteTime = attrs.ftLastWriteTime;
	out->nFileSizeHigh = attrs.nFileSizeHigh;
	out->nFileSizeLow = attrs.nFileSizeLow;
	assignFileNameA(out, entry->name);
}

static void populateFindDataW(const FindSearchEntry *entry, WIN32_FIND_DATAW *out) {
	WIN32_FILE_ATTRIBUTE_DATA attrs;
	findEntryAttributes(entry, &attrs);
	memset(out, 0, sizeof(*out));
	out->dwFileAttributes = attrs.dwFileAttributes;
	out->ftCreationTime = attrs.ftCreationTime;
	out->ftLastAccessTime = attrs.ftLastAccessTime;
	out->ftLastWriteTime = attrs.ftLastWriteTime;
	out->nFileSizeHigh = attrs.nFileSizeHigh;
	out->nFileSizeLow = attrs.nFileSizeLow;
	assignFileNameW(out, entry->name);
}

// Lexical parent directory; returns "" when there is no parent.
static char *lexicalParent(const char *path) {
	const char *slash = strrchr(path, '/');
	if (!slash) {
		return xstrdup("");
	}
	if (slash == path) {
		return xstrdup("/");
	}
	return xstrndup(path, (size_t)(slash - path));
}

static const char *lexicalFilename(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

// Joins a directory and a file name with a single separator.
static char *joinPath(const char *dir, const char *name) {
	const size_t dirLen = strlen(dir);
	const size_t needSlash = (dirLen > 0 && dir[dirLen - 1] != '/') ? 1 : 0;
	const size_t nameLen = strlen(name);
	char *out = xmalloc(dirLen + needSlash + nameLen + 1);
	memcpy(out, dir, dirLen);
	size_t pos = dirLen;
	if (needSlash) {
		out[pos++] = '/';
	}
	memcpy(out + pos, name, nameLen + 1);
	return out;
}

static char *absolutePath(const char *path) {
	if (path[0] == '/') {
		return xstrdup(path);
	}
	char *cwd = getcwd(NULL, 0);
	if (!cwd) {
		return NULL;
	}
	char *out = joinPath(cwd, path);
	free(cwd);
	return out;
}

static char *parentOrSelf(const char *path) {
	char *parent = lexicalParent(path);
	if (parent[0] == '\0') {
		free(parent);
		return xstrdup(path);
	}
	return parent;
}

// Canonicalizes the existing prefix of path, keeping any non-existent trailing components (like weakly_canonical).
static char *resolvedPath(const char *path) {
	char *canonical = realpath(path, NULL);
	if (canonical) {
		return canonical;
	}
	char *absolute = absolutePath(path);
	if (!absolute) {
		return xstrdup(path);
	}
	char *parent = lexicalParent(absolute);
	const char *name = lexicalFilename(absolute);
	char *resolvedParent = resolvedPath(parent);
	char *result = joinPath(resolvedParent, name);
	free(parent);
	free(resolvedParent);
	free(absolute);
	return result;
}

static char *determineDisplayName(const char *path, const char *filePart) {
	const char *name = lexicalFilename(path);
	if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		char *absolute = absolutePath(path);
		if (absolute) {
			const char *absoluteName = lexicalFilename(absolute);
			if (absoluteName[0] != '\0') {
				char *owned = xstrdup(absoluteName);
				free(absolute);
				return owned;
			}
			free(absolute);
		}
	}
	if (name[0] == '\0') {
		return xstrdup(filePart);
	}
	return xstrdup(name);
}

static void appendFindEntry(FindSearchEntryVec *entries, const char *name, const char *path, bool isDirectory) {
	FindSearchEntry entry;
	entry.name = xstrdup(name);
	entry.fullPath = resolvedPath(path);
	entry.isDirectory = isDirectory;
	vec_push(entries, entry);
}

static bool collectDirectoryMatches(const char *directory, const char *pattern, FindSearchEntryVec *outEntries) {
	if (wildcardMatchInsensitive(pattern, ".")) {
		appendFindEntry(outEntries, ".", directory, true);
	}
	if (wildcardMatchInsensitive(pattern, "..")) {
		char *parent = parentOrSelf(directory);
		appendFindEntry(outEntries, "..", parent, true);
		free(parent);
	}

	DIR *dir = opendir(directory);
	if (!dir) {
		kernel32_setLastError(wibo_winErrorFromErrno(errno));
		return false;
	}
	struct dirent *de;
	int readErr = 0;
	while (true) {
		// stat() below may overwrite errno, so it is reset before every readdir().
		errno = 0;
		de = readdir(dir);
		if (!de) {
			readErr = errno;
			break;
		}
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
			continue;
		}
		if (!wildcardMatchInsensitive(pattern, de->d_name)) {
			continue;
		}
		char *childPath = joinPath(directory, de->d_name);
		struct stat st;
		bool isDir = stat(childPath, &st) == 0 && S_ISDIR(st.st_mode);
		appendFindEntry(outEntries, de->d_name, childPath, isDir);
		free(childPath);
	}
	closedir(dir);
	if (readErr != 0) {
		kernel32_setLastError(wibo_winErrorFromErrno(readErr));
		return false;
	}
	return true;
}

static int hostStat(const char *path, struct stat *st) { return stat(path, st) == 0 ? 0 : errno; }

// Resolves a FindFirstFile pattern. On success returns a handle with entries[0] as the first result; sets the
// last error and returns NULL otherwise.
static FindSearchHandle *resolveFindSearch(const char *rawInput) {
	if (rawInput[0] == '\0') {
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		return NULL;
	}

	char *input = xstrdup(rawInput);
	for (char *p = input; *p; ++p) {
		if (*p == '/') {
			*p = '\\';
		}
	}
	const size_t inputLen = strlen(input);

	if (input[inputLen - 1] == '\\') {
		kernel32_setLastError(ERROR_FILE_NOT_FOUND);
		free(input);
		return NULL;
	}

	char *directoryPart = NULL;
	char *filePart = NULL;
	const char *lastSlash = strrchr(input, '\\');
	if (!lastSlash) {
		directoryPart = xstrdup(".");
		filePart = xstrdup(input);
	} else {
		const size_t slashIndex = (size_t)(lastSlash - input);
		filePart = xstrdup(lastSlash + 1);
		if (slashIndex == 0) {
			directoryPart = xstrdup("\\");
		} else if (slashIndex == 2 && inputLen >= 3 && input[1] == ':') {
			directoryPart = xstrndup(input, slashIndex + 1);
		} else {
			directoryPart = xstrndup(input, slashIndex);
		}
	}

	if (filePart[0] == '\0') {
		kernel32_setLastError(ERROR_FILE_NOT_FOUND);
		goto fail;
	}

	if (containsWildcardOutsideExtendedPrefix(directoryPart)) {
		kernel32_setLastError(ERROR_INVALID_NAME);
		goto fail;
	}

	char *hostDirRaw = files_pathFromWindows(directoryPart);
	char *hostDirectory = resolvedPath(hostDirRaw);
	free(hostDirRaw);

	struct stat st;
	int err = hostStat(hostDirectory, &st);
	if (err != 0) {
		kernel32_setLastError(err == ENOENT ? ERROR_PATH_NOT_FOUND : wibo_winErrorFromErrno(err));
		free(hostDirectory);
		goto fail;
	}
	if (!S_ISDIR(st.st_mode)) {
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		free(hostDirectory);
		goto fail;
	}

	FindSearchHandle *state = xcalloc(1, sizeof(*state));
	if (!containsWildcard(filePart)) {
		char *hostTargetRaw = files_pathFromWindows(input);
		char *targetPath = resolvedPath(hostTargetRaw);
		free(hostTargetRaw);

		err = hostStat(targetPath, &st);
		if (err != 0) {
			kernel32_setLastError(err == ENOENT ? ERROR_FILE_NOT_FOUND : wibo_winErrorFromErrno(err));
			free(targetPath);
			free(hostDirectory);
			free(state);
			goto fail;
		}

		FindSearchEntry entry;
		entry.name = determineDisplayName(targetPath, filePart);
		entry.fullPath = targetPath;
		entry.isDirectory = S_ISDIR(st.st_mode);
		vec_push(&state->entries, entry);
		state->singleResult = true;
		state->nextIndex = 1;
	} else {
		if (!collectDirectoryMatches(hostDirectory, filePart, &state->entries)) {
			free(hostDirectory);
			freeFindSearchHandle(state);
			goto fail;
		}
		if (state->entries.len == 0) {
			kernel32_setLastError(ERROR_FILE_NOT_FOUND);
			free(hostDirectory);
			freeFindSearchHandle(state);
			goto fail;
		}
		state->nextIndex = 1;
	}

	free(hostDirectory);
	free(directoryPart);
	free(filePart);
	free(input);
	return state;

fail:
	free(directoryPart);
	free(filePart);
	free(input);
	return NULL;
}

static bool stdHandleForConsoleDevice(const char *name, DWORD desiredAccess, DWORD *outStdHandle) {
	char *lowered = stringToLower(name, strlen(name));
	bool found = true;
	if (strcmp(lowered, "conin$") == 0) {
		*outStdHandle = STD_INPUT_HANDLE;
	} else if (strcmp(lowered, "conout$") == 0) {
		*outStdHandle = STD_OUTPUT_HANDLE;
	} else if (strcmp(lowered, "conerr$") == 0) {
		*outStdHandle = STD_ERROR_HANDLE;
	} else if (strcmp(lowered, "con") == 0) {
		*outStdHandle = (desiredAccess & GENERIC_WRITE) != 0 ? STD_OUTPUT_HANDLE : STD_INPUT_HANDLE;
	} else {
		found = false;
	}
	free(lowered);
	return found;
}

static bool tryOpenConsoleDevice(DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition,
								 DWORD dwFlagsAndAttributes, HANDLE *outHandle, const char *originalName) {
	(void)dwShareMode;
	(void)dwCreationDisposition;
	(void)dwFlagsAndAttributes;
	DWORD stdHandleKind = 0;
	if (!stdHandleForConsoleDevice(originalName, dwDesiredAccess, &stdHandleKind)) {
		return false;
	}
	HANDLE baseHandle = files_getStdHandle(stdHandleKind);
	if (!Handles_duplicateTo(wibo_handles(), baseHandle, wibo_handles(), outHandle, dwDesiredAccess, false, 0)) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		*outHandle = INVALID_HANDLE_VALUE;
		return true;
	}
	return true;
}

DWORD WINAPI GetFileAttributesA(LPCSTR lpFileName) {
	HOST_CONTEXT_GUARD();
	if (!lpFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_FILE_ATTRIBUTES;
	}
	char *pathStr = files_pathFromWindows(lpFileName);
	DEBUG_LOG("GetFileAttributesA(%s) -> %s\n", lpFileName, pathStr);

	if (endsWith(pathStr, "/license.dat")) {
		DEBUG_LOG("MWCC license override\n");
		free(pathStr);
		return FILE_ATTRIBUTE_NORMAL;
	}

	struct stat st;
	int statErr = hostStat(pathStr, &st);
	free(pathStr);
	if (statErr != 0) {
		kernel32_setLastError(wibo_winErrorFromErrno(statErr));
		return INVALID_FILE_ATTRIBUTES;
	}

	if (S_ISREG(st.st_mode)) {
		DEBUG_LOG("File exists\n");
		return FILE_ATTRIBUTE_NORMAL;
	}
	if (S_ISDIR(st.st_mode)) {
		return FILE_ATTRIBUTE_DIRECTORY;
	}
	DEBUG_LOG("File does not exist\n");
	kernel32_setLastError(ERROR_FILE_NOT_FOUND);
	return INVALID_FILE_ATTRIBUTES;
}

DWORD WINAPI GetFileAttributesW(LPCWSTR lpFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileAttributesW -> ");
	if (!lpFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_FILE_ATTRIBUTES;
	}
	char *str = wideStringToString(lpFileName, -1);
	DWORD result = GetFileAttributesA(str);
	free(str);
	return result;
}

BOOL WINAPI GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileAttributesExA(%s, %u, %p)\n", lpFileName ? lpFileName : "(null)", (unsigned)fInfoLevelId,
			  lpFileInformation);
	if (!lpFileName) {
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}
	if (fInfoLevelId != GetFileExInfoStandard || !lpFileInformation) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	char *hostPathStr = files_pathFromWindows(lpFileName);

	if (endsWith(hostPathStr, "/license.dat")) {
		LPWIN32_FILE_ATTRIBUTE_DATA attributeData = (LPWIN32_FILE_ATTRIBUTE_DATA)lpFileInformation;
		memset(attributeData, 0, sizeof(*attributeData));
		attributeData->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
		attributeData->ftCreationTime = kDefaultFindFileTime;
		attributeData->ftLastAccessTime = kDefaultFindFileTime;
		attributeData->ftLastWriteTime = kDefaultFindFileTime;
		free(hostPathStr);
		return TRUE;
	}

	struct stat st;
	int statErr = hostStat(hostPathStr, &st);
	free(hostPathStr);
	if (statErr != 0) {
		kernel32_setLastError(wibo_winErrorFromErrno(statErr));
		return FALSE;
	}

	LPWIN32_FILE_ATTRIBUTE_DATA attributeData = (LPWIN32_FILE_ATTRIBUTE_DATA)lpFileInformation;
	bool isDirectory = S_ISDIR(st.st_mode);
	populateAttributeDataFromStat(&st, isDirectory, attributeData);
	return TRUE;
}

BOOL WINAPI GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileAttributesExW(%p, %u, %p)\n", lpFileName, (unsigned)fInfoLevelId, lpFileInformation);
	if (!lpFileName) {
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}
	char *fileName = wideStringToString(lpFileName, -1);
	BOOL result = GetFileAttributesExA(fileName, fInfoLevelId, lpFileInformation);
	free(fileName);
	return result;
}

UINT WINAPI GetDriveTypeA(LPCSTR lpRootPathName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetDriveTypeA(%s)\n", lpRootPathName ? lpRootPathName : "(null)");
	(void)lpRootPathName;
	return DRIVE_FIXED;
}

UINT WINAPI GetDriveTypeW(LPCWSTR lpRootPathName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetDriveTypeW(%p)\n", lpRootPathName);
	(void)lpRootPathName;
	return DRIVE_FIXED;
}

BOOL WINAPI GetVolumeInformationA(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
								  LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength,
								  LPDWORD lpFileSystemFlags, LPSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetVolumeInformationA(%s)\n", lpRootPathName ? lpRootPathName : "(null)");
	if (lpVolumeNameBuffer && nVolumeNameSize > 0) {
		lpVolumeNameBuffer[0] = '\0';
	}
	if (lpVolumeSerialNumber) {
		*lpVolumeSerialNumber = 0x12345678;
	}
	if (lpMaximumComponentLength) {
		*lpMaximumComponentLength = 255;
	}
	if (lpFileSystemFlags) {
		*lpFileSystemFlags = 0;
	}
	if (lpFileSystemNameBuffer) {
		if (nFileSystemNameSize > 0) {
			const char *fsName = "NTFS";
			size_t copyLen = strlen(fsName);
			if (copyLen > nFileSystemNameSize - 1) {
				copyLen = nFileSystemNameSize - 1;
			}
			memcpy(lpFileSystemNameBuffer, fsName, copyLen);
			lpFileSystemNameBuffer[copyLen] = '\0';
		}
	}
	return TRUE;
}

BOOL WINAPI GetVolumeInformationW(LPCWSTR lpRootPathName, LPWSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
								  LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength,
								  LPDWORD lpFileSystemFlags, LPWSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetVolumeInformationW(%p)\n", lpRootPathName);
	if (lpVolumeNameBuffer && nVolumeNameSize > 0) {
		lpVolumeNameBuffer[0] = 0;
	}
	if (lpVolumeSerialNumber) {
		*lpVolumeSerialNumber = 0x12345678;
	}
	if (lpMaximumComponentLength) {
		*lpMaximumComponentLength = 255;
	}
	if (lpFileSystemFlags) {
		*lpFileSystemFlags = 0;
	}
	if (lpFileSystemNameBuffer) {
		if (nFileSystemNameSize > 0) {
			uint16_t *fsWide = stringToWideString("NTFS", SIZE_MAX);
			size_t copyLen = wstrlen(fsWide);
			if (copyLen > nFileSystemNameSize - 1) {
				copyLen = nFileSystemNameSize - 1;
			}
			for (size_t i = 0; i < copyLen; ++i) {
				lpFileSystemNameBuffer[i] = fsWide[i];
			}
			lpFileSystemNameBuffer[copyLen] = 0;
			free(fsWide);
		}
	}
	return TRUE;
}

static int64_t fileTimeToInt64(const FILETIME *ft) {
	if (!ft) {
		return 0;
	}
	uint64_t combined = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
	return (int64_t)combined;
}

LONG WINAPI CompareFileTime(const FILETIME *lpFileTime1, const FILETIME *lpFileTime2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CompareFileTime(%p, %p)\n", lpFileTime1, lpFileTime2);
	int64_t value1 = fileTimeToInt64(lpFileTime1);
	int64_t value2 = fileTimeToInt64(lpFileTime2);
	if (value1 < value2) {
		return -1;
	}
	if (value1 > value2) {
		return 1;
	}
	return 0;
}

static BOOL writeFileToObject(FileObject *file, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
							  LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped) {
	if (lpOverlapped == NULL && lpNumberOfBytesWritten == NULL) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (lpNumberOfBytesWritten && (!file->overlapped || lpOverlapped == NULL)) {
		*lpNumberOfBytesWritten = 0;
	}

	if (file->overlapped && lpOverlapped == NULL) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	off_t offsetValue = 0;
	const off_t *offset = NULL;
	bool updateFilePointer = true;
	if (lpOverlapped != NULL) {
		offsetValue = (off_t)(((uint64_t)lpOverlapped->Offset) | ((uint64_t)lpOverlapped->OffsetHigh << 32));
		offset = &offsetValue;
		lpOverlapped->Internal = STATUS_PENDING;
		lpOverlapped->InternalHigh = 0;
		updateFilePointer = !file->overlapped;
		kernel32_detail_resetOverlappedEvent(lpOverlapped);
		if (file->overlapped) {
			if (nNumberOfBytesToWrite == 0) {
				kernel32_detail_signalOverlappedEvent(file, lpOverlapped, STATUS_SUCCESS, 0);
				if (lpNumberOfBytesWritten) {
					*lpNumberOfBytesWritten = 0;
				}
				return TRUE;
			}
			AsyncIOBackend *asyncIO = wibo_asyncIO();
			ObjectBase_ref(&file->base.base);
			if (asyncIO->queueWrite(asyncIO, file, lpOverlapped, lpBuffer, nNumberOfBytesToWrite, offset,
									file->isPipe)) {
				if (lpNumberOfBytesWritten) {
					*lpNumberOfBytesWritten = 0;
				}
				kernel32_setLastError(ERROR_IO_PENDING);
				return FALSE;
			}
		}
	}

	const IOResult io = files_write(file, lpBuffer, nNumberOfBytesToWrite, offset, updateFilePointer);
	NTSTATUS completionStatus = STATUS_SUCCESS;
	if (io.unixError != 0) {
		completionStatus = wibo_statusFromErrno(io.unixError);
		kernel32_setLastError(wibo_winErrorFromErrno(io.unixError));
	} else if (io.reachedEnd && io.bytesTransferred == 0) {
		completionStatus = STATUS_END_OF_FILE;
	}

	if (lpNumberOfBytesWritten && (!file->overlapped || lpOverlapped == NULL)) {
		*lpNumberOfBytesWritten = (DWORD)io.bytesTransferred;
	}

	kernel32_detail_signalOverlappedEvent(file, lpOverlapped, completionStatus, io.bytesTransferred);

	return io.unixError == 0;
}

BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten,
					  LPOVERLAPPED lpOverlapped) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WriteFile(%p, %p, %u, %p, %p)\n", hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten,
			  lpOverlapped);

	HandleMeta meta = {0};
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, &meta);
	if (!base || !FsObject_valid((FsObject *)base)) {
		if (base) {
			ObjectBase_unref(base);
		}
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	FileObject *file = (FileObject *)base;
#ifdef CHECK_ACCESS
	if ((meta.grantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA)) == 0) {
		ObjectBase_unref(base);
		kernel32_setLastError(ERROR_ACCESS_DENIED);
		DEBUG_LOG("!!! DENIED: 0x%x\n", meta.grantedAccess);
		return FALSE;
	}
#endif

	BOOL result = writeFileToObject(file, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
	ObjectBase_unref(base);
	return result;
}

BOOL WINAPI FlushFileBuffers(HANDLE hFile) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FlushFileBuffers(%p)\n", hFile);
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, NULL);
	if (!base) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	FileObject *file = (FileObject *)base;
	BOOL result = TRUE;
	if (!FsObject_valid(&file->base)) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		result = FALSE;
	} else if (fsync(file->base.fd) != 0) {
		kernel32_setLastErrorFromErrno();
		result = FALSE;
	}
	ObjectBase_unref(base);
	return result;
}

static BOOL readFileFromObject(FileObject *file, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
							   LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped) {
	if (lpOverlapped == NULL && lpNumberOfBytesRead == NULL) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (lpNumberOfBytesRead && (!file->overlapped || lpOverlapped == NULL)) {
		*lpNumberOfBytesRead = 0;
	}

	if (file->overlapped && lpOverlapped == NULL) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	off_t offsetValue = 0;
	const off_t *offset = NULL;
	bool updateFilePointer = true;
	if (lpOverlapped != NULL) {
		offsetValue = (off_t)(((uint64_t)lpOverlapped->Offset) | ((uint64_t)lpOverlapped->OffsetHigh << 32));
		offset = &offsetValue;
		lpOverlapped->Internal = STATUS_PENDING;
		lpOverlapped->InternalHigh = 0;
		updateFilePointer = !file->overlapped;
		kernel32_detail_resetOverlappedEvent(lpOverlapped);
		if (file->overlapped) {
			if (nNumberOfBytesToRead == 0) {
				kernel32_detail_signalOverlappedEvent(file, lpOverlapped, STATUS_SUCCESS, 0);
				if (lpNumberOfBytesRead) {
					*lpNumberOfBytesRead = 0;
				}
				return TRUE;
			}
			AsyncIOBackend *asyncIO = wibo_asyncIO();
			ObjectBase_ref(&file->base.base);
			if (asyncIO->queueRead(asyncIO, file, lpOverlapped, lpBuffer, nNumberOfBytesToRead, offset,
								   file->isPipe)) {
				if (lpNumberOfBytesRead) {
					*lpNumberOfBytesRead = 0;
				}
				kernel32_setLastError(ERROR_IO_PENDING);
				return FALSE;
			}
		}
	}

	const IOResult io = files_read(file, lpBuffer, nNumberOfBytesToRead, offset, updateFilePointer);
	NTSTATUS completionStatus = STATUS_SUCCESS;
	if (io.unixError != 0) {
		completionStatus = wibo_statusFromErrno(io.unixError);
		kernel32_setLastError(wibo_winErrorFromErrno(io.unixError));
	} else if (io.reachedEnd && io.bytesTransferred == 0) {
		if (file->isPipe) {
			completionStatus = STATUS_PIPE_BROKEN;
			kernel32_setLastError(ERROR_BROKEN_PIPE);
			kernel32_detail_signalOverlappedEvent(file, lpOverlapped, completionStatus, 0);
			DEBUG_LOG("-> ERROR_BROKEN_PIPE\n");
			return FALSE;
		}
		completionStatus = STATUS_END_OF_FILE;
	}

	if (lpNumberOfBytesRead && (!file->overlapped || lpOverlapped == NULL)) {
		*lpNumberOfBytesRead = (DWORD)io.bytesTransferred;
	}

	kernel32_detail_signalOverlappedEvent(file, lpOverlapped, completionStatus, io.bytesTransferred);

	DEBUG_LOG("-> %u bytes read, error %d\n", (unsigned)io.bytesTransferred,
			  io.unixError == 0 ? 0 : (int)kernel32_getLastError());
	return io.unixError == 0;
}

BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead,
					 LPOVERLAPPED lpOverlapped) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ReadFile(%p, %p, %u, %p, %p)\n", hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead,
			  lpOverlapped);

	HandleMeta meta = {0};
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, &meta);
	if (!base || !FsObject_valid((FsObject *)base)) {
		if (base) {
			ObjectBase_unref(base);
		}
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	FileObject *file = (FileObject *)base;
#ifdef CHECK_ACCESS
	if ((meta.grantedAccess & FILE_READ_DATA) == 0) {
		ObjectBase_unref(base);
		kernel32_setLastError(ERROR_ACCESS_DENIED);
		DEBUG_LOG("!!! DENIED: 0x%x\n", meta.grantedAccess);
		return FALSE;
	}
#endif

	BOOL result = readFileFromObject(file, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
	ObjectBase_unref(base);
	return result;
}

static HANDLE createFileOnHostPath(LPCSTR lpFileName, const char *hostPathStr, DWORD dwDesiredAccess, DWORD dwShareMode,
								   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
								   DWORD dwFlagsAndAttributes) {
	DEBUG_LOG("CreateFileA(filename=%s (%s), desiredAccess=0x%x, shareMode=%u, securityAttributes=%p, "
			  "creationDisposition=%u, flagsAndAttributes=%u)\n",
			  lpFileName, hostPathStr, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition,
			  dwFlagsAndAttributes);

	const DWORD kAttributeMask = 0x0000FFFFu;
	DWORD fileAttributes = dwFlagsAndAttributes & kAttributeMask;
	bool backupSemantics = (dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS) != 0;
	bool deleteOnClose = (dwFlagsAndAttributes & FILE_FLAG_DELETE_ON_CLOSE) != 0;
	bool overlapped = (dwFlagsAndAttributes & FILE_FLAG_OVERLAPPED) != 0;

	struct stat st = {0};
	bool pathExists = hostStat(hostPathStr, &st) == 0;
	bool isDirectory = pathExists && S_ISDIR(st.st_mode);

	if ((fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !isDirectory) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER (ENOTDIR)\n");
		return INVALID_HANDLE_VALUE;
	}
	if (isDirectory && (!backupSemantics || deleteOnClose /* not currently implemented for dir */)) {
		kernel32_setLastError(ERROR_ACCESS_DENIED);
		DEBUG_LOG("-> ERROR_ACCESS_DENIED (EISDIR)\n");
		return INVALID_HANDLE_VALUE;
	}

	// TODO: verify share mode against existing opens

	bool allowCreate = false;
	bool truncateExisting = false;
	bool existedBefore = pathExists;

	switch (dwCreationDisposition) {
	case CREATE_NEW:
		allowCreate = true;
		if (pathExists) {
			kernel32_setLastError(ERROR_FILE_EXISTS);
			DEBUG_LOG("-> ERROR_FILE_EXISTS (EEXIST)");
			return INVALID_HANDLE_VALUE;
		}
		break;
	case CREATE_ALWAYS:
		allowCreate = true;
		if (isDirectory) {
			kernel32_setLastError(ERROR_ACCESS_DENIED);
			DEBUG_LOG("-> ERROR_ACCESS_DENIED (EISDIR)");
			return INVALID_HANDLE_VALUE;
		}
		truncateExisting = pathExists;
		break;
	case OPEN_ALWAYS:
		if (!pathExists) {
			allowCreate = true;
		} else if (isDirectory) {
			kernel32_setLastError(ERROR_ACCESS_DENIED);
			DEBUG_LOG("-> ERROR_ACCESS_DENIED (EISDIR)");
			return INVALID_HANDLE_VALUE;
		}
		break;
	case OPEN_EXISTING:
		if (!pathExists) {
			kernel32_setLastError(ERROR_FILE_NOT_FOUND);
			DEBUG_LOG("-> ERROR_FILE_NOT_FOUND (ENOENT)");
			return INVALID_HANDLE_VALUE;
		}
		break;
	case TRUNCATE_EXISTING:
		if (!pathExists) {
			kernel32_setLastError(ERROR_FILE_NOT_FOUND);
			DEBUG_LOG("-> ERROR_FILE_NOT_FOUND (ENOENT)");
			return INVALID_HANDLE_VALUE;
		}
		if (isDirectory) {
			kernel32_setLastError(ERROR_ACCESS_DENIED);
			DEBUG_LOG("-> ERROR_ACCESS_DENIED (EISDIR)");
			return INVALID_HANDLE_VALUE;
		}
		truncateExisting = true;
		break;
	default:
		assert(false);
	}

	const GenericMapping *genericMapping =
		isDirectory ? &wibo_access_kDirectoryGenericMapping : &wibo_access_kFileGenericMapping;
	const uint32_t supportedMask =
		isDirectory ? (FILE_ALL_ACCESS | kDirectorySpecificRightsMask) : (FILE_ALL_ACCESS | kFileSpecificRightsMask);
	const uint32_t defaultMask = FILE_READ_ATTRIBUTES;
	NormalizedAccess normalized =
		wibo_access_normalizeDesiredAccess(dwDesiredAccess, genericMapping, supportedMask, SYNCHRONIZE, defaultMask);
	if (normalized.deniedMask != 0) {
		kernel32_setLastError(ERROR_ACCESS_DENIED);
		DEBUG_LOG("-> ERROR_ACCESS_DENIED: denied mask 0x%x\n", normalized.deniedMask);
		return INVALID_HANDLE_VALUE;
	}

	bool wantsRead = wibo_access_containsAny(normalized.grantedMask, isDirectory ? kDirectoryReadMask : kFileReadMask) ||
					 wibo_access_containsAny(normalized.grantedMask, FILE_EXECUTE);
	bool wantsWrite =
		wibo_access_containsAny(normalized.grantedMask, isDirectory ? kDirectoryWriteMask : kFileWriteMask);
	bool appendRequested = !isDirectory && wibo_access_containsAny(normalized.grantedMask, FILE_APPEND_DATA);
	bool appendOnly = appendRequested && !wibo_access_containsAny(normalized.grantedMask, FILE_WRITE_DATA);
#ifdef CHECK_ACCESS
	if (allowCreate && !wibo_access_containsAny(normalized.grantedMask, FILE_WRITE_DATA | FILE_APPEND_DATA)) {
		kernel32_setLastError(ERROR_ACCESS_DENIED);
		DEBUG_LOG("-> ERROR_ACCESS_DENIED: FILE_WRITE_DATA | FILE_APPEND_DATA required for creation");
		return INVALID_HANDLE_VALUE;
	}
	if (truncateExisting && !wibo_access_containsAny(normalized.grantedMask, FILE_WRITE_DATA)) {
		kernel32_setLastError(ERROR_ACCESS_DENIED);
		DEBUG_LOG("-> ERROR_ACCESS_DENIED: FILE_WRITE_DATA required for truncation");
		return INVALID_HANDLE_VALUE;
	}
	if (deleteOnClose && !wibo_access_containsAny(normalized.grantedMask, DELETE)) {
		kernel32_setLastError(ERROR_ACCESS_DENIED);
		DEBUG_LOG("-> ERROR_ACCESS_DENIED: DELETE required for delete-on-close");
		return INVALID_HANDLE_VALUE;
	}
#else
	(void)allowCreate;
#endif

	uint32_t shareMask = dwShareMode & (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);

	int openFlags = O_CLOEXEC;
	mode_t createMode = 0666;
	bool requestCreate = false;
	if (dwCreationDisposition == CREATE_NEW || dwCreationDisposition == CREATE_ALWAYS ||
		(dwCreationDisposition == OPEN_ALWAYS && !pathExists)) {
		requestCreate = true;
		openFlags |= O_CREAT;
	}
	if (dwCreationDisposition == CREATE_NEW) {
		openFlags |= O_EXCL;
	}
	if (truncateExisting && !isDirectory) {
		openFlags |= O_TRUNC;
	}

	if (isDirectory) {
		openFlags |= O_RDONLY | O_DIRECTORY;
		wantsRead = true;
	} else {
		bool needWrite = wantsWrite || truncateExisting || requestCreate;
		bool needRead = wantsRead || !needWrite;
		if (needWrite && needRead) {
			openFlags |= O_RDWR;
		} else if (needWrite) {
			openFlags |= O_WRONLY;
		} else {
			openFlags |= O_RDONLY;
		}
		if (appendOnly) {
			openFlags |= O_APPEND;
		}
	}

	int fd = open(hostPathStr, openFlags, createMode);
	if (fd < 0) {
		kernel32_setLastErrorFromErrno();
		DEBUG_LOG("-> errno: %d\n", errno);
		return INVALID_HANDLE_VALUE;
	}

	if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		isDirectory = true;
	}

	bool createdNew = !existedBefore && requestCreate;
	char *canonicalPath = files_canonicalPath(hostPathStr);

	ObjectBase *obj;
	FsObject *fsObject;
	if (isDirectory) {
		DirectoryObject *dirObject = DirectoryObject_alloc(fd);
		fsObject = &dirObject->base;
		obj = &dirObject->base.base;
	} else {
		FileObject *fileObject = FileObject_alloc(ObjectType_File, fd);
		fileObject->overlapped = overlapped;
		fileObject->appendOnly = appendOnly;
		fsObject = &fileObject->base;
		obj = &fileObject->base.base;
	}
	fsObject->canonicalPath = canonicalPath;
	fsObject->shareAccess = shareMask;
	fsObject->deletePending = deleteOnClose;

	uint32_t handleFlags = 0;
	if (lpSecurityAttributes && lpSecurityAttributes->bInheritHandle) {
		handleFlags |= HANDLE_FLAG_INHERIT;
	}
	HANDLE handle = Handles_alloc(wibo_handles(), obj, normalized.grantedMask, handleFlags);

	if ((dwCreationDisposition == OPEN_ALWAYS && existedBefore) ||
		(dwCreationDisposition == CREATE_ALWAYS && existedBefore)) {
		kernel32_setLastError(ERROR_ALREADY_EXISTS);
	}

	DEBUG_LOG("-> %p (createdNew=%d, truncate=%d)\n", handle, createdNew ? 1 : 0, truncateExisting ? 1 : 0);
	return handle;
}

HANDLE WINAPI CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
						  LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
						  DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
	HOST_CONTEXT_GUARD();
	(void)hTemplateFile;
	if (!lpFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}

	HANDLE consoleHandle = INVALID_HANDLE_VALUE;
	if (tryOpenConsoleDevice(dwDesiredAccess, dwShareMode, dwCreationDisposition, dwFlagsAndAttributes,
							 &consoleHandle, lpFileName)) {
		DEBUG_LOG("CreateFileA(console=%s, desiredAccess=0x%x, shareMode=%u, flags=0x%x) -> %p\n", lpFileName,
				  dwDesiredAccess, dwShareMode, dwFlagsAndAttributes, consoleHandle);
		return consoleHandle;
	}

	HANDLE pipeHandle = INVALID_HANDLE_VALUE;
	if (kernel32_tryCreateFileNamedPipeA(lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
										 dwCreationDisposition, dwFlagsAndAttributes, &pipeHandle)) {
		DEBUG_LOG("CreateFileA(pipe=%s) -> %p (err=%u)\n", lpFileName, pipeHandle, kernel32_getLastError());
		return pipeHandle;
	}

	char *hostPath = files_pathFromWindows(lpFileName);
	HANDLE handle = createFileOnHostPath(lpFileName, hostPath, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
										 dwCreationDisposition, dwFlagsAndAttributes);
	free(hostPath);
	return handle;
}

HANDLE WINAPI CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
						  LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
						  DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateFileW -> ");
	if (!lpFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	char *lpFileNameA = wideStringToString(lpFileName, -1);
	HANDLE handle = CreateFileA(lpFileNameA, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
								dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
	free(lpFileNameA);
	return handle;
}

BOOL WINAPI DeleteFileA(LPCSTR lpFileName) {
	HOST_CONTEXT_GUARD();
	if (!lpFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG("DeleteFileA(NULL) -> ERROR_INVALID_PARAMETER\n");
		return FALSE;
	}
	char *path = files_pathFromWindows(lpFileName);
	DEBUG_LOG("DeleteFileA(%s) -> %s\n", lpFileName, path);
	if (unlink(path) != 0) {
		kernel32_setLastErrorFromErrno();
		free(path);
		return FALSE;
	}
	free(path);
	return TRUE;
}

BOOL WINAPI DeleteFileW(LPCWSTR lpFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DeleteFileW -> ");
	if (!lpFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	char *name = wideStringToString(lpFileName, -1);
	BOOL result = DeleteFileA(name);
	free(name);
	return result;
}

static bool isDirectoryPath(const char *path) {
	struct stat st = {0};
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int writeAllToFd(int fd, const char *buf, size_t len) {
	while (len > 0) {
		ssize_t written = write(fd, buf, len);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return errno;
		}
		buf += written;
		len -= (size_t)written;
	}
	return 0;
}

// Returns 0 or an errno value. Used for cross-device moves, where rename() cannot be used.
static int copyFileContents(const char *from, const char *to, bool overwrite) {
	int srcFd = open(from, O_RDONLY | O_CLOEXEC);
	if (srcFd < 0) {
		return errno;
	}
	struct stat srcStat = {0};
	if (fstat(srcFd, &srcStat) != 0) {
		int err = errno;
		close(srcFd);
		return err;
	}
	int dstFlags = O_WRONLY | O_CREAT | O_CLOEXEC | (overwrite ? O_TRUNC : O_EXCL);
	int dstFd = open(to, dstFlags, srcStat.st_mode & 07777);
	if (dstFd < 0) {
		int err = errno;
		close(srcFd);
		return err;
	}
	char buf[65536];
	int err = 0;
	for (;;) {
		ssize_t n = read(srcFd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			err = errno;
			break;
		}
		if (n == 0) {
			break;
		}
		err = writeAllToFd(dstFd, buf, (size_t)n);
		if (err) {
			break;
		}
	}
	close(srcFd);
	if (close(dstFd) != 0 && err == 0) {
		err = errno;
	}
	return err;
}

BOOL WINAPI MoveFileExA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MoveFileExA(%s, %s, 0x%x)\n", lpExistingFileName ? lpExistingFileName : "(null)",
			  lpNewFileName ? lpNewFileName : "(null)", dwFlags);
	if (!lpExistingFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (dwFlags & MOVEFILE_DELAY_UNTIL_REBOOT) {
		DEBUG_LOG("-> MOVEFILE_DELAY_UNTIL_REBOOT ignored\n");
		return TRUE;
	}
	if (!lpNewFileName) {
		return DeleteFileA(lpExistingFileName);
	}
	char *fromPath = files_pathFromWindows(lpExistingFileName);
	char *toPath = files_pathFromWindows(lpNewFileName);
	DEBUG_LOG("-> %s -> %s\n", fromPath, toPath);

	struct stat fromLstat = {0};
	if (lstat(fromPath, &fromLstat) != 0) {
		char *parent = lexicalParent(fromPath);
		bool parentExists = parent[0] == '\0' || isDirectoryPath(parent);
		kernel32_setLastError(parentExists ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
		free(parent);
		free(fromPath);
		free(toPath);
		return FALSE;
	}

	struct stat toStat = {0};
	if (stat(toPath, &toStat) == 0) {
		// Case-insensitive path resolution can map the destination onto the source itself.
		struct stat fromStat = {0};
		if (stat(fromPath, &fromStat) == 0 && fromStat.st_dev == toStat.st_dev && fromStat.st_ino == toStat.st_ino) {
			DEBUG_LOG("-> same file, nothing to move\n");
			free(fromPath);
			free(toPath);
			return TRUE;
		}
		if (!(dwFlags & MOVEFILE_REPLACE_EXISTING)) {
			kernel32_setLastError(ERROR_ALREADY_EXISTS);
			free(fromPath);
			free(toPath);
			return FALSE;
		}
		if (S_ISDIR(toStat.st_mode)) {
			DEBUG_LOG("-> cannot replace a directory\n");
			kernel32_setLastError(ERROR_ACCESS_DENIED);
			free(fromPath);
			free(toPath);
			return FALSE;
		}
	}

	int moveErr = rename(fromPath, toPath) == 0 ? 0 : errno;
	if (moveErr == EXDEV && (dwFlags & MOVEFILE_COPY_ALLOWED)) {
		if (S_ISDIR(fromLstat.st_mode)) {
			DEBUG_LOG("-> EXDEV, cannot copy a directory\n");
			kernel32_setLastError(ERROR_ACCESS_DENIED);
			free(fromPath);
			free(toPath);
			return FALSE;
		}
		DEBUG_LOG("-> EXDEV, copying\n");
		struct stat fromStat = {0};
		bool haveStat = stat(fromPath, &fromStat) == 0;
		moveErr = copyFileContents(fromPath, toPath, (dwFlags & MOVEFILE_REPLACE_EXISTING) != 0);
		if (moveErr == 0) {
			if (haveStat) {
				const struct timespec times[2] = {accessTimespec(&fromStat), modifyTimespec(&fromStat)};
				if (utimensat(AT_FDCWD, toPath, times, 0) != 0) {
					DEBUG_LOG("-> failed to copy timestamps: %s\n", strerror(errno));
				}
			}
			if (unlink(fromPath) != 0) {
				DEBUG_LOG("-> source deletion failed after copy: %s\n", strerror(errno));
			}
		}
	}
	free(fromPath);
	free(toPath);
	if (moveErr) {
		kernel32_setLastError(moveErr == ENOENT ? ERROR_PATH_NOT_FOUND : wibo_winErrorFromErrno(moveErr));
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MoveFileExW -> ");
	if (!lpExistingFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	char *from = wideStringToString(lpExistingFileName, -1);
	char *to = lpNewFileName ? wideStringToString(lpNewFileName, -1) : NULL;
	BOOL result = MoveFileExA(from, to, dwFlags);
	free(from);
	free(to);
	return result;
}

BOOL WINAPI MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MoveFileA -> ");
	if (!lpExistingFileName || !lpNewFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return MoveFileExA(lpExistingFileName, lpNewFileName, MOVEFILE_COPY_ALLOWED);
}

BOOL WINAPI MoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MoveFileW -> ");
	if (!lpExistingFileName || !lpNewFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	char *from = wideStringToString(lpExistingFileName, -1);
	char *to = wideStringToString(lpNewFileName, -1);
	BOOL result = MoveFileExA(from, to, MOVEFILE_COPY_ALLOWED);
	free(from);
	free(to);
	return result;
}

// Locks the file, computes the new position, and commits it only on success. Returns -1 with errno set on failure.
static off_t seekFile(FileObject *file, off_t offset, DWORD dwMoveMethod) {
	pthread_mutex_lock(&file->base.m);
	off_t position = 0;
	if (dwMoveMethod == FILE_BEGIN) {
		position = offset;
	} else if (dwMoveMethod == FILE_CURRENT) {
		position = file->filePos + offset;
	} else if (dwMoveMethod == FILE_END) {
		position = lseek(file->base.fd, offset, SEEK_END);
	}
	if (position >= 0) {
		file->filePos = position;
	}
	pthread_mutex_unlock(&file->base.m);
	return position;
}

DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetFilePointer(%p, %ld, %p, %u)\n", hFile, (long)lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);
	if (hFile == NO_HANDLE) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return INVALID_SET_FILE_POINTER;
	}
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, NULL);
	if (!base) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return INVALID_SET_FILE_POINTER;
	}
	// TODO access check
	off_t position = seekFile((FileObject *)base, (off_t)lDistanceToMove, dwMoveMethod);
	int seekErr = errno;
	ObjectBase_unref(base);
	if (position < 0) {
		kernel32_setLastError(seekErr == EINVAL ? ERROR_NEGATIVE_SEEK : ERROR_INVALID_PARAMETER);
		return INVALID_SET_FILE_POINTER;
	}
	if (lpDistanceToMoveHigh) {
		*lpDistanceToMoveHigh = (LONG)((uint64_t)position >> 32);
	}
	return (DWORD)((uint64_t)position & 0xFFFFFFFFu);
}

BOOL WINAPI SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer,
							 DWORD dwMoveMethod) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetFilePointerEx(%p, %lld, %p, %u)\n", hFile, (long long)liDistanceToMove.QuadPart, lpNewFilePointer,
			  dwMoveMethod);
	if (hFile == NO_HANDLE) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, NULL);
	if (!base) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	// TODO access check
	off_t position = seekFile((FileObject *)base, (off_t)liDistanceToMove.QuadPart, dwMoveMethod);
	int seekErr = errno;
	ObjectBase_unref(base);
	if (position < 0) {
		kernel32_setLastError(seekErr == EINVAL ? ERROR_NEGATIVE_SEEK : ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (lpNewFilePointer) {
		lpNewFilePointer->QuadPart = position;
	}
	return TRUE;
}

BOOL WINAPI SetEndOfFile(HANDLE hFile) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetEndOfFile(%p)\n", hFile);
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, NULL);
	if (!base) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	// TODO access check
	FileObject *file = (FileObject *)base;
	pthread_mutex_lock(&file->base.m);
	BOOL result = TRUE;
	if (file->filePos < 0) {
		kernel32_setLastErrorFromErrno();
		result = FALSE;
	} else if (ftruncate(file->base.fd, file->filePos) != 0) {
		kernel32_setLastErrorFromErrno();
		result = FALSE;
	}
	pthread_mutex_unlock(&file->base.m);
	ObjectBase_unref(base);
	return result;
}

BOOL WINAPI CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
	HOST_CONTEXT_GUARD();
	(void)lpSecurityAttributes;
	if (!lpPathName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	char *path = files_pathFromWindows(lpPathName);
	DEBUG_LOG("CreateDirectoryA(%s, %p)\n", path, lpSecurityAttributes);
	BOOL result = TRUE;
	if (mkdir(path, 0755) != 0) {
		kernel32_setLastErrorFromErrno();
		result = FALSE;
	}
	free(path);
	return result;
}

BOOL WINAPI RemoveDirectoryA(LPCSTR lpPathName) {
	HOST_CONTEXT_GUARD();
	if (!lpPathName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	char *path = files_pathFromWindows(lpPathName);
	DEBUG_LOG("RemoveDirectoryA(%s)\n", path);
	BOOL result = TRUE;
	if (rmdir(path) != 0) {
		kernel32_setLastErrorFromErrno();
		result = FALSE;
	}
	free(path);
	return result;
}

BOOL WINAPI SetFileAttributesA(LPCSTR lpFileName, DWORD dwFileAttributes) {
	HOST_CONTEXT_GUARD();
	(void)dwFileAttributes;
	if (!lpFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	DEBUG_LOG("STUB: SetFileAttributesA(%s, %u)\n", lpFileName, dwFileAttributes);
	return TRUE;
}

DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileSize(%p, %p) ", hFile, lpFileSizeHigh);
	// TODO access check
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, NULL);
	if (!base || !FsObject_valid((FsObject *)base)) {
		if (base) {
			ObjectBase_unref(base);
		}
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		DEBUG_LOG("-> INVALID_FILE_SIZE (ERROR_INVALID_HANDLE)\n");
		return INVALID_FILE_SIZE;
	}
	off_t size = lseek(((FileObject *)base)->base.fd, 0, SEEK_END);
	ObjectBase_unref(base);
	if (size < 0) {
		if (lpFileSizeHigh) {
			*lpFileSizeHigh = 0;
		}
		DEBUG_LOG("-> INVALID_FILE_SIZE\n");
		return INVALID_FILE_SIZE;
	}
	DEBUG_LOG("-> %lld\n", (long long)size);
	uint64_t uSize = (uint64_t)size;
	if (lpFileSizeHigh) {
		*lpFileSizeHigh = (DWORD)(uSize >> 32);
	}
	return (DWORD)(uSize & 0xFFFFFFFFu);
}

static bool assignFileTime(LPFILETIME target, const struct timespec *spec) {
	if (!target) {
		return true;
	}
	FILETIME result = {0};
	uint32_t hundreds = (uint32_t)(spec->tv_nsec / 100L);
	if (!unixPartsToFileTime((int64_t)spec->tv_sec, hundreds, &result)) {
		return false;
	}
	*target = result;
	return true;
}

BOOL WINAPI GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime,
						LPFILETIME lpLastWriteTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileTime(%p, %p, %p, %p)\n", hFile, lpCreationTime, lpLastAccessTime, lpLastWriteTime);
	HandleMeta meta = {0};
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, &meta);
	if (!base || !FsObject_valid((FsObject *)base)) {
		if (base) {
			ObjectBase_unref(base);
		}
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	FileObject *file = (FileObject *)base;
#ifdef CHECK_ACCESS
	if ((meta.grantedAccess & FILE_READ_ATTRIBUTES) == 0) {
		ObjectBase_unref(base);
		kernel32_setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
#endif

	BOOL result = TRUE;
	struct stat st = {0};
	if (fstat(file->base.fd, &st) != 0) {
		kernel32_setLastErrorFromErrno();
		result = FALSE;
	} else {
		struct timespec creation = changeTimespec(&st);
		struct timespec access = accessTimespec(&st);
		struct timespec write = modifyTimespec(&st);
		if (!assignFileTime(lpCreationTime, &creation) || !assignFileTime(lpLastAccessTime, &access) ||
			!assignFileTime(lpLastWriteTime, &write)) {
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			result = FALSE;
		}
	}
	ObjectBase_unref(base);
	return result;
}

static BOOL setFileTimesOnObject(FileObject *file, const FILETIME *lpCreationTime, const FILETIME *lpLastAccessTime,
								 const FILETIME *lpLastWriteTime) {
	bool changeAccess = !shouldIgnoreFileTimeParam(lpLastAccessTime);
	bool changeWrite = !shouldIgnoreFileTimeParam(lpLastWriteTime);
	if (!changeAccess && !changeWrite) {
		return TRUE;
	}
	struct stat st = {0};
	if (fstat(file->base.fd, &st) != 0) {
		kernel32_setLastErrorFromErrno();
		return FALSE;
	}
	struct timespec accessSpec = accessTimespec(&st);
	struct timespec writeSpec = modifyTimespec(&st);
	if (changeAccess) {
		int64_t seconds = 0;
		uint32_t hundreds = 0;
		if (!fileTimeToUnixParts(*lpLastAccessTime, &seconds, &hundreds) ||
			!unixPartsToTimespec(seconds, hundreds, &accessSpec)) {
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
	}
	if (changeWrite) {
		int64_t seconds = 0;
		uint32_t hundreds = 0;
		if (!fileTimeToUnixParts(*lpLastWriteTime, &seconds, &hundreds) ||
			!unixPartsToTimespec(seconds, hundreds, &writeSpec)) {
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
	}
#if defined(__APPLE__) || defined(__FreeBSD__)
	struct timeval tv[2];
	tv[0].tv_sec = accessSpec.tv_sec;
	tv[0].tv_usec = accessSpec.tv_nsec / 1000L;
	tv[1].tv_sec = writeSpec.tv_sec;
	tv[1].tv_usec = writeSpec.tv_nsec / 1000L;
	if (futimes(file->base.fd, tv) != 0) {
		kernel32_setLastErrorFromErrno();
		return FALSE;
	}
#else
	struct timespec times[2] = {accessSpec, writeSpec};
	if (futimens(file->base.fd, times) != 0) {
		kernel32_setLastErrorFromErrno();
		return FALSE;
	}
#endif
	if (!shouldIgnoreFileTimeParam(lpCreationTime) && lpCreationTime) {
		DEBUG_LOG("SetFileTime: creation time not supported\n");
	}
	return TRUE;
}

BOOL WINAPI SetFileTime(HANDLE hFile, const FILETIME *lpCreationTime, const FILETIME *lpLastAccessTime,
						const FILETIME *lpLastWriteTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetFileTime(%p, %p, %p, %p)\n", hFile, lpCreationTime, lpLastAccessTime, lpLastWriteTime);
	HandleMeta meta = {0};
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, &meta);
	if (!base || !FsObject_valid((FsObject *)base)) {
		if (base) {
			ObjectBase_unref(base);
		}
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
#ifdef CHECK_ACCESS
	if ((meta.grantedAccess & FILE_WRITE_ATTRIBUTES) == 0) {
		ObjectBase_unref(base);
		kernel32_setLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
#endif
	BOOL result = setFileTimesOnObject((FileObject *)base, lpCreationTime, lpLastAccessTime, lpLastWriteTime);
	ObjectBase_unref(base);
	return result;
}

BOOL WINAPI GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileInformationByHandle(%p, %p)\n", hFile, lpFileInformation);
	if (!lpFileInformation) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, NULL);
	if (!base || !FsObject_valid((FsObject *)base)) {
		if (base) {
			ObjectBase_unref(base);
		}
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	// TODO access check
	FileObject *file = (FileObject *)base;
	struct stat st = {0};
	if (fstat(file->base.fd, &st) != 0) {
		kernel32_setLastErrorFromErrno();
		ObjectBase_unref(base);
		return FALSE;
	}
	ObjectBase_unref(base);
	lpFileInformation->dwFileAttributes = 0;
	if (S_ISDIR(st.st_mode)) {
		lpFileInformation->dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
	}
	if (S_ISREG(st.st_mode)) {
		lpFileInformation->dwFileAttributes |= FILE_ATTRIBUTE_NORMAL;
	}
	lpFileInformation->ftCreationTime = kDefaultFileInformationTime;
	lpFileInformation->ftLastAccessTime = kDefaultFileInformationTime;
	lpFileInformation->ftLastWriteTime = kDefaultFileInformationTime;
	lpFileInformation->dwVolumeSerialNumber = 0;
	lpFileInformation->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
	lpFileInformation->nFileSizeLow = (DWORD)((uint64_t)st.st_size & 0xFFFFFFFFULL);
	lpFileInformation->nNumberOfLinks = 1;
	lpFileInformation->nFileIndexHigh = 0;
	lpFileInformation->nFileIndexLow = 0;
	return TRUE;
}

DWORD WINAPI GetFileType(HANDLE hFile) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileType(%p) ", hFile);
	ObjectBase *base = Handles_getWithFlag(wibo_handles(), hFile, Of_File, NULL);
	if (!base || !FsObject_valid((FsObject *)base)) {
		if (base) {
			ObjectBase_unref(base);
		}
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		DEBUG_LOG("-> ERROR_INVALID_HANDLE\n");
		return FILE_TYPE_UNKNOWN;
	}
	FileObject *file = (FileObject *)base;
	struct stat st = {0};
	if (fstat(file->base.fd, &st) != 0) {
		kernel32_setLastErrorFromErrno();
		ObjectBase_unref(base);
		DEBUG_LOG("-> fstat error\n");
		return FILE_TYPE_UNKNOWN;
	}
	ObjectBase_unref(base);
	DWORD type = FILE_TYPE_UNKNOWN;
	if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) || S_ISBLK(st.st_mode)) {
		type = FILE_TYPE_DISK;
	}
	if (S_ISCHR(st.st_mode)) {
		type = FILE_TYPE_CHAR;
	}
	if (S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode)) {
		type = FILE_TYPE_PIPE;
	}
	DEBUG_LOG("-> %u\n", type);
	return type;
}

static char *absoluteWindowsPath(const char *windowsPath) {
	char *hostPath = files_pathFromWindows(windowsPath);
	char *absHost = absolutePath(hostPath);
	free(hostPath);
	if (!absHost) {
		return NULL;
	}
	char *out = files_pathToWindows(absHost);
	free(absHost);
	return out;
}

DWORD WINAPI GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, GUEST_PTR *lpFilePart) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFullPathNameA(%s, %u)\n", lpFileName ? lpFileName : "(null)", nBufferLength);

	if (!lpFileName || !lpFileName[0]) {
		return 0;
	}

	if (lpFilePart) {
		*lpFilePart = GUEST_NULL;
	}

	FullPathInfo info;
	if (!computeFullPath(lpFileName, &info)) {
		return 0;
	}

	DEBUG_LOG(" -> %s\n", info.path);

	const size_t pathLen = strlen(info.path);
	const DWORD required = (DWORD)(pathLen + 1);
	DWORD result = (DWORD)pathLen;

	if (nBufferLength == 0) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		result = required;
	} else if (!lpBuffer) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		result = 0;
	} else if (nBufferLength < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		result = required;
	} else {
		memcpy(lpBuffer, info.path, pathLen);
		lpBuffer[pathLen] = '\0';
		if (lpFilePart) {
			if (info.filePartOffset != kNoFilePart && info.filePartOffset < pathLen) {
				*lpFilePart = toGuestPtr(lpBuffer + info.filePartOffset);
			} else {
				*lpFilePart = GUEST_NULL;
			}
		}
	}

	free(info.path);
	return result;
}

DWORD WINAPI GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, GUEST_PTR *lpFilePart) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFullPathNameW(%p, %u)\n", lpFileName, nBufferLength);

	if (!lpFileName || !lpFileName[0]) {
		return 0;
	}

	if (lpFilePart) {
		*lpFilePart = GUEST_NULL;
	}

	char *narrow = wideStringToString(lpFileName, -1);
	FullPathInfo info;
	bool ok = computeFullPath(narrow, &info);
	free(narrow);
	if (!ok) {
		return 0;
	}

	DEBUG_LOG(" -> %s\n", info.path);

	uint16_t *widePath = stringToWideString(info.path, strlen(info.path));
	const DWORD wideLen = (DWORD)(wstrlen(widePath) + 1);
	const DWORD required = wideLen;
	DWORD result = wideLen - 1;

	if (nBufferLength == 0) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		result = required;
	} else if (!lpBuffer) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		result = 0;
	} else if (nBufferLength < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		result = required;
	} else {
		wstrncpy(lpBuffer, widePath, wideLen);
		if (lpFilePart) {
			if (info.filePartOffset != kNoFilePart && info.filePartOffset < strlen(info.path)) {
				*lpFilePart = toGuestPtr(lpBuffer + info.filePartOffset);
			} else {
				*lpFilePart = GUEST_NULL;
			}
		}
	}

	free(widePath);
	free(info.path);
	return result;
}

DWORD WINAPI GetShortPathNameA(LPCSTR lpszLongPath, LPSTR lpszShortPath, DWORD cchBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetShortPathNameA(%s)\n", lpszLongPath ? lpszLongPath : "(null)");
	if (!lpszLongPath || !lpszShortPath) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	char *absStr = absoluteWindowsPath(lpszLongPath);
	if (!absStr) {
		kernel32_setLastErrorFromErrno();
		return 0;
	}
	DWORD required = (DWORD)(strlen(absStr) + 1);
	DWORD result = required - 1;
	if (cchBuffer < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		result = required;
	} else {
		strcpy(lpszShortPath, absStr);
	}
	free(absStr);
	return result;
}

DWORD WINAPI GetShortPathNameW(LPCWSTR lpszLongPath, LPWSTR lpszShortPath, DWORD cchBuffer) {
	HOST_CONTEXT_GUARD();
	if (!lpszLongPath || !lpszShortPath) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *longPath = wideStringToString(lpszLongPath, -1);
	DEBUG_LOG("GetShortPathNameW(%s)\n", longPath);
	char *absStr = absoluteWindowsPath(longPath);
	free(longPath);
	if (!absStr) {
		kernel32_setLastErrorFromErrno();
		return 0;
	}
	uint16_t *absStrW = stringToWideString(absStr, strlen(absStr));
	free(absStr);
	size_t len = wstrlen(absStrW);
	DWORD required = (DWORD)(len + 1);
	DWORD result = (DWORD)len;
	if (cchBuffer < required) {
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		result = required;
	} else {
		wstrncpy(lpszShortPath, absStrW, len + 1);
	}
	free(absStrW);
	return result;
}

static char *makeTempFileName(const char *dir, const char *prefix, UINT unique) {
	char filename[12];
	snprintf(filename, sizeof(filename), "%.3s%04X.TMP", prefix, unique & 0xFFFF);
	size_t dirLen = strlen(dir);
	Buf path;
	Buf_init(&path);
	Buf_appendstr(&path, dir);
	if (dirLen > 0 && dir[dirLen - 1] != '\\' && dir[dirLen - 1] != '/') {
		Buf_appendc(&path, '\\');
	}
	Buf_appendstr(&path, filename);
	return bufTake(&path);
}

static UINT randomUniqueStart(void) {
	uint16_t value = 0;
#ifdef __APPLE__
	arc4random_buf(&value, sizeof(value));
#else
	if (getrandom(&value, sizeof(value), 0) != (ssize_t)sizeof(value)) {
		value = 0;
	}
#endif
	return value;
}

UINT WINAPI GetTempFileNameA(LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetTempFileNameA(%s, %s, %u)\n", lpPathName ? lpPathName : "(null)",
			  lpPrefixString ? lpPrefixString : "(null)", uUnique);
	if (!lpPathName || !lpTempFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (strlen(lpPathName) > MAX_PATH - 14) {
		kernel32_setLastError(ERROR_BUFFER_OVERFLOW);
		return 0;
	}

	// A null prefix is treated as an empty string, matching Windows/Wine.
	const char *prefix = lpPrefixString ? lpPrefixString : "";

	if (uUnique == 0) {
		UINT startUnique = randomUniqueStart();
		if (startUnique == 0) {
			startUnique = 1;
		}
		bool created = false;
		for (UINT attempt = 0; attempt < 0xFFFF; ++attempt) {
			uUnique = (startUnique + attempt) & 0xFFFF;
			if (uUnique == 0) {
				continue;
			}
			char *path = makeTempFileName(lpPathName, prefix, uUnique);
			HANDLE handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
										FILE_ATTRIBUTE_TEMPORARY, NO_HANDLE);
			if (handle != INVALID_HANDLE_VALUE) {
				CloseHandle(handle);
				strncpy(lpTempFileName, path, MAX_PATH);
				lpTempFileName[MAX_PATH - 1] = '\0';
				free(path);
				DEBUG_LOG(" -> %s\n", lpTempFileName);
				created = true;
				break;
			}
			free(path);
			DWORD error = kernel32_getLastError();
			if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
				DEBUG_LOG(" -> error %u\n", error);
				return 0;
			}
		}
		if (!created) {
			kernel32_setLastError(ERROR_FILE_EXISTS);
			DEBUG_LOG(" -> ERROR_FILE_EXISTS\n");
			return 0;
		}
	} else {
		char *path = makeTempFileName(lpPathName, prefix, uUnique);
		strncpy(lpTempFileName, path, MAX_PATH);
		lpTempFileName[MAX_PATH - 1] = '\0';
		free(path);
		DEBUG_LOG(" -> %s\n", lpTempFileName);
	}
	return uUnique & 0xFFFF;
}

UINT WINAPI GetTempFileNameW(LPCWSTR lpPathName, LPCWSTR lpPrefixString, UINT uUnique, LPWSTR lpTempFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetTempFileNameW -> ");
	if (!lpPathName || !lpTempFileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *pathName = wideStringToString(lpPathName, -1);
	// A null prefix is treated as an empty string, matching Windows/Wine.
	char *prefixString = lpPrefixString ? wideStringToString(lpPrefixString, -1) : xstrdup("");
	char tempFileName[MAX_PATH];
	UINT result = GetTempFileNameA(pathName, prefixString, uUnique, tempFileName);
	free(pathName);
	free(prefixString);
	if (result == 0) {
		return 0;
	}
	uint16_t *wide = stringToWideString(tempFileName, strlen(tempFileName));
	wstrncpy(lpTempFileName, wide, wstrlen(wide) + 1);
	free(wide);
	return result;
}

DWORD WINAPI GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetTempPathA(%u, %p)\n", nBufferLength, lpBuffer);

	if (nBufferLength != 0 && lpBuffer == NULL) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG(" -> ERROR_INVALID_PARAMETER\n");
		return 0;
	}

	char *pathStr = getTempPath();
	size_t len = strlen(pathStr);
	if (len + 1 > nBufferLength) {
		DEBUG_LOG(" -> %zu (required)\n", len + 1);
		free(pathStr);
		return (DWORD)(len + 1);
	}

	DEBUG_LOG(" -> %s\n", pathStr);
	strncpy(lpBuffer, pathStr, nBufferLength);
	lpBuffer[nBufferLength - 1] = '\0';
	free(pathStr);
	return (DWORD)len;
}

DWORD WINAPI GetTempPathW(DWORD nBufferLength, LPWSTR lpBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetTempPathW(%u, %p)\n", nBufferLength, lpBuffer);

	if (nBufferLength != 0 && lpBuffer == NULL) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG(" -> ERROR_INVALID_PARAMETER\n");
		return 0;
	}

	char *pathStr = getTempPath();
	size_t len = strlen(pathStr);
	if (len + 1 > nBufferLength) {
		DEBUG_LOG(" -> %zu (required)\n", len + 1);
		free(pathStr);
		return (DWORD)(len + 1);
	}

	DEBUG_LOG(" -> %s\n", pathStr);
	uint16_t *wide = stringToWideString(pathStr, len);
	wstrncpy(lpBuffer, wide, nBufferLength);
	lpBuffer[nBufferLength - 1] = 0;
	free(wide);
	free(pathStr);
	return (DWORD)len;
}

static HANDLE findFirstFileCommonA(const char *name, WIN32_FIND_DATAA *data) {
	FindSearchHandle *state = resolveFindSearch(name);
	if (!state) {
		return INVALID_HANDLE_VALUE;
	}
	populateFindDataA(&state->entries.data[0], data);
	kernel32_setLastError(ERROR_SUCCESS);
	return registerFindHandle(state);
}

static HANDLE findFirstFileCommonW(const char *name, WIN32_FIND_DATAW *data) {
	FindSearchHandle *state = resolveFindSearch(name);
	if (!state) {
		return INVALID_HANDLE_VALUE;
	}
	populateFindDataW(&state->entries.data[0], data);
	kernel32_setLastError(ERROR_SUCCESS);
	return registerFindHandle(state);
}

HANDLE WINAPI FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindFirstFileA(%s, %p)", lpFileName ? lpFileName : "(null)", lpFindFileData);
	if (!lpFindFileData) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG(" -> ERROR_INVALID_PARAMETER\n");
		return INVALID_HANDLE_VALUE;
	}
	if (!lpFileName) {
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		DEBUG_LOG(" -> ERROR_PATH_NOT_FOUND\n");
		return INVALID_HANDLE_VALUE;
	}

	HANDLE handle = findFirstFileCommonA(lpFileName, lpFindFileData);
	DEBUG_LOG(" -> %p\n", handle);
	return handle;
}

HANDLE WINAPI FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindFirstFileW(%p, %p)", lpFileName, lpFindFileData);
	if (!lpFindFileData) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG(" -> ERROR_INVALID_PARAMETER\n");
		return INVALID_HANDLE_VALUE;
	}
	if (!lpFileName) {
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		DEBUG_LOG(" -> ERROR_PATH_NOT_FOUND\n");
		return INVALID_HANDLE_VALUE;
	}

	char *narrowName = wideStringToString(lpFileName, -1);
	HANDLE handle = findFirstFileCommonW(narrowName, lpFindFileData);
	free(narrowName);
	DEBUG_LOG(" -> %p\n", handle);
	return handle;
}

HANDLE WINAPI FindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData,
							   FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindFirstFileExW(%p, %d, %p, %d, %p, 0x%x)\n", lpFileName, fInfoLevelId, lpFindFileData, fSearchOp,
			  lpSearchFilter, dwAdditionalFlags);
	if (!lpFindFileData || !lpFileName || fInfoLevelId != FindExInfoStandard || fSearchOp != FindExSearchNameMatch ||
		lpSearchFilter || dwAdditionalFlags != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	return FindFirstFileW(lpFileName, (LPWIN32_FIND_DATAW)lpFindFileData);
}

HANDLE WINAPI FindFirstFileExA(LPCSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData,
							   FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindFirstFileExA(%s, %d, %p, %d, %p, 0x%x)", lpFileName ? lpFileName : "(null)", fInfoLevelId,
			  lpFindFileData, fSearchOp, lpSearchFilter, dwAdditionalFlags);
	if (!lpFindFileData) {
		DEBUG_LOG(" -> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if (!lpFileName) {
		DEBUG_LOG(" -> ERROR_PATH_NOT_FOUND\n");
		kernel32_setLastError(ERROR_PATH_NOT_FOUND);
		return INVALID_HANDLE_VALUE;
	}
	if (fInfoLevelId != FindExInfoStandard) {
		DEBUG_LOG(" -> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if (fSearchOp != FindExSearchNameMatch) {
		DEBUG_LOG(" -> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if (lpSearchFilter) {
		DEBUG_LOG(" -> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if (dwAdditionalFlags != 0) {
		DEBUG_LOG(" -> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}

	return findFirstFileCommonA(lpFileName, (LPWIN32_FIND_DATAA)lpFindFileData);
}

BOOL WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindNextFileA(%p, %p)\n", hFindFile, lpFindFileData);
	if (!lpFindFileData) {
		DEBUG_LOG(" -> ERROR_INVALID_PARAMETER\n");
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	pthread_mutex_lock(&g_findHandleMutex);
	FindSearchHandle *state = lookupFindHandleLocked(hFindFile);
	if (!state) {
		pthread_mutex_unlock(&g_findHandleMutex);
		DEBUG_LOG(" -> ERROR_INVALID_HANDLE\n");
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (state->singleResult || state->nextIndex >= state->entries.len) {
		pthread_mutex_unlock(&g_findHandleMutex);
		DEBUG_LOG(" -> ERROR_NO_MORE_FILES\n");
		kernel32_setLastError(ERROR_NO_MORE_FILES);
		return FALSE;
	}
	populateFindDataA(&state->entries.data[state->nextIndex++], lpFindFileData);
	pthread_mutex_unlock(&g_findHandleMutex);
	return TRUE;
}

BOOL WINAPI FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindNextFileW(%p, %p)\n", hFindFile, lpFindFileData);
	if (!lpFindFileData) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	pthread_mutex_lock(&g_findHandleMutex);
	FindSearchHandle *state = lookupFindHandleLocked(hFindFile);
	if (!state) {
		pthread_mutex_unlock(&g_findHandleMutex);
		DEBUG_LOG(" -> ERROR_INVALID_HANDLE\n");
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (state->singleResult || state->nextIndex >= state->entries.len) {
		pthread_mutex_unlock(&g_findHandleMutex);
		DEBUG_LOG(" -> ERROR_NO_MORE_FILES\n");
		kernel32_setLastError(ERROR_NO_MORE_FILES);
		return FALSE;
	}
	populateFindDataW(&state->entries.data[state->nextIndex++], lpFindFileData);
	pthread_mutex_unlock(&g_findHandleMutex);
	return TRUE;
}

BOOL WINAPI FindClose(HANDLE hFindFile) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindClose(%p)\n", hFindFile);
	if (hFindFile == NO_HANDLE) {
		DEBUG_LOG(" -> ERROR_INVALID_HANDLE\n");
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	FindSearchHandle *owned = detachFindHandle(hFindFile);
	if (!owned) {
		DEBUG_LOG(" -> ERROR_INVALID_HANDLE\n");
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	freeFindSearchHandle(owned);
	return TRUE;
}
