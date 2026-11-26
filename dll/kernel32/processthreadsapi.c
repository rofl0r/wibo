#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "processthreadsapi.h"

#include "common.h"
#include "context.h"
#include "cutil.h"
#include "errors.h"
#include "files.h"
#include "handles.h"
#include "internal.h"
#include "kernel32.h"
#include "kernel32_trampolines.h"
#include "modules.h"
#include "processes.h"
#include "strutil.h"
#include "timeutil.h"
#include "tls.h"
#include "types.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define STARTF_USESHOWWINDOW 0x00000001
#define STARTF_USESTDHANDLES 0x00000100
#define SW_SHOWNORMAL 1
#define CREATE_SUSPENDED 0x00000004
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x00010000

static DWORD_PTR g_processAffinityMask = 0;
static bool g_processAffinityMaskInitialized = false;

static const FILETIME kDefaultThreadFileTime = {(DWORD)(UNIX_TIME_ZERO & 0xFFFFFFFFULL),
												(DWORD)(UNIX_TIME_ZERO >> 32)};

static FILETIME fileTimeFromTimeval(const struct timeval *value) {
	uint64_t total = 0;
	if (value->tv_sec > 0 || value->tv_usec > 0) {
		total = (uint64_t)value->tv_sec * 10000000ULL + (uint64_t)value->tv_usec * 10ULL;
	}
	return fileTimeFromDuration(total);
}

static FILETIME fileTimeFromTimespec(const struct timespec *value) {
	uint64_t total = 0;
	if (value->tv_sec > 0 || value->tv_nsec > 0) {
		total = (uint64_t)value->tv_sec * 10000000ULL + (uint64_t)value->tv_nsec / 100ULL;
	}
	return fileTimeFromDuration(total);
}

static DWORD_PTR computeSystemAffinityMask(void) {
	long reported = sysconf(_SC_NPROCESSORS_ONLN);
	if (reported <= 0) {
		reported = 1;
	}
	const unsigned int bitCount = 8 * sizeof(DWORD_PTR);
	const unsigned int usable = (unsigned int)reported;
	if (usable >= bitCount) {
		return (DWORD_PTR)~(DWORD_PTR)0;
	}
	return ((DWORD_PTR)1 << usable) - 1;
}

static void populateStartupInfoA(LPSTARTUPINFOA info) {
	if (!info) {
		return;
	}
	memset(info, 0, sizeof(*info));
	info->cb = sizeof(*info);
	info->dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	info->wShowWindow = SW_SHOWNORMAL;
	info->cbReserved2 = 0;
	info->lpReserved2 = GUEST_NULL;
	info->hStdInput = files_getStdHandle(STD_INPUT_HANDLE);
	info->hStdOutput = files_getStdHandle(STD_OUTPUT_HANDLE);
	info->hStdError = files_getStdHandle(STD_ERROR_HANDLE);
}

static void populateStartupInfoW(LPSTARTUPINFOW info) {
	if (!info) {
		return;
	}
	memset(info, 0, sizeof(*info));
	info->cb = sizeof(*info);
	info->dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
	info->wShowWindow = SW_SHOWNORMAL;
	info->cbReserved2 = 0;
	info->lpReserved2 = GUEST_NULL;
	info->hStdInput = files_getStdHandle(STD_INPUT_HANDLE);
	info->hStdOutput = files_getStdHandle(STD_OUTPUT_HANDLE);
	info->hStdError = files_getStdHandle(STD_ERROR_HANDLE);
}

static __thread ThreadObject *g_currentThreadObject = NULL;

typedef struct {
	ThreadObject *obj;
	LPTHREAD_START_ROUTINE entry;
	LPVOID userData;
} ThreadStartData;

static void threadCleanup(ThreadObject *obj) {
	if (!obj) {
		return;
	}
	pthread_mutex_lock(&obj->base.m);
	obj->base.signaled = true;
	// Exit code set before pthread_exit
	pthread_mutex_unlock(&obj->base.m);
	g_currentThreadObject = NULL;
	wibo_notifyDllThreadDetach();
	wibo_uninstallTebForCurrentThread();
	// TODO: mark mutexes owned by this thread as abandoned
	pthread_cond_broadcast(&obj->base.cv);
	WaitableObject_notifyWaiters(&obj->base, false);
	ObjectBase_unref(&obj->base.base);
}

static void *threadTrampoline(void *param) {
	// We ref'd the ThreadObject when constructing ThreadStartData,
	// so we need to deref it when done. (Either normal exit or via pthread_cleanup)
	ThreadStartData *dataPtr = (ThreadStartData *)param;
	ThreadStartData data = *dataPtr;
	free(dataPtr);

	g_currentThreadObject = data.obj;

	// Install TIB
	TEB *threadTib = wibo_allocateTib();
	wibo_initializeTibStackInfo(threadTib);
	if (!wibo_installTibForCurrentThread(threadTib)) {
		fprintf(stderr, "!!! Failed to install TIB for new thread\n");
		wibo_destroyTib(threadTib);
		threadTib = NULL;
	}

	// Wait until resumed (if suspended at start)
	pthread_mutex_lock(&data.obj->base.m);
	data.obj->tib = threadTib;
	if (data.obj->suspendCount) {
		DEBUG_LOG("Thread is suspended at start; waiting...\n");
		while (data.obj->suspendCount != 0) {
			pthread_cond_wait(&data.obj->base.cv, &data.obj->base.m);
		}
	}
	pthread_mutex_unlock(&data.obj->base.m);

	wibo_notifyDllThreadAttach();
	DEBUG_LOG("Calling thread entry %p with userData %p\n", (void *)data.entry, data.userData);
	DWORD result = 0;
	if (data.entry) {
		result = call_LPTHREAD_START_ROUTINE(data.entry, data.userData);
	}
	DEBUG_LOG("Thread exiting with code %u\n", result);
	pthread_mutex_lock(&data.obj->base.m);
	data.obj->exitCode = result;
	pthread_mutex_unlock(&data.obj->base.m);
	threadCleanup(data.obj);
	return NULL;
}

// Returns a malloc'd UTF-8 copy, or NULL for a NULL or empty input.
static char *wideStringToOptionalUtf8(LPCWSTR src) {
	if (!src) {
		return NULL;
	}
	char *utf8 = wideStringToString(src, -1);
	if (*utf8 == '\0') {
		free(utf8);
		return NULL;
	}
	return utf8;
}

BOOL WINAPI IsProcessorFeaturePresent(DWORD ProcessorFeature) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsProcessorFeaturePresent(%u)\n", ProcessorFeature);
	if (ProcessorFeature == 0) { // PF_FLOATING_POINT_PRECISION_ERRATA
		return FALSE;
	}
	if (ProcessorFeature == 1) { // PF_FLOATING_POINT_EMULATED
		return FALSE;
	}
	if (ProcessorFeature == 10) { // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
		return TRUE;
	}
	if (ProcessorFeature == 23) { // PF_FASTFAIL_AVAILABLE (__fastfail)
		return FALSE;
	}
	DEBUG_LOG("  IsProcessorFeaturePresent: unknown feature %u, returning TRUE\n", ProcessorFeature);
	return TRUE;
}

HANDLE WINAPI GetCurrentProcess() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCurrentProcess() -> %d\n", kPseudoCurrentProcessHandleValue);
	return kPseudoCurrentProcessHandleValue;
}

DWORD WINAPI GetCurrentProcessId() {
	HOST_CONTEXT_GUARD();
	DWORD pid = (DWORD)getpid();
	DEBUG_LOG("GetCurrentProcessId() -> %u\n", pid);
	return pid;
}

DWORD WINAPI GetCurrentThreadId() {
	HOST_CONTEXT_GUARD();
	DWORD threadId = wibo_getThreadId();
	DEBUG_LOG("GetCurrentThreadId() -> %u\n", threadId);
	return threadId;
}

HANDLE WINAPI GetCurrentThread() {
	HOST_CONTEXT_GUARD();
	HANDLE pseudoHandle = kPseudoCurrentThreadHandleValue;
	DEBUG_LOG("GetCurrentThread() -> %p\n", pseudoHandle);
	return pseudoHandle;
}

BOOL WINAPI GetProcessAffinityMask(HANDLE hProcess, PDWORD_PTR lpProcessAffinityMask, PDWORD_PTR lpSystemAffinityMask) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetProcessAffinityMask(%p, %p, %p)\n", hProcess, lpProcessAffinityMask, lpSystemAffinityMask);
	if (!lpProcessAffinityMask || !lpSystemAffinityMask) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (!kernel32_isPseudoCurrentProcessHandle(hProcess)) {
		ObjectBase *obj = Handles_getAs(wibo_handles(), hProcess, ObjectType_Process, NULL);
		if (!obj) {
			kernel32_setLastError(ERROR_INVALID_HANDLE);
			return 0;
		}
		ObjectBase_unref(obj);
	}

	DWORD_PTR systemMask = computeSystemAffinityMask();
	if (!g_processAffinityMaskInitialized) {
		g_processAffinityMask = systemMask;
		g_processAffinityMaskInitialized = true;
	}
	DWORD_PTR processMask = g_processAffinityMask & systemMask;
	if (processMask == 0) {
		processMask = systemMask == 0 ? 1 : systemMask;
	}

	*lpProcessAffinityMask = processMask;
	*lpSystemAffinityMask = systemMask == 0 ? 1 : systemMask;
	return TRUE;
}

BOOL WINAPI SetProcessAffinityMask(HANDLE hProcess, DWORD_PTR dwProcessAffinityMask) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetProcessAffinityMask(%p, 0x%lx)\n", hProcess, (unsigned long)dwProcessAffinityMask);
	if (dwProcessAffinityMask == 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (!kernel32_isPseudoCurrentProcessHandle(hProcess)) {
		ObjectBase *obj = Handles_getAs(wibo_handles(), hProcess, ObjectType_Process, NULL);
		if (!obj) {
			kernel32_setLastError(ERROR_INVALID_HANDLE);
			return 0;
		}
		ObjectBase_unref(obj);
	}

	DWORD_PTR systemMask = computeSystemAffinityMask();
	if ((dwProcessAffinityMask & systemMask) == 0 || (dwProcessAffinityMask & ~systemMask) != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	g_processAffinityMask = dwProcessAffinityMask & systemMask;
	g_processAffinityMaskInitialized = true;
	return TRUE;
}

DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetThreadAffinityMask(%p, 0x%lx)\n", hThread, (unsigned long)dwThreadAffinityMask);
	if (dwThreadAffinityMask == 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	if (!kernel32_isPseudoCurrentThreadHandle(hThread)) {
		ObjectBase *obj = Handles_getAs(wibo_handles(), hThread, ObjectType_Thread, NULL);
		if (!obj) {
			kernel32_setLastError(ERROR_INVALID_HANDLE);
			return 0;
		}
		ObjectBase_unref(obj);
	}

	DWORD_PTR processMask = 0;
	DWORD_PTR systemMask = 0;
	if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
		return 0;
	}
	if ((dwThreadAffinityMask & ~systemMask) != 0 || (dwThreadAffinityMask & processMask) == 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	return processMask;
}

void WINAPI ExitProcess(UINT uExitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ExitProcess(%u)\n", uExitCode);
	kernel32_exitInternal(uExitCode);
}

BOOL WINAPI TerminateProcess(HANDLE hProcess, UINT uExitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("TerminateProcess(%p, %u)\n", hProcess, uExitCode);
	if (kernel32_isPseudoCurrentProcessHandle(hProcess)) {
		kernel32_exitInternal(uExitCode);
	}
	ProcessObject *process = (ProcessObject *)Handles_getAs(wibo_handles(), hProcess, ObjectType_Process, NULL);
	if (!process) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	BOOL result = TRUE;
	pthread_mutex_lock(&process->base.m);
	if (process->base.signaled) {
		pthread_mutex_unlock(&process->base.m);
		ObjectBase_unref(&process->base.base);
		return TRUE;
	}
	int killResult = 0;
#if defined(__linux__)
	if (process->pidfd != -1) {
		if (syscall(SYS_pidfd_send_signal, process->pidfd, SIGKILL, NULL, 0) != 0) {
			killResult = errno;
			DEBUG_LOG("TerminateProcess: pidfd_send_signal(%d) failed: %s\n", process->pidfd, strerror(killResult));
		}
	} else if (kill(process->pid, SIGKILL) != 0) {
		killResult = errno;
		DEBUG_LOG("TerminateProcess: kill(%d) failed: %s\n", process->pid, strerror(killResult));
	}
#else
	if (kill(process->pid, SIGKILL) != 0) {
		killResult = errno;
		DEBUG_LOG("TerminateProcess: kill(%d) failed: %s\n", process->pid, strerror(killResult));
	}
#endif
	if (killResult != 0) {
		switch (killResult) {
		case ESRCH:
		case EPERM:
			kernel32_setLastError(ERROR_ACCESS_DENIED);
			break;
		default:
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			break;
		}
		result = FALSE;
	} else {
		process->exitCode = uExitCode;
		process->forcedExitCode = true;
	}
	pthread_mutex_unlock(&process->base.m);
	ObjectBase_unref(&process->base.base);
	return result;
}

BOOL WINAPI GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetExitCodeProcess(%p, %p)\n", hProcess, lpExitCode);
	if (!lpExitCode) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (kernel32_isPseudoCurrentProcessHandle(hProcess)) {
		*lpExitCode = STILL_ACTIVE;
		return TRUE;
	}
	ProcessObject *process = (ProcessObject *)Handles_getAs(wibo_handles(), hProcess, ObjectType_Process, NULL);
	if (!process) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	DWORD exitCode = STILL_ACTIVE;
	pthread_mutex_lock(&process->base.m);
	if (process->base.signaled) {
		exitCode = process->exitCode;
	}
	pthread_mutex_unlock(&process->base.m);
	ObjectBase_unref(&process->base.base);
	*lpExitCode = exitCode;
	return TRUE;
}

DWORD WINAPI TlsAlloc() {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("TlsAlloc()\n");
	DWORD index = wibo_tls_reserveSlot();
	if (index == kInvalidTlsIndex) {
		kernel32_setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return TLS_OUT_OF_INDEXES;
	}
	kernel32_setLastError(ERROR_SUCCESS);
	return index;
}

BOOL WINAPI TlsFree(DWORD dwTlsIndex) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("TlsFree(%u)\n", dwTlsIndex);
	if (!wibo_tls_releaseSlot(dwTlsIndex)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	kernel32_setLastError(ERROR_SUCCESS);
	return TRUE;
}

LPVOID WINAPI TlsGetValue(DWORD dwTlsIndex) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("TlsGetValue(%u)\n", dwTlsIndex);
	if (!wibo_tls_isSlotAllocated(dwTlsIndex)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	GUEST_PTR result = wibo_tls_getValue(dwTlsIndex);
	kernel32_setLastError(ERROR_SUCCESS);
	return (LPVOID)(uintptr_t)result;
}

BOOL WINAPI TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("TlsSetValue(%u, %p)\n", dwTlsIndex, lpTlsValue);
	if (!wibo_tls_isSlotAllocated(dwTlsIndex)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!wibo_tls_setValue(dwTlsIndex, toGuestPtr(lpTlsValue))) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	kernel32_setLastError(ERROR_SUCCESS);
	return TRUE;
}

DWORD WINAPI ResumeThread(HANDLE hThread) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ResumeThread(%p)\n", hThread);
	// TODO: behavior with current thread handle?
	ThreadObject *obj = (ThreadObject *)Handles_getAs(wibo_handles(), hThread, ObjectType_Thread, NULL);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return (DWORD)-1;
	}
	DWORD previous = 0;
	bool notify = false;
	pthread_mutex_lock(&obj->base.m);
	previous = obj->suspendCount;
	if (obj->suspendCount > 0) {
		obj->suspendCount--;
		if (obj->suspendCount == 0) {
			notify = true;
		}
	}
	pthread_mutex_unlock(&obj->base.m);
	if (notify) {
		pthread_cond_broadcast(&obj->base.cv);
	}
	ObjectBase_unref(&obj->base.base);
	return previous;
}

HRESULT WINAPI SetThreadDescription(HANDLE hThread, LPCWSTR lpThreadDescription) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetThreadDescription(%p, %p)\n", hThread, lpThreadDescription);
	(void)hThread;
	(void)lpThreadDescription;
	return S_OK;
}

HANDLE WINAPI CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
						   LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags,
						   LPDWORD lpThreadId) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateThread(%p, %zu, %p, %p, %u, %p)\n", lpThreadAttributes, dwStackSize, (void *)lpStartAddress,
			  lpParameter, dwCreationFlags, lpThreadId);
	(void)lpThreadAttributes;
	const DWORD SUPPORTED_FLAGS = CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION;
	if ((dwCreationFlags & ~SUPPORTED_FLAGS) != 0) {
		DEBUG_LOG("CreateThread: unsupported creation flags 0x%x\n", dwCreationFlags);
		kernel32_setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}

	ThreadObject *obj = ThreadObject_alloc(pthread_null); // tid set during pthread_create
	if ((dwCreationFlags & CREATE_SUSPENDED) != 0) {
		obj->suspendCount = 1;
	}
	ObjectBase_ref(&obj->base.base); // Increment ref for the new thread to adopt
	ThreadStartData *startData = xmalloc(sizeof(*startData));
	startData->obj = obj;
	startData->entry = lpStartAddress;
	startData->userData = lpParameter;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (dwStackSize != 0) {
#ifdef PTHREAD_STACK_MIN
		if (dwStackSize < (SIZE_T)PTHREAD_STACK_MIN) {
			dwStackSize = (SIZE_T)PTHREAD_STACK_MIN;
		}
#endif
		// TODO: should we just ignore this?
		pthread_attr_setstacksize(&attr, dwStackSize);
	}

	int rc = pthread_create(&obj->thread, &attr, threadTrampoline, startData);
	pthread_attr_destroy(&attr);
	if (rc != 0) {
		// Clean up
		free(startData);
		ObjectBase_unref(&obj->base.base);
		ObjectBase_unref(&obj->base.base);
		kernel32_setLastError(wibo_winErrorFromErrno(rc));
		return INVALID_HANDLE_VALUE;
	}

	if (lpThreadId) {
		*lpThreadId = (DWORD)((uintptr_t)obj->thread & 0xffffffffu);
	}

	return Handles_alloc(wibo_handles(), &obj->base.base, 0 /* TODO */, 0);
}

_Noreturn void WINAPI ExitThread(DWORD dwExitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ExitThread(%u)\n", dwExitCode);
	ThreadObject *obj = g_currentThreadObject;
	pthread_mutex_lock(&obj->base.m);
	obj->exitCode = dwExitCode;
	pthread_mutex_unlock(&obj->base.m);
	// Can't use pthread_cleanup_push/pop because it can't unwind the Windows stack
	// So call the cleanup function directly before pthread_exit
	threadCleanup(obj);
	pthread_exit(NULL);
}

BOOL WINAPI GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetExitCodeThread(%p, %p)\n", hThread, lpExitCode);
	if (!lpExitCode) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (kernel32_isPseudoCurrentThreadHandle(hThread)) {
		*lpExitCode = STILL_ACTIVE;
		return TRUE;
	}
	ThreadObject *obj = (ThreadObject *)Handles_getAs(wibo_handles(), hThread, ObjectType_Thread, NULL);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	pthread_mutex_lock(&obj->base.m);
	*lpExitCode = obj->exitCode;
	pthread_mutex_unlock(&obj->base.m);
	ObjectBase_unref(&obj->base.base);
	return TRUE;
}

BOOL WINAPI SetThreadPriority(HANDLE hThread, int nPriority) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetThreadPriority(%p, %d)\n", hThread, nPriority);
	(void)hThread;
	(void)nPriority;
	return TRUE;
}

int WINAPI GetThreadPriority(HANDLE hThread) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetThreadPriority(%p)\n", hThread);
	(void)hThread;
	return 0;
}

DWORD WINAPI GetPriorityClass(HANDLE hProcess) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetPriorityClass(%p)\n", hProcess);
	(void)hProcess;
	return NORMAL_PRIORITY_CLASS;
}

BOOL WINAPI GetThreadTimes(HANDLE hThread, FILETIME *lpCreationTime, FILETIME *lpExitTime, FILETIME *lpKernelTime,
						   FILETIME *lpUserTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetThreadTimes(%p, %p, %p, %p, %p)\n", hThread, lpCreationTime, lpExitTime, lpKernelTime, lpUserTime);

	if (!lpKernelTime || !lpUserTime) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	if (!kernel32_isPseudoCurrentThreadHandle(hThread)) {
		DEBUG_LOG("GetThreadTimes: unsupported handle %p\n", hThread);
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}

	if (lpCreationTime) {
		*lpCreationTime = kDefaultThreadFileTime;
	}
	if (lpExitTime) {
		lpExitTime->dwLowDateTime = 0;
		lpExitTime->dwHighDateTime = 0;
	}

#ifdef __linux__
	struct rusage usage = {0};
	if (getrusage(RUSAGE_THREAD, &usage) == 0) {
		*lpKernelTime = fileTimeFromTimeval(&usage.ru_stime);
		*lpUserTime = fileTimeFromTimeval(&usage.ru_utime);
		return TRUE;
	}
#endif

	struct timespec cpuTime = {0};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpuTime) == 0) {
		*lpKernelTime = fileTimeFromDuration(0);
		*lpUserTime = fileTimeFromTimespec(&cpuTime);
		return TRUE;
	}

	kernel32_setLastErrorFromErrno();
	*lpKernelTime = fileTimeFromDuration(0);
	*lpUserTime = fileTimeFromDuration(0);
	return FALSE;
}

BOOL WINAPI CreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
						   LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
						   LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
						   LPPROCESS_INFORMATION lpProcessInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateProcessA %s \"%s\" %p %p %d 0x%x %p %s %p %p\n", lpApplicationName ? lpApplicationName : "<null>",
			  lpCommandLine ? lpCommandLine : "<null>", lpProcessAttributes, lpThreadAttributes, bInheritHandles,
			  dwCreationFlags, lpEnvironment, lpCurrentDirectory ? lpCurrentDirectory : "<none>", lpStartupInfo,
			  lpProcessInformation);

	bool useSearchPath = lpApplicationName == NULL;
	const char *commandLine = lpCommandLine ? lpCommandLine : "";
	char *ownedApplication = NULL;
	const char *application = lpApplicationName;
	if (!application) {
		StringVec arguments = wibo_splitCommandLine(commandLine);
		if (arguments.len == 0) {
			StringVec_free(&arguments);
			kernel32_setLastError(ERROR_FILE_NOT_FOUND);
			return FALSE;
		}
		ownedApplication = xstrdup(arguments.data[0]);
		StringVec_free(&arguments);
		application = ownedApplication;
	}

	char *resolved = wibo_resolveExecutable(application, useSearchPath);
	if (!resolved) {
		free(ownedApplication);
		kernel32_setLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}

	ProcessObject *obj = NULL;
	int spawnResult = wibo_spawnWithCommandLine(resolved, commandLine, &obj);
	free(resolved);
	free(ownedApplication);
	if (spawnResult != 0) {
		kernel32_setLastError((spawnResult == ENOENT) ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
		return FALSE;
	}

	if (lpProcessInformation) {
		lpProcessInformation->dwProcessId = (DWORD)obj->pid;
		lpProcessInformation->dwThreadId = (DWORD)obj->pid; // Use the process ID as the thread ID
		ObjectBase_ref(&obj->base.base);
		lpProcessInformation->hProcess = Handles_alloc(wibo_handles(), &obj->base.base, 0 /* TODO: access */, 0);
		// Give hThread a process handle for now
		lpProcessInformation->hThread = Handles_alloc(wibo_handles(), &obj->base.base, 0 /* TODO: access */, 0);
	} else {
		ObjectBase_unref(&obj->base.base);
	}
	(void)lpProcessAttributes;
	(void)lpThreadAttributes;
	(void)bInheritHandles;
	(void)dwCreationFlags;
	(void)lpEnvironment;
	(void)lpCurrentDirectory;
	(void)lpStartupInfo;
	return TRUE;
}

BOOL WINAPI CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
						   LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
						   LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
						   LPPROCESS_INFORMATION lpProcessInformation) {
	HOST_CONTEXT_GUARD();
	char *applicationUtf8 = wideStringToOptionalUtf8(lpApplicationName);
	char *commandUtf8 = wideStringToOptionalUtf8(lpCommandLine);
	char *directoryUtf8 = wideStringToOptionalUtf8(lpCurrentDirectory);
	DEBUG_LOG("CreateProcessW %s \"%s\" %p %p %d 0x%x %p %s %p %p\n", applicationUtf8 ? applicationUtf8 : "<null>",
			  commandUtf8 ? commandUtf8 : "<null>", lpProcessAttributes, lpThreadAttributes, bInheritHandles,
			  dwCreationFlags, lpEnvironment, directoryUtf8 ? directoryUtf8 : "<none>", lpStartupInfo,
			  lpProcessInformation);
	BOOL result = CreateProcessA(applicationUtf8, commandUtf8, lpProcessAttributes, lpThreadAttributes, bInheritHandles,
								 dwCreationFlags, lpEnvironment, directoryUtf8, NULL /* TODO: lpStartupInfo */,
								 lpProcessInformation);
	free(applicationUtf8);
	free(commandUtf8);
	free(directoryUtf8);
	return result;
}

void WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetStartupInfoA(%p)\n", lpStartupInfo);
	populateStartupInfoA(lpStartupInfo);
}

void WINAPI GetStartupInfoW(LPSTARTUPINFOW lpStartupInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetStartupInfoW(%p)\n", lpStartupInfo);
	populateStartupInfoW(lpStartupInfo);
}

BOOL WINAPI SetThreadStackGuarantee(PULONG StackSizeInBytes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetThreadStackGuarantee(%p)\n", StackSizeInBytes);
	(void)StackSizeInBytes;
	return TRUE;
}
