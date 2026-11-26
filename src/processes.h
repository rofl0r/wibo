#pragma once

#include "kernel32/internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>

typedef struct ProcessManagerImpl ProcessManagerImpl;

// Backends embed this as their first member `base` and receive it as `self`.
struct ProcessManagerImpl {
	bool (*init)(ProcessManagerImpl *self);
	void (*shutdown)(ProcessManagerImpl *self);
	// Takes ownership of one reference to po.
	bool (*addProcess)(ProcessManagerImpl *self, ProcessObject *po);
	bool (*running)(const ProcessManagerImpl *self);
	void (*destroy)(ProcessManagerImpl *self);
};

// pid must be set to -1 and pidfd to -1 before calling wibo_detail_spawnProcess.
typedef struct {
	pid_t pid;
	int pidfd;
} SpawnProcessInfo;

// Returns an owned implementation; release it through its destroy callback.
ProcessManagerImpl *wibo_detail_createProcessManagerImpl(void);
int wibo_detail_spawnProcess(char *const argv[], char *const envp[], SpawnProcessInfo *info);

typedef struct {
	ProcessManagerImpl *mImpl;
} ProcessManager;

bool ProcessManager_init(ProcessManager *self);
void ProcessManager_shutdown(ProcessManager *self);
// Takes ownership of one reference to po.
bool ProcessManager_addProcess(ProcessManager *self, ProcessObject *po);
bool ProcessManager_running(const ProcessManager *self);

ProcessManager *wibo_processes(void);

// Returns a malloc'd path, or NULL if the command cannot be resolved.
char *wibo_resolveExecutable(const char *command, bool searchPath);
// On success, *pinOut receives one owned reference to the new process object.
int wibo_spawnWithCommandLine(const char *applicationName, const char *commandLine, ProcessObject **pinOut);
int wibo_spawnWithArgv(const char *applicationName, const StringVec *argv, ProcessObject **pinOut);
// Each string and the vector storage are malloc'd; release with StringVec_free.
StringVec wibo_splitCommandLine(const char *commandLine);

static inline void StringVec_free(StringVec *v) {
	for (size_t i = 0; i < v->len; i++) {
		free(v->data[i]);
	}
	vec_free(v);
}

DWORD wibo_getThreadId(void);
