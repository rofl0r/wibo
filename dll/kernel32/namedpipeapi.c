#include "namedpipeapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "overlapped_util.h"
#include "strutil.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct NamedPipeInstance NamedPipeInstance;
typedef struct NamedPipeState NamedPipeState;
typedef VEC(NamedPipeInstance *) NamedPipeInstanceVec;

static const char kLocalPipePrefix[] = "\\\\.\\pipe\\";
static const char kNtPipePrefix[] = "\\\\?\\pipe\\";

static void configureInheritability(int fd, bool inherit) {
	if (fd < 0) {
		return;
	}
	int flags = fcntl(fd, F_GETFD);
	if (flags == -1) {
		return;
	}
	if (inherit) {
		flags &= ~FD_CLOEXEC;
	} else {
		flags |= FD_CLOEXEC;
	}
	fcntl(fd, F_SETFD, flags);
}

typedef struct {
	char *key;
	uint16_t *namespaceKey;
	size_t namespaceKeyLen;
} ParsedPipeName;

static void ParsedPipeName_free(ParsedPipeName *self) {
	free(self->key);
	free(self->namespaceKey);
	self->key = NULL;
	self->namespaceKey = NULL;
	self->namespaceKeyLen = 0;
}

static bool hasPrefix(const char *s, size_t len, const char *prefix, size_t prefixLen) {
	return len >= prefixLen && memcmp(s, prefix, prefixLen) == 0;
}

// Returns false when the name is not a pipe path (with *error left as ERROR_SUCCESS) or when it is invalid.
static bool parsePipeName(LPCSTR name, DWORD *error, ParsedPipeName *out) {
	*error = ERROR_SUCCESS;
	if (!name) {
		*error = ERROR_PATH_NOT_FOUND;
		return false;
	}
	size_t inputLen = strlen(name);
	if (inputLen == 0) {
		*error = ERROR_INVALID_NAME;
		return false;
	}
	if (inputLen > 256) {
		*error = ERROR_INVALID_PARAMETER;
		return false;
	}
	char *lower = xmalloc(inputLen + 1);
	for (size_t i = 0; i < inputLen; i++) {
		lower[i] = (char)tolower((unsigned char)name[i]);
	}
	lower[inputLen] = '\0';

	const size_t localLen = sizeof(kLocalPipePrefix) - 1;
	const size_t ntLen = sizeof(kNtPipePrefix) - 1;
	size_t prefixLen = 0;
	if (hasPrefix(lower, inputLen, kLocalPipePrefix, localLen)) {
		prefixLen = localLen;
	} else if (hasPrefix(lower, inputLen, kNtPipePrefix, ntLen)) {
		prefixLen = ntLen;
	} else {
		// Not a pipe path; treat as non-match without error.
		free(lower);
		return false;
	}

	const size_t rawLen = inputLen - prefixLen;
	if (rawLen == 0) {
		*error = ERROR_INVALID_HANDLE;
		free(lower);
		return false;
	}
	if (memchr(name + prefixLen, '\\', rawLen) || memchr(name + prefixLen, '/', rawLen)) {
		*error = ERROR_INVALID_NAME;
		free(lower);
		return false;
	}
	out->key = xstrdup(lower + prefixLen);
	out->namespaceKey = stringToUtf16(lower + prefixLen, rawLen);
	out->namespaceKeyLen = rawLen;
	free(lower);
	return true;
}

static DWORD normalizeMaxInstances(DWORD value) {
	if (value == 0) {
		return 1;
	}
	if (value >= PIPE_UNLIMITED_INSTANCES) {
		return PIPE_UNLIMITED_INSTANCES;
	}
	return value;
}

struct NamedPipeState {
	ObjectBase base;
	pthread_mutex_t mutex;
	char *key;
	DWORD accessMode;
	DWORD pipeType;
	DWORD defaultTimeout;
	DWORD maxInstances;
	uint32_t instanceCount;
	NamedPipeInstanceVec instances;
};

static void NamedPipeState_destroy(ObjectBase *base) {
	NamedPipeState *self = (NamedPipeState *)base;
	Namespace_remove(&wibo_g_namespace, &self->base);
	pthread_mutex_destroy(&self->mutex);
	free(self->key);
	vec_free(&self->instances);
	free(self);
}

static NamedPipeState *NamedPipeState_alloc(const char *key) {
	NamedPipeState *self = xcalloc(1, sizeof(*self));
	ObjectBase_init(&self->base, ObjectType_NamedPipeState, NamedPipeState_destroy);
	pthread_mutex_init(&self->mutex, NULL);
	self->key = xstrdup(key);
	self->accessMode = PIPE_ACCESS_DUPLEX;
	self->pipeType = PIPE_TYPE_BYTE;
	self->defaultTimeout = 0;
	self->maxInstances = PIPE_UNLIMITED_INSTANCES;
	self->instanceCount = 0;
	return self;
}

static ObjectBase *NamedPipeState_makeForName(void *ctx) {
	ParsedPipeName *parsed = ctx;
	NamedPipeState *state = NamedPipeState_alloc(parsed->key);
	return &state->base;
}

static void NamedPipeState_registerInstance(NamedPipeState *self, NamedPipeInstance *inst) {
	pthread_mutex_lock(&self->mutex);
	vec_push(&self->instances, inst);
	pthread_mutex_unlock(&self->mutex);
}

static void NamedPipeState_unregisterInstance(NamedPipeState *self, NamedPipeInstance *inst) {
	pthread_mutex_lock(&self->mutex);
	for (size_t i = 0; i < self->instances.len; i++) {
		if (self->instances.data[i] == inst) {
			vec_erase(&self->instances, i);
			break;
		}
	}
	pthread_mutex_unlock(&self->mutex);
}

static bool NamedPipeState_reserveInstance(NamedPipeState *self, DWORD access, DWORD type, DWORD timeout,
										   DWORD maxAllowed, bool firstFlag, bool isNew, DWORD *error) {
	*error = ERROR_SUCCESS;
	bool reserved = false;
	pthread_mutex_lock(&self->mutex);
	if (isNew) {
		self->accessMode = access;
		self->pipeType = type;
		self->defaultTimeout = timeout;
		self->maxInstances = maxAllowed;
	}
	if (!isNew && (self->accessMode != access || self->pipeType != type || self->defaultTimeout != timeout ||
				   self->maxInstances != maxAllowed)) {
		*error = ERROR_ACCESS_DENIED;
	} else if (firstFlag && self->instanceCount > 0) {
		*error = ERROR_ACCESS_DENIED;
	} else if (self->maxInstances != PIPE_UNLIMITED_INSTANCES && self->instanceCount >= self->maxInstances) {
		*error = ERROR_PIPE_BUSY;
	} else {
		++self->instanceCount;
		reserved = true;
	}
	pthread_mutex_unlock(&self->mutex);
	return reserved;
}

static bool NamedPipeState_releaseInstance(NamedPipeState *self) {
	pthread_mutex_lock(&self->mutex);
	if (self->instanceCount > 0) {
		--self->instanceCount;
	}
	bool empty = self->instanceCount == 0;
	pthread_mutex_unlock(&self->mutex);
	return empty;
}

struct NamedPipeInstance {
	FileObject base;
	NamedPipeState *state; // owns one reference
	int companionFd;
	DWORD accessMode;
	DWORD pipeMode;
	bool clientConnected;
	bool connectPending;
	LPOVERLAPPED pendingOverlapped;
	pthread_mutex_t connectMutex;
	pthread_cond_t connectCv;
};

static void NamedPipeInstance_destroy(ObjectBase *base) {
	NamedPipeInstance *self = (NamedPipeInstance *)base;
	int localCompanion = -1;
	pthread_mutex_lock(&self->connectMutex);
	localCompanion = self->companionFd;
	self->companionFd = -1;
	kernel32_detail_signalOverlappedEvent(&self->base, self->pendingOverlapped, STATUS_PIPE_BROKEN, 0);
	self->pendingOverlapped = NULL;
	self->connectPending = false;
	pthread_cond_broadcast(&self->connectCv);
	pthread_mutex_unlock(&self->connectMutex);

	if (localCompanion >= 0) {
		close(localCompanion);
	}
	if (self->state) {
		NamedPipeState_unregisterInstance(self->state, self);
		NamedPipeState_releaseInstance(self->state);
		ObjectBase_unref(&self->state->base);
	}
	pthread_mutex_destroy(&self->connectMutex);
	pthread_cond_destroy(&self->connectCv);
	FileObject_free(&self->base);
	free(self);
}

// Takes ownership of one reference to state.
static NamedPipeInstance *NamedPipeInstance_alloc(int fd, NamedPipeState *state, int companion, DWORD open,
												  DWORD mode) {
	NamedPipeInstance *self = xcalloc(1, sizeof(*self));
	FileObject_init(&self->base, ObjectType_NamedPipe, fd, NamedPipeInstance_destroy);
	self->state = state;
	self->companionFd = companion;
	self->accessMode = open;
	self->pipeMode = mode;
	pthread_mutex_init(&self->connectMutex, NULL);
	pthread_cond_init(&self->connectCv, NULL);
	if (state) {
		NamedPipeState_registerInstance(state, self);
	}
	return self;
}

static bool NamedPipeInstance_canAcceptClient(NamedPipeInstance *self, DWORD desiredAccess) {
	bool accept = false;
	pthread_mutex_lock(&self->connectMutex);
	if (self->companionFd >= 0 && !self->clientConnected) {
		DWORD access = self->accessMode & PIPE_ACCESS_DUPLEX;
		switch (access) {
		case PIPE_ACCESS_DUPLEX:
			accept = (desiredAccess & (GENERIC_READ | GENERIC_WRITE)) != 0;
			break;
		case PIPE_ACCESS_INBOUND:
			accept = (desiredAccess & (GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
			break;
		case PIPE_ACCESS_OUTBOUND:
			accept = (desiredAccess & (GENERIC_READ | FILE_READ_DATA)) != 0;
			break;
		default:
			break;
		}
	}
	pthread_mutex_unlock(&self->connectMutex);
	return accept;
}

static int NamedPipeInstance_takeCompanion(NamedPipeInstance *self) {
	int fd = -1;
	pthread_mutex_lock(&self->connectMutex);
	if (self->companionFd >= 0 && !self->clientConnected) {
		fd = self->companionFd;
		self->companionFd = -1;
		self->clientConnected = true;
		kernel32_detail_signalOverlappedEvent(&self->base, self->pendingOverlapped, STATUS_SUCCESS, 0);
		self->pendingOverlapped = NULL;
		if (self->connectPending) {
			self->connectPending = false;
			pthread_cond_broadcast(&self->connectCv);
		}
	}
	pthread_mutex_unlock(&self->connectMutex);
	return fd;
}

// Returns a new reference to a connectable instance, or NULL with *error set.
static NamedPipeInstance *acquireConnectableInstance(NamedPipeState *state, DWORD desiredAccess, DWORD *error) {
	if (!state) {
		*error = ERROR_FILE_NOT_FOUND;
		return NULL;
	}
	pthread_mutex_lock(&state->mutex);
	for (size_t i = 0; i < state->instances.len; i++) {
		NamedPipeInstance *inst = state->instances.data[i];
		if (!inst) {
			continue;
		}
		if (NamedPipeInstance_canAcceptClient(inst, desiredAccess)) {
			ObjectBase_ref(&inst->base.base.base);
			pthread_mutex_unlock(&state->mutex);
			return inst;
		}
	}
	pthread_mutex_unlock(&state->mutex);
	*error = ERROR_PIPE_BUSY;
	return NULL;
}

// Does not take ownership of state.
static void openNamedPipeClient(NamedPipeState *state, DWORD dwDesiredAccess,
								LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwFlagsAndAttributes,
								HANDLE *outHandle) {
	DWORD acquireError = ERROR_SUCCESS;
	NamedPipeInstance *instance = acquireConnectableInstance(state, dwDesiredAccess, &acquireError);
	if (!instance) {
		kernel32_setLastError(acquireError);
		*outHandle = INVALID_HANDLE_VALUE;
		return;
	}

	int clientFd = NamedPipeInstance_takeCompanion(instance);
	if (clientFd < 0) {
		kernel32_setLastError(ERROR_PIPE_BUSY);
		*outHandle = INVALID_HANDLE_VALUE;
		ObjectBase_unref(&instance->base.base.base);
		return;
	}

	bool inherit = lpSecurityAttributes && lpSecurityAttributes->bInheritHandle;
	configureInheritability(clientFd, inherit);

	FileObject *clientObj = FileObject_alloc(ObjectType_File, clientFd);
	clientObj->base.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	clientObj->overlapped = (dwFlagsAndAttributes & FILE_FLAG_OVERLAPPED) != 0;

	uint32_t grantedAccess = SYNCHRONIZE;
	switch (instance->accessMode & PIPE_ACCESS_DUPLEX) {
	case PIPE_ACCESS_DUPLEX:
		grantedAccess |= FILE_GENERIC_READ | FILE_GENERIC_WRITE;
		break;
	case PIPE_ACCESS_INBOUND:
		grantedAccess |= FILE_GENERIC_WRITE;
		break;
	case PIPE_ACCESS_OUTBOUND:
		grantedAccess |= FILE_GENERIC_READ;
		break;
	default:
		break;
	}

	uint32_t handleFlags = inherit ? HANDLE_FLAG_INHERIT : 0;
	*outHandle = Handles_alloc(wibo_handles(), &clientObj->base.base, grantedAccess, handleFlags);
	ObjectBase_unref(&instance->base.base.base);
}

bool kernel32_tryCreateFileNamedPipeA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
									  LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
									  DWORD dwFlagsAndAttributes, HANDLE *outHandle) {
	(void)dwShareMode;
	(void)dwCreationDisposition;

	DWORD parseError = ERROR_SUCCESS;
	ParsedPipeName parsed;
	if (!parsePipeName(lpFileName, &parseError, &parsed)) {
		if (parseError != ERROR_SUCCESS) {
			kernel32_setLastError(parseError);
			*outHandle = INVALID_HANDLE_VALUE;
			return true;
		}
		return false;
	}

	NamedPipeState *state = (NamedPipeState *)Namespace_getAs(&wibo_g_namespace, parsed.namespaceKey,
															  parsed.namespaceKeyLen, ObjectType_NamedPipeState);
	if (!state) {
		kernel32_setLastError(ERROR_FILE_NOT_FOUND);
		*outHandle = INVALID_HANDLE_VALUE;
	} else {
		openNamedPipeClient(state, dwDesiredAccess, lpSecurityAttributes, dwFlagsAndAttributes, outHandle);
		ObjectBase_unref(&state->base);
	}
	ParsedPipeName_free(&parsed);
	return true;
}

BOOL WINAPI CreatePipe(PHANDLE hReadPipe, PHANDLE hWritePipe, LPSECURITY_ATTRIBUTES lpPipeAttributes, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreatePipe(%p, %p, %p, %u)\n", hReadPipe, hWritePipe, lpPipeAttributes, nSize);
	if (!hReadPipe || !hWritePipe) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*hReadPipe = NO_HANDLE;
	*hWritePipe = NO_HANDLE;

	int pipeFds[2];
	if (pipe(pipeFds) != 0) {
		kernel32_setLastErrorFromErrno();
		return FALSE;
	}

	bool inheritHandles = lpPipeAttributes && lpPipeAttributes->bInheritHandle;
	configureInheritability(pipeFds[0], inheritHandles);
	configureInheritability(pipeFds[1], inheritHandles);

#ifdef __linux__
	if (nSize != 0) {
		// Best-effort adjustment; ignore failures as recommended by docs.
		fcntl(pipeFds[0], F_SETPIPE_SZ, (int)nSize);
		fcntl(pipeFds[1], F_SETPIPE_SZ, (int)nSize);
	}
#endif

	FileObject *readObj = FileObject_alloc(ObjectType_File, pipeFds[0]);
	readObj->base.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	FileObject *writeObj = FileObject_alloc(ObjectType_File, pipeFds[1]);
	writeObj->base.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	*hReadPipe = Handles_alloc(wibo_handles(), &readObj->base.base, FILE_GENERIC_READ, 0);
	*hWritePipe = Handles_alloc(wibo_handles(), &writeObj->base.base, FILE_GENERIC_WRITE, 0);
	return TRUE;
}

// Releases the state reference and any instance reservation, closing fds the caller still owns.
static HANDLE createNamedPipeFail(int serverFd, int companionFd, NamedPipeState *state, DWORD err) {
	if (serverFd >= 0) {
		close(serverFd);
	}
	if (companionFd >= 0) {
		close(companionFd);
	}
	NamedPipeState_releaseInstance(state);
	ObjectBase_unref(&state->base);
	kernel32_setLastError(err);
	return INVALID_HANDLE_VALUE;
}

static HANDLE createNamedPipe(ParsedPipeName *parsed, DWORD dwOpenMode, DWORD dwPipeMode, DWORD nMaxInstances,
							  DWORD nOutBufferSize, DWORD nInBufferSize, DWORD nDefaultTimeOut,
							  LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
	const DWORD kAllowedOpenFlags = PIPE_ACCESS_DUPLEX | WRITE_DAC | WRITE_OWNER | ACCESS_SYSTEM_SECURITY |
									FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OVERLAPPED;
	if ((dwOpenMode & ~kAllowedOpenFlags) != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}

	DWORD accessMode = dwOpenMode & PIPE_ACCESS_DUPLEX;
	if (accessMode != PIPE_ACCESS_DUPLEX && accessMode != PIPE_ACCESS_INBOUND && accessMode != PIPE_ACCESS_OUTBOUND) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}

	const bool firstInstanceFlag = (dwOpenMode & FILE_FLAG_FIRST_PIPE_INSTANCE) != 0;
	const bool inheritHandles = lpSecurityAttributes && lpSecurityAttributes->bInheritHandle;
	const bool overlapped = (dwOpenMode & FILE_FLAG_OVERLAPPED) != 0;

	const DWORD kAllowedPipeModeFlags =
		PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS;
	if ((dwPipeMode & ~kAllowedPipeModeFlags) != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}
	if ((dwPipeMode & PIPE_READMODE_MESSAGE) != 0 && (dwPipeMode & PIPE_TYPE_MESSAGE) == 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return INVALID_HANDLE_VALUE;
	}

	DWORD pipeType = (dwPipeMode & PIPE_TYPE_MESSAGE) != 0 ? PIPE_TYPE_MESSAGE : PIPE_TYPE_BYTE;
	DWORD normalizedMaxInstances = normalizeMaxInstances(nMaxInstances);

	bool isNewState = false;
	NamedPipeState *state =
		(NamedPipeState *)Namespace_getOrCreate(&wibo_g_namespace, parsed->namespaceKey, parsed->namespaceKeyLen,
												NamedPipeState_makeForName, parsed, &isNewState);
	if (!state) {
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}

	DWORD reserveError = ERROR_SUCCESS;
	if (!NamedPipeState_reserveInstance(state, accessMode, pipeType, nDefaultTimeOut, normalizedMaxInstances,
										firstInstanceFlag, isNewState, &reserveError)) {
		kernel32_setLastError(reserveError);
		ObjectBase_unref(&state->base);
		return INVALID_HANDLE_VALUE;
	}

	int serverFd = -1;
	int companionFd = -1;
	int fds[2] = {-1, -1};
	if (accessMode == PIPE_ACCESS_DUPLEX) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
			int savedErrno = errno;
			return createNamedPipeFail(-1, -1, state, wibo_winErrorFromErrno(savedErrno));
		}
		serverFd = fds[0];
		companionFd = fds[1];
	} else {
		if (pipe(fds) != 0) {
			int savedErrno = errno;
			return createNamedPipeFail(-1, -1, state, wibo_winErrorFromErrno(savedErrno));
		}
		if (accessMode == PIPE_ACCESS_INBOUND) {
			serverFd = fds[0];
			companionFd = fds[1];
#ifdef __linux__
			if (nInBufferSize != 0) {
				fcntl(serverFd, F_SETPIPE_SZ, (int)nInBufferSize);
			}
#endif
		} else {
			serverFd = fds[1];
			companionFd = fds[0];
#ifdef __linux__
			if (nOutBufferSize != 0) {
				fcntl(serverFd, F_SETPIPE_SZ, (int)nOutBufferSize);
			}
#endif
		}
	}

	configureInheritability(serverFd, inheritHandles);
	if (companionFd >= 0) {
		configureInheritability(companionFd, inheritHandles);
	}

	// The instance takes ownership of the state reference from here on.
	NamedPipeInstance *pipeObj = NamedPipeInstance_alloc(serverFd, state, companionFd, accessMode, dwPipeMode);

	pipeObj->base.base.shareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
	pipeObj->base.overlapped = overlapped;

	uint32_t grantedAccess = SYNCHRONIZE;
	switch (accessMode) {
	case PIPE_ACCESS_DUPLEX:
		grantedAccess |= FILE_GENERIC_READ | FILE_GENERIC_WRITE;
		break;
	case PIPE_ACCESS_INBOUND:
		grantedAccess |= FILE_GENERIC_READ;
		break;
	case PIPE_ACCESS_OUTBOUND:
		grantedAccess |= FILE_GENERIC_WRITE;
		break;
	default:
		break;
	}

	uint32_t handleFlags = inheritHandles ? HANDLE_FLAG_INHERIT : 0;
	return Handles_alloc(wibo_handles(), &pipeObj->base.base.base, grantedAccess, handleFlags);
}

HANDLE WINAPI CreateNamedPipeA(LPCSTR lpName, DWORD dwOpenMode, DWORD dwPipeMode, DWORD nMaxInstances,
							   DWORD nOutBufferSize, DWORD nInBufferSize, DWORD nDefaultTimeOut,
							   LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateNamedPipeA(%s, 0x%08x, 0x%08x, %u, %u, %u, %u, %p)\n", lpName ? lpName : "(null)", dwOpenMode,
			  dwPipeMode, nMaxInstances, nOutBufferSize, nInBufferSize, nDefaultTimeOut, lpSecurityAttributes);

	DWORD parseError = ERROR_SUCCESS;
	ParsedPipeName parsed;
	if (!parsePipeName(lpName, &parseError, &parsed)) {
		kernel32_setLastError((parseError == ERROR_SUCCESS) ? ERROR_INVALID_NAME : parseError);
		return INVALID_HANDLE_VALUE;
	}

	HANDLE result = createNamedPipe(&parsed, dwOpenMode, dwPipeMode, nMaxInstances, nOutBufferSize, nInBufferSize,
									nDefaultTimeOut, lpSecurityAttributes);
	ParsedPipeName_free(&parsed);
	return result;
}

// Returns TRUE when the pipe is connected; otherwise sets the last error (or reports ERROR_IO_PENDING for overlapped).
static BOOL connectNamedPipeInstance(NamedPipeInstance *pipe, LPOVERLAPPED lpOverlapped) {
	const bool isOverlappedHandle = pipe->base.overlapped;
	if (isOverlappedHandle && lpOverlapped == NULL) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	pthread_mutex_lock(&pipe->connectMutex);
	DWORD error = ERROR_SUCCESS;
	BOOL result = FALSE;
	if (pipe->clientConnected) {
		error = ERROR_PIPE_CONNECTED;
	} else if (pipe->companionFd < 0) {
		error = ERROR_PIPE_BUSY;
	} else if (pipe->connectPending) {
		error = ERROR_PIPE_LISTENING;
	} else if ((pipe->pipeMode & PIPE_NOWAIT) != 0) {
		error = ERROR_PIPE_LISTENING;
	} else if (isOverlappedHandle) {
		pipe->connectPending = true;
		pipe->pendingOverlapped = lpOverlapped;
		lpOverlapped->Internal = STATUS_PENDING;
		lpOverlapped->InternalHigh = 0;
		kernel32_detail_resetOverlappedEvent(lpOverlapped);
		pthread_mutex_unlock(&pipe->connectMutex);
		kernel32_setLastError(ERROR_IO_PENDING);
		return FALSE;
	} else {
		pipe->connectPending = true;
		while (!(pipe->clientConnected || pipe->companionFd < 0)) {
			pthread_cond_wait(&pipe->connectCv, &pipe->connectMutex);
		}
		pipe->connectPending = false;
		if (pipe->clientConnected) {
			result = TRUE;
		} else {
			error = ERROR_NO_DATA;
		}
	}
	pthread_mutex_unlock(&pipe->connectMutex);
	if (!result) {
		kernel32_setLastError(error);
	}
	return result;
}

BOOL WINAPI ConnectNamedPipe(HANDLE hNamedPipe, LPOVERLAPPED lpOverlapped) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ConnectNamedPipe(%p, %p)\n", hNamedPipe, lpOverlapped);

	NamedPipeInstance *pipe =
		(NamedPipeInstance *)Handles_getAs(wibo_handles(), hNamedPipe, ObjectType_NamedPipe, NULL);
	if (!pipe) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	BOOL result = connectNamedPipeInstance(pipe, lpOverlapped);
	ObjectBase_unref(&pipe->base.base.base);
	return result;
}
