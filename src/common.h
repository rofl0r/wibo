#pragma once

#include "types.h"

#include <assert.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEBUG_LOG(...)                                                                                                 \
	do {                                                                                                               \
		if (wibo_debugEnabled) {                                                                                       \
			wibo_debug_log(__VA_ARGS__);                                                                               \
		}                                                                                                              \
	} while (0)
#ifndef NDEBUG
#define VERBOSE_LOG(...) DEBUG_LOG(__VA_ARGS__)
#else
#define VERBOSE_LOG(...) ((void)0)
#endif

extern __thread TEB *currentThreadTeb;

extern char **wibo_argv;
extern int wibo_argc;
// NULL when no guest executable path has been set.
extern char *wibo_guestExecutablePath;
extern char *wibo_executableName;
extern char *wibo_commandLine;
// NUL-terminated UTF-16 copy of wibo_commandLine.
extern uint16_t *wibo_commandLineW;
extern bool wibo_debugEnabled;
extern unsigned int wibo_debugIndent;
extern int wibo_tibEntryNumber;
extern PEB *wibo_processPeb;

TEB *wibo_allocateTib(void);
void wibo_destroyTib(TEB *tib);
void wibo_initializeTibStackInfo(TEB *tib);
bool wibo_installTibForCurrentThread(TEB *tib);
void wibo_uninstallTebForCurrentThread(void);

void wibo_debug_log(const char *fmt, ...);
