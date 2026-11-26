#include "processes.h"

#include "common.h"
#include "handles.h"
#include "kernel32/internal.h"

#include <errno.h>
#include <linux/sched.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
	ProcessManagerImpl base;
	pthread_rwlock_t m;
	atomic_bool mRunning;
	pthread_t mThread;
	int mEpollFd;
	int mWakeFd;
	hashmap *mReg;
} LinuxProcessManager;

static DWORD decodeExitCode(const siginfo_t *si) {
	switch (si->si_code) {
	case CLD_EXITED:
		return (DWORD)si->si_status;
	case CLD_KILLED:
	case CLD_DUMPED:
		return 0xC0000000u | (DWORD)si->si_status;
	default:
		return 0;
	}
}

static bool epollAdd(int epollFd, int fd) {
	struct epoll_event ev = {0};
	ev.events = EPOLLIN;
	ev.data.fd = fd;
	if (epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		perror("epoll_ctl");
		return false;
	}
	return true;
}

static void linuxWake(const LinuxProcessManager *self) {
	if (self->mWakeFd < 0) {
		return;
	}
	uint64_t n = 1;
	ssize_t r = write(self->mWakeFd, &n, sizeof(n));
	(void)r;
}

static void unrefRegistered(const void *key, size_t keylen, void *value, void *ctx) {
	(void)key;
	(void)keylen;
	(void)ctx;
	ObjectBase_unref(&((ProcessObject *)value)->base.base);
}

static void checkPidfd(LinuxProcessManager *self, int pidfd) {
	DEBUG_LOG("ProcessManager: checking pidfd %d\n", pidfd);

	siginfo_t si = {0};
	si.si_code = CLD_DUMPED;
	if (pidfd >= 0) {
		int rc = waitid(P_PIDFD, pidfd, &si, WEXITED | WNOHANG);
		if (rc < 0) {
			perror("waitid");
		} else if (rc == 0 && si.si_pid == 0) {
			return;
		}
		epoll_ctl(self->mEpollFd, EPOLL_CTL_DEL, pidfd, NULL);
	}

	DEBUG_LOG("ProcessManager: pidfd %d exited: code=%d status=%d\n", pidfd, si.si_code, si.si_status);

	ProcessObject *po = NULL;
	pthread_rwlock_wrlock(&self->m);
	void *value = NULL;
	if (hashmap_remove(self->mReg, &pidfd, sizeof(pidfd), &value)) {
		po = value;
	}
	pthread_rwlock_unlock(&self->m);

	close(pidfd);
	if (!po) {
		return;
	}
	pthread_mutex_lock(&po->base.m);
	po->base.signaled = true;
	po->pidfd = -1;
	if (!po->forcedExitCode) {
		po->exitCode = decodeExitCode(&si);
	}
	pthread_mutex_unlock(&po->base.m);
	pthread_cond_broadcast(&po->base.cv);
	WaitableObject_notifyWaiters(&po->base, false);
	ObjectBase_unref(&po->base.base);
}

static void runLoop(LinuxProcessManager *self) {
	enum { kMaxEvents = 64 };
	struct epoll_event events[kMaxEvents] = {0};
	while (atomic_load_explicit(&self->mRunning, memory_order_acquire)) {
		int n = epoll_wait(self->mEpollFd, events, kMaxEvents, -1);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			perror("epoll_wait");
			break;
		}
		for (int i = 0; i < n; ++i) {
			const struct epoll_event *ev = &events[i];
			if (ev->data.fd == self->mWakeFd) {
				uint64_t value;
				while (read(self->mWakeFd, &value, sizeof(value)) == sizeof(value)) {
				}
				continue;
			}
			checkPidfd(self, ev->data.fd);
		}
	}
}

static void *runLoopThread(void *arg) {
	runLoop((LinuxProcessManager *)arg);
	return NULL;
}

static bool linuxInit(ProcessManagerImpl *base) {
	LinuxProcessManager *self = (LinuxProcessManager *)base;
	if (atomic_load_explicit(&self->mRunning, memory_order_acquire)) {
		return true;
	}

	self->mEpollFd = epoll_create1(EPOLL_CLOEXEC);
	if (self->mEpollFd < 0) {
		perror("epoll_create1");
		return false;
	}

	self->mWakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (self->mWakeFd < 0) {
		perror("eventfd");
		close(self->mEpollFd);
		self->mEpollFd = -1;
		return false;
	}

	if (!epollAdd(self->mEpollFd, self->mWakeFd)) {
		close(self->mWakeFd);
		self->mWakeFd = -1;
		close(self->mEpollFd);
		self->mEpollFd = -1;
		return false;
	}

	atomic_store_explicit(&self->mRunning, true, memory_order_release);
	int rc = pthread_create(&self->mThread, NULL, runLoopThread, self);
	if (rc != 0) {
		fprintf(stderr, "pthread_create: %s\n", strerror(rc));
		abort();
	}
	DEBUG_LOG("ProcessManager (Linux) initialized\n");
	return true;
}

static void linuxShutdown(ProcessManagerImpl *base) {
	LinuxProcessManager *self = (LinuxProcessManager *)base;
	if (!atomic_exchange_explicit(&self->mRunning, false, memory_order_acq_rel)) {
		return;
	}
	linuxWake(self);
	pthread_join(self->mThread, NULL);

	pthread_rwlock_wrlock(&self->m);
	hashmap_foreach(self->mReg, unrefRegistered, NULL);
	hashmap_free(self->mReg);
	self->mReg = hashmap_create();
	pthread_rwlock_unlock(&self->m);

	if (self->mWakeFd >= 0) {
		close(self->mWakeFd);
		self->mWakeFd = -1;
	}
	if (self->mEpollFd >= 0) {
		close(self->mEpollFd);
		self->mEpollFd = -1;
	}
}

static bool linuxAddProcess(ProcessManagerImpl *base, ProcessObject *po) {
	LinuxProcessManager *self = (LinuxProcessManager *)base;
	if (!po) {
		return false;
	}
	pid_t pid;
	int pidfd;
	pthread_mutex_lock(&po->base.m);
	pid = po->pid;
	pidfd = po->pidfd;
	if (pidfd < 0) {
		pthread_mutex_unlock(&po->base.m);
		ObjectBase_unref(&po->base.base);
		return false;
	}
	if (!epollAdd(self->mEpollFd, pidfd)) {
		close(pidfd);
		po->pidfd = -1;
		pthread_mutex_unlock(&po->base.m);
		ObjectBase_unref(&po->base.base);
		return false;
	}
	pthread_mutex_unlock(&po->base.m);

	pthread_rwlock_wrlock(&self->m);
	hashmap_put(self->mReg, &pidfd, sizeof(pidfd), po);
	pthread_rwlock_unlock(&self->m);
	DEBUG_LOG("ProcessManager: registered pid %d with pidfd %d\n", pid, pidfd);
	linuxWake(self);
	return true;
}

static bool linuxRunning(const ProcessManagerImpl *base) {
	const LinuxProcessManager *self = (const LinuxProcessManager *)base;
	return atomic_load_explicit(&self->mRunning, memory_order_acquire);
}

static void linuxDestroy(ProcessManagerImpl *base) {
	LinuxProcessManager *self = (LinuxProcessManager *)base;
	linuxShutdown(base);
	pthread_rwlock_destroy(&self->m);
	hashmap_free(self->mReg);
	free(self);
}

ProcessManagerImpl *wibo_detail_createProcessManagerImpl(void) {
	LinuxProcessManager *self = xcalloc(1, sizeof(*self));
	self->base.init = linuxInit;
	self->base.shutdown = linuxShutdown;
	self->base.addProcess = linuxAddProcess;
	self->base.running = linuxRunning;
	self->base.destroy = linuxDestroy;
	pthread_rwlock_init(&self->m, NULL);
	self->mEpollFd = -1;
	self->mWakeFd = -1;
	self->mReg = hashmap_create();
	return &self->base;
}

int wibo_detail_spawnProcess(char *const argv[], char *const envp[], SpawnProcessInfo *info) {
	pid_t pid = (pid_t)syscall(SYS_clone, CLONE_PIDFD, NULL, &info->pidfd);
	if (pid < 0) {
		info->pidfd = -1;
		int err = errno;
		perror("clone");
		return err;
	}
	if (pid == 0) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) {
			perror("prctl(PR_SET_PDEATHSIG)");
		}
		execve("/proc/self/exe", argv, envp);
		perror("execve");
		_Exit(127);
	}
	info->pid = pid;
	return 0;
}
