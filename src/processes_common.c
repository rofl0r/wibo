#include "processes.h"

#include "common.h"
#include "files.h"
#include "handles.h"
#include "kernel32/internal.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
extern char **environ;
#endif

static ProcessManager g_processManager;
static pthread_once_t g_processManagerOnce = PTHREAD_ONCE_INIT;

static void createProcessManagerImpl(void) {
	g_processManager.mImpl = wibo_detail_createProcessManagerImpl();
}

bool ProcessManager_init(ProcessManager *self) {
	if (!self->mImpl) {
		return false;
	}
	return self->mImpl->init(self->mImpl);
}

void ProcessManager_shutdown(ProcessManager *self) {
	if (self->mImpl) {
		self->mImpl->shutdown(self->mImpl);
	}
}

bool ProcessManager_addProcess(ProcessManager *self, ProcessObject *po) {
	if (!self->mImpl) {
		ObjectBase_unref(&po->base.base);
		return false;
	}
	return self->mImpl->addProcess(self->mImpl, po);
}

bool ProcessManager_running(const ProcessManager *self) {
	return self->mImpl && self->mImpl->running(self->mImpl);
}

ProcessManager *wibo_processes(void) {
	pthread_once(&g_processManagerOnce, createProcessManagerImpl);
	if (!ProcessManager_init(&g_processManager)) {
		fprintf(stderr, "Failed to initialize ProcessManager\n");
		abort();
	}
	return &g_processManager;
}

static char *strConcat(const char *a, const char *b) {
	size_t aLen = strlen(a);
	size_t bLen = strlen(b);
	char *out = xmalloc(aLen + bLen + 1);
	memcpy(out, a, aLen);
	memcpy(out + aLen, b, bLen + 1);
	return out;
}

static char *currentPath(void) {
	char *cwd = getcwd(NULL, 0);
	if (!cwd) {
		fprintf(stderr, "Failed to determine current directory\n");
		abort();
	}
	return cwd;
}

// Returns the directory portion of path, or NULL if path has no parent component.
static char *parentPath(const char *path) {
	const char *slash = strrchr(path, '/');
	if (!slash) {
		return NULL;
	}
	if (slash == path) {
		return xstrdup("/");
	}
	return xstrndup(path, (size_t)(slash - path));
}

static const char *fileNameOf(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static bool hasDirectoryComponent(const char *command) {
	return strchr(command, '/') || strchr(command, '\\') || strchr(command, ':');
}

static bool hasExtension(const char *command) {
	const char *dot = strrchr(command, '.');
	const char *lastSep = NULL;
	for (const char *p = command; *p; ++p) {
		if (*p == '/' || *p == '\\') {
			lastSep = p;
		}
	}
	return dot && (!lastSep || dot > lastSep + 1);
}

static char *prependDot(const char *s, size_t len) {
	char *out = xmalloc(len + 2);
	out[0] = '.';
	memcpy(out + 1, s, len);
	out[len + 1] = '\0';
	return out;
}

static StringVec pathextValues(void) {
	const char *envValue = getenv("PATHEXT");
	const char *raw = envValue ? envValue : ".COM;.EXE;.BAT;.CMD";
	size_t rawLen = strlen(raw);
	StringVec exts = {0};
	size_t start = 0;
	while (start <= rawLen) {
		const char *semi = strchr(raw + start, ';');
		size_t end = semi ? (size_t)(semi - raw) : rawLen;
		size_t partLen = end - start;
		if (partLen) {
			if (raw[start] != '.') {
				vec_push(&exts, prependDot(raw + start, partLen));
			} else {
				vec_push(&exts, xstrndup(raw + start, partLen));
			}
		}
		if (end == rawLen) {
			break;
		}
		start = end + 1;
	}
	if (exts.len == 0) {
		vec_push(&exts, xstrdup(".COM"));
		vec_push(&exts, xstrdup(".EXE"));
		vec_push(&exts, xstrdup(".BAT"));
		vec_push(&exts, xstrdup(".CMD"));
	}
	return exts;
}

// Appends the entries of a host PATH-style list (';' or ':' separated) to out, converting Windows-style entries.
static void appendHostPathEntries(StringVec *out, const char *value) {
	char delim = strchr(value, ';') ? ';' : ':';
	size_t len = strlen(value);
	size_t start = 0;
	while (start <= len) {
		const char *d = strchr(value + start, delim);
		size_t end = d ? (size_t)(d - value) : len;
		size_t n = end - start;
		if (n) {
			char *entry = xstrndup(value + start, n);
			bool looksWindows = strchr(entry, '\\') || (n >= 2 && entry[1] == ':' && entry[0] != '/');
			char *candidate = NULL;
			if (looksWindows) {
				char *converted = files_pathFromWindows(entry);
				if (converted && *converted) {
					candidate = converted;
				} else {
					free(converted);
				}
			}
			if (candidate) {
				free(entry);
			} else {
				candidate = entry;
			}
			vec_push(out, candidate);
		}
		if (end == len) {
			break;
		}
		start = end + 1;
	}
}

static void appendEnvPathEntries(StringVec *dirs, const char *envVar) {
	const char *envPath = getenv(envVar);
	if (envPath) {
		appendHostPathEntries(dirs, envPath);
	}
}

static StringVec buildSearchDirectories(void) {
	StringVec dirs = {0};
	if (wibo_guestExecutablePath) {
		char *parent = parentPath(wibo_guestExecutablePath);
		if (parent) {
			vec_push(&dirs, parent);
		}
	}
	vec_push(&dirs, currentPath());
	appendEnvPathEntries(&dirs, "WIBO_PATH");
	appendEnvPathEntries(&dirs, "WINEPATH");
	appendEnvPathEntries(&dirs, "PATH");
	return dirs;
}

// Resolves a single candidate name that contains a directory component. Returns a malloc'd path or NULL.
static char *tryResolveDirect(const char *name) {
	char *host = files_pathFromWindows(name);
	if (!host || !*host) {
		free(host);
		host = xstrdup(name);
		for (char *p = host; *p; ++p) {
			if (*p == '\\') {
				*p = '/';
			}
		}
	}

	char *parent = parentPath(host);
	char *dir = parent ? parent : currentPath();
	const char *filename = fileNameOf(host);
	char *resolved = NULL;

	char *found = files_findCaseInsensitiveFile(dir, filename);
	if (found) {
		resolved = files_canonicalPath(found);
		free(found);
	} else {
		struct stat st;
		if (*filename && stat(host, &st) == 0) {
			resolved = files_canonicalPath(host);
		}
	}

	free(dir);
	free(host);
	return resolved;
}

// Searches the PATH-like directories for the first candidate that exists. Returns a malloc'd canonical path or NULL.
static char *searchCandidates(const StringVec *candidates) {
	StringVec dirs = buildSearchDirectories();
	char *result = NULL;
	for (size_t d = 0; d < dirs.len && !result; ++d) {
		for (size_t c = 0; c < candidates->len && !result; ++c) {
			char *found = files_findCaseInsensitiveFile(dirs.data[d], candidates->data[c]);
			if (found) {
				result = files_canonicalPath(found);
				free(found);
			}
		}
	}
	StringVec_free(&dirs);
	return result;
}

char *wibo_resolveExecutable(const char *command, bool searchPath) {
	if (!command || !*command) {
		return NULL;
	}

	StringVec candidates = {0};
	vec_push(&candidates, xstrdup(command));
	if (!hasExtension(command)) {
		StringVec exts = pathextValues();
		for (size_t i = 0; i < exts.len; ++i) {
			vec_push(&candidates, strConcat(command, exts.data[i]));
		}
		StringVec_free(&exts);
	}

	char *result = NULL;
	if (hasDirectoryComponent(command)) {
		for (size_t i = 0; i < candidates.len && !result; ++i) {
			result = tryResolveDirect(candidates.data[i]);
		}
	} else if (searchPath) {
		result = searchCandidates(&candidates);
	}

	StringVec_free(&candidates);
	return result;
}

static int spawnInternal(const StringVec *args, ProcessObject **pinOut) {
	char **argv = xmalloc((args->len + 2) * sizeof(*argv));
	argv[0] = (char *)"wibo";
	for (size_t i = 0; i < args->len; ++i) {
		argv[i + 1] = args->data[i];
	}
	argv[args->len + 1] = NULL;

	if (wibo_debugEnabled) {
		Buf cmdline = {0};
		for (size_t i = 1; i <= args->len; ++i) {
			if (i != 1) {
				Buf_appendc(&cmdline, ' ');
			}
			Buf_appendc(&cmdline, '\'');
			Buf_appendstr(&cmdline, argv[i]);
			Buf_appendc(&cmdline, '\'');
		}
		DEBUG_LOG("Spawning process: %s %s\n", argv[0], Buf_cstr(&cmdline));
		Buf_free(&cmdline);
	}

	size_t environCount = 0;
	for (char **e = environ; *e; ++e) {
		++environCount;
	}
	char **envp = xmalloc((environCount + 2) * sizeof(*envp));
	size_t envLen = 0;
	for (char **e = environ; *e; ++e) {
		if (strncmp(*e, "WIBO_DEBUG_INDENT=", 18) != 0) {
			envp[envLen++] = *e;
		}
	}
	char indentVar[64];
	snprintf(indentVar, sizeof(indentVar), "WIBO_DEBUG_INDENT=%u", wibo_debugIndent + 1);
	envp[envLen++] = xstrdup(indentVar);
	envp[envLen] = NULL;

	SpawnProcessInfo info = {.pid = -1, .pidfd = -1};
	int rc = wibo_detail_spawnProcess(argv, envp, &info);
	free(envp[envLen - 1]);
	free(envp);
	free(argv);
	if (rc != 0) {
		return rc;
	}

	DEBUG_LOG("Spawned process with PID %d (pidfd=%d)\n", info.pid, info.pidfd);

	ProcessObject *obj = ProcessObject_alloc(info.pid, info.pidfd, true);
	ObjectBase_ref(&obj->base.base);
	*pinOut = obj;
	if (!ProcessManager_addProcess(wibo_processes(), obj)) {
		fprintf(stderr, "Failed to add process to process manager\n");
		abort();
	}
	return 0;
}

int wibo_spawnWithCommandLine(const char *applicationName, const char *commandLine, ProcessObject **pinOut) {
	bool hasApp = applicationName && *applicationName;
	bool hasCmdLine = commandLine && *commandLine;
	if (!hasApp && !hasCmdLine) {
		return ENOENT;
	}

	StringVec args = {0};
	if (hasCmdLine) {
		vec_push(&args, xstrdup("--cmdline"));
		vec_push(&args, xstrdup(commandLine));
	}
	if (hasApp) {
		vec_push(&args, xstrdup(applicationName));
	}

	int rc = spawnInternal(&args, pinOut);
	StringVec_free(&args);
	return rc;
}

int wibo_spawnWithArgv(const char *applicationName, const StringVec *argv, ProcessObject **pinOut) {
	bool hasApp = applicationName && *applicationName;
	if (!hasApp && argv->len == 0) {
		return ENOENT;
	}

	StringVec args = {0};
	if (hasApp) {
		vec_push(&args, xstrdup(applicationName));
	}
	vec_push(&args, xstrdup("--"));
	for (size_t i = 0; i < argv->len; ++i) {
		vec_push(&args, xstrdup(argv->data[i]));
	}

	int rc = spawnInternal(&args, pinOut);
	StringVec_free(&args);
	return rc;
}

static void appendRepeated(Buf *b, char c, int count) {
	for (int i = 0; i < count; ++i) {
		Buf_appendc(b, c);
	}
}

StringVec wibo_splitCommandLine(const char *commandLine) {
	StringVec result = {0};
	if (!commandLine) {
		return result;
	}
	size_t len = strlen(commandLine);
	size_t i = 0;
	while (i < len) {
		while (i < len && (commandLine[i] == ' ' || commandLine[i] == '\t')) {
			++i;
		}
		if (i >= len) {
			break;
		}
		Buf arg = {0};
		bool inQuotes = false;
		int backslashes = 0;
		for (; i < len; ++i) {
			char c = commandLine[i];
			if (c == '\\') {
				++backslashes;
				continue;
			}
			if (c == '"') {
				if ((backslashes % 2) == 0) {
					appendRepeated(&arg, '\\', backslashes / 2);
					inQuotes = !inQuotes;
				} else {
					appendRepeated(&arg, '\\', backslashes / 2);
					Buf_appendc(&arg, '"');
				}
				backslashes = 0;
				continue;
			}
			appendRepeated(&arg, '\\', backslashes);
			backslashes = 0;
			if (!inQuotes && (c == ' ' || c == '\t')) {
				break;
			}
			Buf_appendc(&arg, c);
		}
		appendRepeated(&arg, '\\', backslashes);
		vec_push(&result, Buf_cstr(&arg));
		Buf_init(&arg);
		while (i < len && (commandLine[i] == ' ' || commandLine[i] == '\t')) {
			++i;
		}
	}
	return result;
}

DWORD wibo_getThreadId(void) {
#if defined(HAVE_PTHREAD_GETTID_NP)
	pid_t threadId = pthread_gettid_np(pthread_self());
#elif defined(__linux__)
	static __thread pid_t threadId = 0;
	if (threadId == 0) {
		threadId = gettid();
	}
#elif defined(__APPLE__)
	uint64_t threadId = 0;
	pthread_threadid_np(NULL, &threadId);
#else
#error "Unknown platform"
#endif
	return (DWORD)threadId;
}
