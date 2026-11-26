#include "files.h"
#include "common.h"
#include "cutil.h"
#include "errors.h"
#include "handles.h"
#include "strutil.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static HANDLE stdinHandle;
static HANDLE stdoutHandle;
static HANDLE stderrHandle;

static void replaceChar(char *s, char from, char to) {
	for (; *s; s++) {
		if (*s == from) {
			*s = to;
		}
	}
}

static char *bufTake(Buf *b) {
	char *s = xstrdup(Buf_cstr(b));
	Buf_free(b);
	return s;
}

static void freeStrings(StringVec *v) {
	for (size_t i = 0; i < v->len; i++) {
		free(v->data[i]);
	}
	vec_free(v);
}

static bool pathExists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

// Joins like std::filesystem::path::operator/: an absolute name replaces the directory.
static char *joinPath(const char *dir, const char *name) {
	if (name[0] == '/') {
		return xstrdup(name);
	}
	size_t dirLen = strlen(dir);
	bool needSep = dirLen > 0 && dir[dirLen - 1] != '/';
	Buf out = {0};
	Buf_appendstr(&out, dir);
	if (needSep) {
		Buf_appendc(&out, '/');
	}
	Buf_appendstr(&out, name);
	return bufTake(&out);
}

// Mirrors std::filesystem::path::lexically_normal for '/'-separated paths.
static char *lexicallyNormal(const char *path) {
	size_t len = strlen(path);
	if (len == 0) {
		return xstrdup("");
	}
	bool absolute = path[0] == '/';
	StringVec parts = {0};
	size_t i = 0;
	while (i < len) {
		while (i < len && path[i] == '/') {
			i++;
		}
		if (i >= len) {
			break;
		}
		size_t start = i;
		while (i < len && path[i] != '/') {
			i++;
		}
		size_t n = i - start;
		if (n == 1 && path[start] == '.') {
			continue;
		}
		if (n == 2 && path[start] == '.' && path[start + 1] == '.') {
			if (parts.len > 0 && strcmp(parts.data[parts.len - 1], "..") != 0) {
				free(vec_pop(&parts));
				continue;
			}
			if (absolute) {
				continue;
			}
		}
		vec_push(&parts, xstrndup(path + start, n));
	}

	Buf out = {0};
	if (absolute) {
		Buf_appendc(&out, '/');
	}
	for (size_t j = 0; j < parts.len; j++) {
		if (j > 0) {
			Buf_appendc(&out, '/');
		}
		Buf_appendstr(&out, parts.data[j]);
	}
	bool endsInDot = len >= 1 && path[len - 1] == '.' && (len == 1 || path[len - 2] == '/');
	if (parts.len > 0 && ((len > 0 && path[len - 1] == '/') || endsInDot)) {
		Buf_appendc(&out, '/');
	}
	if (out.len == 0) {
		Buf_appendc(&out, '.');
	}
	freeStrings(&parts);
	return bufTake(&out);
}

// Strip the Windows trailing-dot "no extension" convention from each path
// component. Windows treats "foo." and "foo" as the same filename — the
// trailing dot means "no extension" — and many NT-era tools rely on that
// equivalence (notably NMAKE's @<< temp response files like "nm12345."
// and older makefile directives like "!INCLUDE .\sources."). Linux
// filesystems rarely carry a literal trailing-dot name, so normalize
// before our lookup / case-insensitive fallback runs.
static char *stripTrailingDots(const char *s) {
	size_t n = strlen(s);
	Buf out = {0};
	size_t i = 0;
	while (i < n) {
		size_t start = i;
		while (i < n && s[i] != '/') {
			i++;
		}
		size_t end = i;
		size_t len = end - start;
		// Leave "." and ".." untouched.
		bool isDotDir = (len == 1 && s[start] == '.') || (len == 2 && s[start] == '.' && s[start + 1] == '.');
		if (!isDotDir) {
			while (end > start && s[end - 1] == '.') {
				end--;
			}
		}
		Buf_append(&out, s + start, end - start);
		if (i < n) {
			Buf_appendc(&out, '/');
			i++;
		}
	}
	return bufTake(&out);
}

// Equivalent to std::filesystem::weakly_canonical: resolves the longest existing prefix and appends the rest.
static char *weaklyCanonical(const char *path) {
	char *normal = lexicallyNormal(path);
	size_t cut = strlen(normal);
	while (cut > 0) {
		char *prefix = xstrndup(normal, cut);
		char *resolved = realpath(prefix, NULL);
		free(prefix);
		if (resolved) {
			const char *tail = normal + cut;
			if (*tail == '/') {
				tail++;
			}
			Buf out = {0};
			Buf_appendstr(&out, resolved);
			free(resolved);
			if (*tail) {
				if (out.len == 0 || out.data[out.len - 1] != '/') {
					Buf_appendc(&out, '/');
				}
				Buf_appendstr(&out, tail);
			}
			free(normal);
			return bufTake(&out);
		}
		size_t k = cut;
		while (k > 0 && normal[k - 1] != '/') {
			k--;
		}
		if (k == 0) {
			break;
		}
		size_t slash = k - 1;
		if (slash == 0) {
			if (cut == 1) {
				break;
			}
			cut = 1;
		} else {
			cut = slash;
		}
	}
	if (normal[0] == '/') {
		return normal;
	}
	// Anchor unresolved relative paths to the cwd so the result survives a later chdir.
	char *cwd = getcwd(NULL, 0);
	if (!cwd) {
		return normal;
	}
	Buf out = {0};
	Buf_appendstr(&out, cwd);
	free(cwd);
	if (out.len == 0 || out.data[out.len - 1] != '/') {
		Buf_appendc(&out, '/');
	}
	Buf_appendstr(&out, normal);
	free(normal);
	return bufTake(&out);
}

static StringVec splitList(const char *value, char delimiter) {
	StringVec entries = {0};
	size_t n = strlen(value);
	size_t start = 0;
	while (start <= n) {
		const char *found = memchr(value + start, delimiter, n - start);
		size_t end = found ? (size_t)(found - value) : n;
		vec_push(&entries, xstrndup(value + start, end - start));
		if (end == n) {
			break;
		}
		start = end + 1;
	}
	return entries;
}

static char *toWindowsPathEntry(const char *entry) {
	if (entry[0] == '\0') {
		return xstrdup("");
	}
	bool looksWindows = strchr(entry, '\\') != NULL || (strlen(entry) >= 2 && entry[1] == ':' && entry[0] != '/');
	if (looksWindows) {
		char *normalized = xstrdup(entry);
		replaceChar(normalized, '/', '\\');
		return normalized;
	}
	return files_pathToWindows(entry);
}

static char *toHostPathEntry(const char *entry) {
	if (entry[0] == '\0') {
		return xstrdup("");
	}
	char *converted = files_pathFromWindows(entry);
	if (converted[0] != '\0') {
		return converted;
	}
	free(converted);
	char *normalized = xstrdup(entry);
	replaceChar(normalized, '\\', '/');
	return normalized;
}

static void readAt(FileObject *file, uint8_t *in, size_t bytesToRead, off_t pos, IOResult *result) {
	size_t total = 0;
	size_t remaining = bytesToRead;
	while (remaining > 0) {
		size_t chunk = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
		ssize_t rc = pread(file->base.fd, in + total, chunk, pos);
		if (rc == -1) {
			if (errno == EINTR) {
				continue;
			}
			result->unixError = errno ? errno : EIO;
			break;
		}
		if (rc == 0) {
			result->reachedEnd = true;
			break;
		}
		total += (size_t)rc;
		remaining -= (size_t)rc;
		pos += rc;
	}
	result->bytesTransferred = total;
}

static void writeAt(FileObject *file, const uint8_t *in, size_t bytesToWrite, off_t pos, IOResult *result) {
	size_t total = 0;
	size_t remaining = bytesToWrite;
	while (remaining > 0) {
		size_t chunk = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
		ssize_t rc = pwrite(file->base.fd, in + total, chunk, pos);
		if (rc == -1) {
			if (errno == EINTR) {
				continue;
			}
			result->unixError = errno ? errno : EIO;
			break;
		}
		if (rc == 0) {
			break;
		}
		total += (size_t)rc;
		remaining -= (size_t)rc;
		pos += rc;
	}
	result->bytesTransferred = total;
}

char *files_pathFromWindows(const char *inStr) {
	// Convert to forward slashes
	char *str = xstrdup(inStr);
	replaceChar(str, '\\', '/');

	// Remove "//?/" prefix
	const char *s = str;
	if (strncmp(s, "//?/", 4) == 0) {
		s += 4;
	}

	// Remove the drive letter
	if (s[0] != '\0' && s[1] == ':' && s[2] == '/' && strchr("zZcC", s[0]) != NULL) {
		s += 2;
	}

	// Apply Windows trailing-dot normalization per path component.
	char *stripped = stripTrailingDots(s);
	free(str);

	// Return as-is if it exists, else traverse the filesystem looking for
	// a path that matches case insensitively
	char *path = lexicallyNormal(stripped);
	free(stripped);
	if (pathExists(path)) {
		return path;
	}

	bool absolute = path[0] == '/';
	char *newPath = xstrdup(absolute ? "/" : ".");
	bool followingExisting = true;
	size_t len = strlen(path);
	size_t i = 0;
	while (i < len) {
		while (i < len && path[i] == '/') {
			i++;
		}
		if (i >= len) {
			break;
		}
		size_t start = i;
		while (i < len && path[i] != '/') {
			i++;
		}
		char *component = xstrndup(path + start, i - start);
		bool isDot = strcmp(component, ".") == 0 || strcmp(component, "..") == 0;
		char *newPath2 = joinPath(newPath, component);
		if (followingExisting && !pathExists(newPath2) && !isDot) {
			followingExisting = false;
			DIR *dir = opendir(newPath);
			if (dir) {
				struct dirent *entry;
				while ((entry = readdir(dir)) != NULL) {
					if (strcasecmp(entry->d_name, component) == 0) {
						followingExisting = true;
						free(newPath2);
						newPath2 = joinPath(newPath, entry->d_name);
						break;
					}
				}
				closedir(dir);
			}
		}
		free(component);
		free(newPath);
		newPath = newPath2;
	}
	if (len > 1 && path[len - 1] == '/' && newPath[strlen(newPath) - 1] != '/') {
		char *withSlash = joinPath(newPath, "");
		free(newPath);
		newPath = withSlash;
	}
	free(path);
	if (followingExisting) {
		DEBUG_LOG("Resolved case-insensitive path: %s\n", newPath);
	} else {
		DEBUG_LOG("Failed to resolve path: %s\n", newPath);
	}
	return newPath;
}

char *files_pathToWindows(const char *path) {
	char *normal = lexicallyNormal(path);
	char *str;
	if (path[0] == '/') {
		size_t len = strlen(normal);
		str = xmalloc(len + 3);
		memcpy(str, "Z:", 2);
		memcpy(str + 2, normal, len + 1);
	} else {
		str = xstrdup(normal);
	}
	free(normal);
	replaceChar(str, '/', '\\');
	return str;
}

IOResult files_read(FileObject *file, void *buffer, size_t bytesToRead, const off_t *offset, bool updateFilePointer) {
	IOResult result = {0};
	if (!file || !FsObject_valid(&file->base)) {
		result.unixError = EBADF;
		return result;
	}
	if (bytesToRead == 0) {
		return result;
	}

	// Sanity check: if no offset is given, we must update the file pointer
	assert(offset || updateFilePointer);

	if (file->isPipe) {
		size_t chunk = bytesToRead > SSIZE_MAX ? SSIZE_MAX : bytesToRead;
		ssize_t rc;
		int err;
		pthread_mutex_lock(&file->base.m);
		do {
			rc = read(file->base.fd, buffer, chunk);
			err = errno;
		} while (rc == -1 && err == EINTR);
		pthread_mutex_unlock(&file->base.m);
		if (rc == -1) {
			result.unixError = err ? err : EIO;
		} else if (rc == 0) {
			result.reachedEnd = true;
		} else {
			result.bytesTransferred = (size_t)rc;
		}
		return result;
	}

	if (updateFilePointer || !offset) {
		pthread_mutex_lock(&file->base.m);
		const off_t pos = offset ? *offset : file->filePos;
		readAt(file, buffer, bytesToRead, pos, &result);
		if (updateFilePointer) {
			file->filePos = pos + (off_t)result.bytesTransferred;
		}
		pthread_mutex_unlock(&file->base.m);
	} else {
		readAt(file, buffer, bytesToRead, *offset, &result);
	}

	return result;
}

IOResult files_write(FileObject *file, const void *buffer, size_t bytesToWrite, const off_t *offset,
					 bool updateFilePointer) {
	IOResult result = {0};
	if (!file || !FsObject_valid(&file->base)) {
		result.unixError = EBADF;
		return result;
	}
	if (bytesToWrite == 0) {
		return result;
	}

	// Sanity check: if no offset is given, we must update the file pointer
	assert(offset || updateFilePointer);

	if (file->appendOnly || file->isPipe) {
		const uint8_t *in = buffer;
		size_t total = 0;
		size_t remaining = bytesToWrite;
		pthread_mutex_lock(&file->base.m);
		while (remaining > 0) {
			size_t chunk = remaining > SSIZE_MAX ? SSIZE_MAX : remaining;
			ssize_t rc = write(file->base.fd, in + total, chunk);
			if (rc == -1) {
				if (errno == EINTR) {
					continue;
				}
				result.unixError = errno ? errno : EIO;
				break;
			}
			if (rc == 0) {
				break;
			}
			total += (size_t)rc;
			remaining -= (size_t)rc;
		}
		result.bytesTransferred = total;
		if (updateFilePointer) {
			off_t pos = file->isPipe ? 0 : lseek(file->base.fd, 0, SEEK_CUR);
			if (pos >= 0) {
				file->filePos = pos;
			} else if (result.unixError == 0) {
				result.unixError = errno ? errno : EIO;
			}
		}
		pthread_mutex_unlock(&file->base.m);
		return result;
	}

	if (updateFilePointer || !offset) {
		pthread_mutex_lock(&file->base.m);
		const off_t pos = offset ? *offset : file->filePos;
		writeAt(file, buffer, bytesToWrite, pos, &result);
		if (updateFilePointer) {
			file->filePos = pos + (off_t)result.bytesTransferred;
		}
		pthread_mutex_unlock(&file->base.m);
	} else {
		writeAt(file, buffer, bytesToWrite, *offset, &result);
	}

	return result;
}

HANDLE files_getStdHandle(DWORD nStdHandle) {
	switch (nStdHandle) {
	case STD_INPUT_HANDLE:
		return stdinHandle;
	case STD_OUTPUT_HANDLE:
		return stdoutHandle;
	case STD_ERROR_HANDLE:
		return stderrHandle;
	default:
		return INVALID_HANDLE_VALUE;
	}
}

BOOL files_setStdHandle(DWORD nStdHandle, HANDLE hHandle) {
	switch (nStdHandle) {
	case STD_INPUT_HANDLE:
		stdinHandle = hHandle;
		break;
	case STD_OUTPUT_HANDLE:
		stdoutHandle = hHandle;
		break;
	case STD_ERROR_HANDLE:
		stderrHandle = hHandle;
		break;
	default:
		return 0; // fail
	}
	return 1; // success
}

void files_init(void) {
	signal(SIGPIPE, SIG_IGN);
	Handles *handles = wibo_handles();
	FileObject *stdinObject = FileObject_alloc(ObjectType_File, STDIN_FILENO);
	stdinObject->base.closeOnDestroy = false;
	stdinHandle = Handles_alloc(handles, &stdinObject->base.base, FILE_GENERIC_READ, 0);
	FileObject *stdoutObject = FileObject_alloc(ObjectType_File, STDOUT_FILENO);
	stdoutObject->base.closeOnDestroy = false;
	stdoutObject->appendOnly = true;
	stdoutHandle = Handles_alloc(handles, &stdoutObject->base.base, FILE_GENERIC_WRITE, 0);
	FileObject *stderrObject = FileObject_alloc(ObjectType_File, STDERR_FILENO);
	stderrObject->base.closeOnDestroy = false;
	stderrObject->appendOnly = true;
	stderrHandle = Handles_alloc(handles, &stderrObject->base.base, FILE_GENERIC_WRITE, 0);
}

char *files_findCaseInsensitiveFile(const char *directory, const char *filename) {
	if (directory[0] == '\0') {
		return NULL;
	}
	struct stat st;
	if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return NULL;
	}
	char *needle = xstrdup(filename);
	toLowerInPlace(needle);
	DIR *dir = opendir(directory);
	if (dir) {
		struct dirent *entry;
		while ((entry = readdir(dir)) != NULL) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}
			char *candidate = xstrdup(entry->d_name);
			toLowerInPlace(candidate);
			bool match = strcmp(candidate, needle) == 0;
			free(candidate);
			if (match) {
				char *entryPath = joinPath(directory, entry->d_name);
				char *canonical = files_canonicalPath(entryPath);
				free(entryPath);
				closedir(dir);
				free(needle);
				return canonical;
			}
		}
		closedir(dir);
	}
	free(needle);
	char *direct = joinPath(directory, filename);
	char *result = NULL;
	if (pathExists(direct)) {
		result = files_canonicalPath(direct);
	}
	free(direct);
	return result;
}

char *files_canonicalPath(const char *path) {
	return weaklyCanonical(path);
}

char *files_hostPathListToWindows(const char *value) {
	if (value[0] == '\0') {
		return xstrdup(value);
	}
	char delimiter = strchr(value, ';') ? ';' : ':';
	StringVec entries = splitList(value, delimiter);
	Buf result = {0};
	for (size_t i = 0; i < entries.len; i++) {
		if (i != 0) {
			Buf_appendc(&result, ';');
		}
		if (entries.data[i][0] != '\0') {
			char *converted = toWindowsPathEntry(entries.data[i]);
			Buf_appendstr(&result, converted);
			free(converted);
		}
	}
	freeStrings(&entries);
	return bufTake(&result);
}

char *files_windowsPathListToHost(const char *value) {
	if (value[0] == '\0') {
		return xstrdup(value);
	}
	StringVec entries = splitList(value, ';');
	Buf result = {0};
	for (size_t i = 0; i < entries.len; i++) {
		if (i != 0) {
			Buf_appendc(&result, ':');
		}
		if (entries.data[i][0] != '\0') {
			char *converted = toHostPathEntry(entries.data[i]);
			Buf_appendstr(&result, converted);
			free(converted);
		}
	}
	freeStrings(&entries);
	return bufTake(&result);
}
