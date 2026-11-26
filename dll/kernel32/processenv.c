#include "processenv.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "files.h"
#include "heap.h"
#include "internal.h"
#include "strutil.h"
#include "types.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

extern char **environ;

static GUEST_PTR g_commandLineA = GUEST_NULL;
static GUEST_PTR g_commandLineW = GUEST_NULL;
static pthread_once_t g_tempEnvOnce = PTHREAD_ONCE_INIT;

// Returns a malloc'd string.
static char *convertEnvValueForWindows(const char *name, const char *rawValue) {
	if (!rawValue) {
		return xstrdup("");
	}
	if (strcasecmp(name, "PATH") != 0) {
		if (strcasecmp(name, "TMP") != 0 && strcasecmp(name, "TEMP") != 0) {
			return xstrdup(rawValue);
		}
		bool looksWindows = strchr(rawValue, '\\') != NULL ||
							(strlen(rawValue) >= 2 && rawValue[1] == ':' && rawValue[0] != '/');
		if (looksWindows) {
			char *path = xstrdup(rawValue);
			for (char *p = path; *p; ++p) {
				if (*p == '/') {
					*p = '\\';
				}
			}
			return path;
		}
		return files_pathToWindows(rawValue);
	}
	char *converted = files_hostPathListToWindows(rawValue);
	if (converted && *converted) {
		return converted;
	}
	free(converted);
	return xstrdup(rawValue);
}

// Returns a malloc'd string.
static char *convertEnvValueToHost(const char *name, const char *rawValue) {
	if (!rawValue) {
		return xstrdup("");
	}
	if (strcasecmp(name, "PATH") != 0) {
		return xstrdup(rawValue);
	}
	char *converted = files_windowsPathListToHost(rawValue);
	if (converted && *converted) {
		return converted;
	}
	free(converted);
	return xstrdup(rawValue);
}

static const char *getenvCaseInsensitive(const char *name) {
	const char *exact = getenv(name);
	if (exact) {
		return exact;
	}
	const size_t nameLen = strlen(name);
	for (char **work = environ; *work; ++work) {
		const char *eq = strchr(*work, '=');
		if (!eq) {
			continue;
		}
		const size_t entryNameLen = (size_t)(eq - *work);
		if (entryNameLen == nameLen && strncasecmp(*work, name, nameLen) == 0) {
			return eq + 1;
		}
	}
	return NULL;
}

static void initTempEnvVariables(void) {
	const char *hostTemp = getenv("TMPDIR");
	if (!hostTemp || !*hostTemp) {
		hostTemp = "/tmp";
	}
	if (!getenvCaseInsensitive("TMP")) {
		setenv("TMP", hostTemp, 0);
	}
	if (!getenvCaseInsensitive("TEMP")) {
		setenv("TEMP", hostTemp, 0);
	}
}

static void ensureTempEnvVariables(void) { pthread_once(&g_tempEnvOnce, initTempEnvVariables); }

// Returns a malloc'd Windows value, or NULL if the variable is not set.
static char *getEnvValueForWindows(const char *name) {
	ensureTempEnvVariables();
	const char *rawValue = getenvCaseInsensitive(name);
	if (!rawValue) {
		return NULL;
	}
	return convertEnvValueForWindows(name, rawValue);
}

// Fills `out` with the NUL-separated "NAME=value" block and returns its size including the final NUL.
static size_t prepareEnvStrings(Buf *out) {
	ensureTempEnvVariables();
	for (char **work = environ; *work; ++work) {
		const char *eq = strchr(*work, '=');
		if (eq) {
			char *name = xstrndup(*work, (size_t)(eq - *work));
			char *converted = convertEnvValueForWindows(name, eq + 1);
			Buf_appendstr(out, name);
			Buf_appendc(out, '=');
			Buf_appendstr(out, converted);
			free(name);
			free(converted);
		} else {
			Buf_appendstr(out, *work);
		}
		Buf_appendc(out, '\0');
	}
	Buf_appendc(out, '\0');
	return out->len;
}

GUEST_PTR WINAPI GetCommandLineA() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCommandLineA() -> %s\n", wibo_commandLine);
	if (g_commandLineA == GUEST_NULL) {
		const size_t len = strlen(wibo_commandLine);
		void *tmp = wibo_heap_guestMalloc(len + 1, true);
		memcpy(tmp, wibo_commandLine, len);
		g_commandLineA = toGuestPtr(tmp);
	}
	return g_commandLineA;
}

GUEST_PTR WINAPI GetCommandLineW() {
	HOST_CONTEXT_GUARD();
	char *commandLineA = wideStringToString(wibo_commandLineW, -1);
	DEBUG_LOG("GetCommandLineW() -> %s\n", commandLineA);
	free(commandLineA);
	if (g_commandLineW == GUEST_NULL) {
		const size_t len = wstrlen(wibo_commandLineW);
		void *tmp = wibo_heap_guestMalloc((len + 1) * sizeof(WCHAR), true);
		memcpy(tmp, wibo_commandLineW, (len + 1) * sizeof(WCHAR));
		g_commandLineW = toGuestPtr(tmp);
	}
	return g_commandLineW;
}

HANDLE WINAPI GetStdHandle(DWORD nStdHandle) {
	HOST_CONTEXT_GUARD();
	HANDLE handle = files_getStdHandle(nStdHandle);
	DEBUG_LOG("GetStdHandle(%d) -> %p\n", nStdHandle, handle);
	return handle;
}

BOOL WINAPI SetStdHandle(DWORD nStdHandle, HANDLE hHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetStdHandle(%d, %p)\n", nStdHandle, hHandle);
	return files_setStdHandle(nStdHandle, hHandle);
}

GUEST_PTR WINAPI GetEnvironmentStrings() { return GetEnvironmentStringsA(); }

GUEST_PTR WINAPI GetEnvironmentStringsA() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetEnvironmentStringsA()\n");

	Buf strings = {0};
	const size_t bufSize = prepareEnvStrings(&strings);

	char *buffer = wibo_heap_guestMalloc(bufSize, false);
	if (!buffer) {
		Buf_free(&strings);
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	memcpy(buffer, strings.data, bufSize);
	Buf_free(&strings);

	return toGuestPtr(buffer);
}

GUEST_PTR WINAPI GetEnvironmentStringsW() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetEnvironmentStringsW()\n");

	Buf strings = {0};
	const size_t bufSizeW = prepareEnvStrings(&strings);

	uint16_t *buffer = wibo_heap_guestMalloc(bufSizeW * sizeof(uint16_t), false);
	if (!buffer) {
		Buf_free(&strings);
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	for (size_t i = 0; i < bufSizeW; ++i) {
		buffer[i] = (uint16_t)(unsigned char)strings.data[i];
	}
	Buf_free(&strings);

	return toGuestPtr(buffer);
}

BOOL WINAPI FreeEnvironmentStringsA(LPCH penv) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FreeEnvironmentStringsA(%p)\n", penv);
	if (!penv) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	wibo_heap_guestFree((void *)penv);
	return TRUE;
}

BOOL WINAPI FreeEnvironmentStringsW(LPWCH penv) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FreeEnvironmentStringsW(%p)\n", penv);
	if (!penv) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	wibo_heap_guestFree((void *)penv);
	return TRUE;
}

DWORD WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetEnvironmentVariableA(%s, %p, %u)\n", lpName ? lpName : "(null)", lpBuffer, nSize);
	if (!lpName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *value = getEnvValueForWindows(lpName);
	if (!value) {
		kernel32_setLastError(ERROR_ENVVAR_NOT_FOUND);
		return 0;
	}
	const DWORD len = (DWORD)strlen(value);
	if (nSize == 0) {
		free(value);
		return len + 1;
	}
	if (!lpBuffer) {
		free(value);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (nSize <= len) {
		free(value);
		return len + 1;
	}
	memcpy(lpBuffer, value, len + 1);
	free(value);
	return len;
}

DWORD WINAPI GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	char *name = wideStringToString(lpName, -1);
	DEBUG_LOG("GetEnvironmentVariableW(%s, %p, %u)\n", name, lpBuffer, nSize);
	if (!*name) {
		free(name);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	char *value = getEnvValueForWindows(name);
	free(name);
	if (!value) {
		kernel32_setLastError(ERROR_ENVVAR_NOT_FOUND);
		return 0;
	}
	uint16_t *wideValue = stringToWideString(value, SIZE_MAX);
	free(value);
	const DWORD required = (DWORD)wstrlen(wideValue) + 1;
	if (nSize == 0) {
		free(wideValue);
		return required;
	}
	if (!lpBuffer) {
		free(wideValue);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (nSize < required) {
		free(wideValue);
		return required;
	}
	memcpy(lpBuffer, wideValue, required * sizeof(uint16_t));
	free(wideValue);
	return required - 1;
}

BOOL WINAPI SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetEnvironmentVariableA(%s, %s)\n", lpName ? lpName : "(null)", lpValue ? lpValue : "(null)");
	if (!lpName || strchr(lpName, '=')) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ensureTempEnvVariables();
	if (!lpValue) {
		if (unsetenv(lpName) != 0) {
			kernel32_setLastErrorFromErrno();
			return FALSE;
		}
		return TRUE;
	}
	char *hostValue = convertEnvValueToHost(lpName, lpValue);
	const int rc = setenv(lpName, hostValue, 1);
	free(hostValue);
	if (rc != 0) {
		kernel32_setLastErrorFromErrno();
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetEnvironmentVariableW -> ");
	if (!lpName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG("ERROR_INVALID_PARAMETER\n");
		return FALSE;
	}
	char *name = wideStringToString(lpName, -1);
	char *value = lpValue ? wideStringToString(lpValue, -1) : NULL;
	BOOL result = SetEnvironmentVariableA(name, value);
	free(name);
	free(value);
	return result;
}
