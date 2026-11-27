#include "processes.h"

#include "common.h"
#include "handles.h"
#include "kernel32/internal.h"

#include <errno.h>
#define ENUM_DYLD_BOOL
#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

enum { kWakeIdent = 1 };

typedef struct {
	ProcessManagerImpl base;
	pthread_rwlock_t m;
	atomic_bool mRunning;
	pthread_t mThread;
	int mKqueueFd;
	hashmap *mReg;
} DarwinProcessManager;

static DWORD decodeExitStatus(int status) {
	if (WIFEXITED(status)) {
		return (DWORD)WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status)) {
		return 0xC0000000u | (DWORD)WTERMSIG(status);
	}
	return 0;
}

static char *g_executablePath;
static pthread_once_t g_executablePathOnce = PTHREAD_ONCE_INIT;

static void initExecutablePath(void) {
	uint32_t size = 0;
	if (_NSGetExecutablePath(NULL, &size) != 0 && size > 0) {
		char *buffer = xmalloc(size);
		if (_NSGetExecutablePath(buffer, &size) == 0) {
			char *canonical = realpath(buffer, NULL);
			if (canonical) {
				free(buffer);
				g_executablePath = canonical;
			} else {
				g_executablePath = buffer;
			}
		} else {
			free(buffer);
		}
	}
	if (!g_executablePath) {
		g_executablePath = xstrdup("wibo");
	}
}

static const char *executablePath(void) {
	pthread_once(&g_executablePathOnce, initExecutablePath);
	return g_executablePath;
}

static void unrefRegistered(const void *key, size_t keylen, void *value, void *ctx) {
	(void)key;
	(void)keylen;
	(void)ctx;
	ObjectBase_unref(&((ProcessObject *)value)->base.base);
}

static void completeProcess(ProcessObject *po, int status) {
	pthread_mutex_lock(&po->base.m);
	po->base.signaled = true;
	po->pidfd = -1;
	if (!po->forcedExitCode) {
		po->exitCode = decodeExitStatus(status);
	}
	pthread_mutex_unlock(&po->base.m);
	pthread_cond_broadcast(&po->base.cv);
	WaitableObject_notifyWaiters(&po->base, false);
}

static void darwinWake(const DarwinProcessManager *self) {
	if (self->mKqueueFd < 0) {
		return;
	}
	struct kevent kev;
	EV_SET(&kev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
	kevent(self->mKqueueFd, &kev, 1, NULL, 0, NULL);
}

static void handleExit(DarwinProcessManager *self, pid_t pid) {
	void *value = NULL;
	ProcessObject *po = NULL;
	pthread_rwlock_wrlock(&self->m);
	if (hashmap_remove(self->mReg, &pid, sizeof(pid), &value)) {
		po = value;
	}
	pthread_rwlock_unlock(&self->m);

	int status = 0;
	if (!po) {
		// Might be a race with registration; still ensure we reap the child.
		waitpid(pid, &status, WNOHANG);
		return;
	}
	pid_t waited = waitpid(pid, &status, WNOHANG);
	if (waited == 0) {
		// Child still around; block to reap.
		waited = waitpid(pid, &status, 0);
	}
	if (waited < 0) {
		int err = errno;
		DEBUG_LOG("ProcessManager: waitpid(%d) failed: %s\n", pid, strerror(err));
		status = 0;
	}
	completeProcess(po, status);
	ObjectBase_unref(&po->base.base);
}

static void runLoop(DarwinProcessManager *self) {
	enum { kMaxEvents = 64 };
	struct kevent events[kMaxEvents];
	memset(events, 0, sizeof(events));
	while (atomic_load_explicit(&self->mRunning, memory_order_acquire)) {
		int n = kevent(self->mKqueueFd, NULL, 0, events, kMaxEvents, NULL);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("kevent");
			break;
		}
		for (int i = 0; i < n; ++i) {
			const struct kevent *ev = &events[i];
			if (ev->filter == EVFILT_USER) {
				continue;
			}
			if (ev->filter == EVFILT_PROC && (ev->fflags & NOTE_EXIT)) {
				handleExit(self, (pid_t)ev->ident);
			}
		}
	}
}

static void *runLoopThread(void *arg) {
	runLoop((DarwinProcessManager *)arg);
	return NULL;
}

static bool darwinInit(ProcessManagerImpl *base) {
	DarwinProcessManager *self = (DarwinProcessManager *)base;
	if (atomic_load_explicit(&self->mRunning, memory_order_acquire)) {
		return true;
	}

	self->mKqueueFd = kqueue();
	if (self->mKqueueFd < 0) {
		perror("kqueue");
		return false;
	}

	struct kevent kev;
	EV_SET(&kev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
	if (kevent(self->mKqueueFd, &kev, 1, NULL, 0, NULL) < 0) {
		perror("kevent(EV_ADD user)");
		close(self->mKqueueFd);
		self->mKqueueFd = -1;
		return false;
	}

	atomic_store_explicit(&self->mRunning, true, memory_order_release);
	int rc = pthread_create(&self->mThread, NULL, runLoopThread, self);
	if (rc != 0) {
		fprintf(stderr, "pthread_create: %s\n", strerror(rc));
		abort();
	}
	DEBUG_LOG("ProcessManager (Darwin) initialized\n");
	return true;
}

static void darwinShutdown(ProcessManagerImpl *base) {
	DarwinProcessManager *self = (DarwinProcessManager *)base;
	if (!atomic_exchange_explicit(&self->mRunning, false, memory_order_acq_rel)) {
		return;
	}
	darwinWake(self);
	pthread_join(self->mThread, NULL);

	pthread_rwlock_wrlock(&self->m);
	hashmap_foreach(self->mReg, unrefRegistered, NULL);
	hashmap_free(self->mReg);
	self->mReg = hashmap_create();
	pthread_rwlock_unlock(&self->m);

	if (self->mKqueueFd >= 0) {
		close(self->mKqueueFd);
		self->mKqueueFd = -1;
	}
}

static bool darwinAddProcess(ProcessManagerImpl *base, ProcessObject *po) {
	DarwinProcessManager *self = (DarwinProcessManager *)base;
	if (!po) {
		return false;
	}
	pid_t pid;
	pthread_mutex_lock(&po->base.m);
	pid = po->pid;
	pthread_mutex_unlock(&po->base.m);

	struct kevent kev;
	EV_SET(&kev, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT | NOTE_EXITSTATUS, 0, NULL);
	if (kevent(self->mKqueueFd, &kev, 1, NULL, 0, NULL) < 0) {
		int err = errno;
		DEBUG_LOG("ProcessManager: kevent add for pid %d failed: %s\n", pid, strerror(err));
		if (err == ESRCH) {
			int status = 0;
			pid_t waited = waitpid(pid, &status, WNOHANG);
			if (waited <= 0) {
				waitpid(pid, &status, 0);
			}
			completeProcess(po, status);
			ObjectBase_unref(&po->base.base);
			return true;
		}
		ObjectBase_unref(&po->base.base);
		return false;
	}
	pthread_rwlock_wrlock(&self->m);
	hashmap_put(self->mReg, &pid, sizeof(pid), po);
	pthread_rwlock_unlock(&self->m);
	DEBUG_LOG("ProcessManager: registered pid %d\n", pid);
	darwinWake(self);
	return true;
}

static bool darwinRunning(const ProcessManagerImpl *base) {
	const DarwinProcessManager *self = (const DarwinProcessManager *)base;
	return atomic_load_explicit(&self->mRunning, memory_order_acquire);
}

static void darwinDestroy(ProcessManagerImpl *base) {
	DarwinProcessManager *self = (DarwinProcessManager *)base;
	darwinShutdown(base);
	pthread_rwlock_destroy(&self->m);
	hashmap_free(self->mReg);
	free(self);
}

ProcessManagerImpl *wibo_detail_createProcessManagerImpl(void) {
	DarwinProcessManager *self = xcalloc(1, sizeof(*self));
	self->base.init = darwinInit;
	self->base.shutdown = darwinShutdown;
	self->base.addProcess = darwinAddProcess;
	self->base.running = darwinRunning;
	self->base.destroy = darwinDestroy;
	pthread_rwlock_init(&self->m, NULL);
	self->mKqueueFd = -1;
	self->mReg = hashmap_create();
	return &self->base;
}

int wibo_detail_spawnProcess(char *const argv[], char *const envp[], SpawnProcessInfo *info) {
	const char *path = executablePath();
	posix_spawnattr_t attr;
	int rc = posix_spawnattr_init(&attr);
	if (rc != 0) {
		return rc;
	}
	sigset_t mask;
	sigemptyset(&mask);
	rc = posix_spawnattr_setsigmask(&attr, &mask);
	if (rc == 0) {
		rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK);
	}
	pid_t pid = -1;
	if (rc == 0) {
		rc = posix_spawn(&pid, path, NULL, &attr, argv, envp);
	}
	posix_spawnattr_destroy(&attr);
	if (rc != 0) {
		return rc;
	}
	info->pid = pid;
	info->pidfd = -1;
	return 0;
}
