#include "common.h"
#include "bundle.h"
#include "entry.h"
#include "entry_trampolines.h"
#include "files.h"
#include "heap.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "processes.h"
#include "setup.h"
#include "strutil.h"
#include "tls.h"
#include "types.h"
#include "version_info.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef VEC(const char *) ConstStrVec;

char **wibo_argv;
int wibo_argc;
char *wibo_guestExecutablePath;
char *wibo_commandLine;
uint16_t *wibo_commandLineW;
bool wibo_debugEnabled = false;
unsigned int wibo_debugIndent = 0;
int wibo_tibEntryNumber = -1;
PEB *wibo_processPeb = NULL;
__thread TEB *currentThreadTeb = NULL;

void wibo_debug_log(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	if (wibo_debugEnabled) {
		for (size_t i = 0; i < wibo_debugIndent; i++)
			fprintf(stderr, "\t");
		fprintf(stderr, "[thread %x] ", wibo_getThreadId());
		vfprintf(stderr, fmt, args);
		fflush(stderr);
	}

	va_end(args);
}

TEB *wibo_allocateTib(void) {
	TEB *newTib = wibo_heap_guestMalloc(sizeof(TEB), true);
	if (!newTib) {
		return NULL;
	}
	wibo_tls_initializeTib(newTib);
	newTib->Tib.Self = toGuestPtr(newTib);
	newTib->Peb = toGuestPtr(wibo_processPeb);
	return newTib;
}

void wibo_destroyTib(TEB *tibPtr) {
	if (!tibPtr) {
		return;
	}
	wibo_tls_cleanupTib(tibPtr);
	wibo_heap_guestFree(tibPtr);
}

void wibo_initializeTibStackInfo(TEB *tibPtr) {
	if (!tibPtr) {
		return;
	}
	// Allocate a stack for the thread in the guest address space (below 2GB)
	void *guestLimit = NULL;
	void *guestBase = NULL;
	if (!wibo_heap_reserveGuestStack(1 * 1024 * 1024, &guestLimit, &guestBase)) {
		fprintf(stderr, "Failed to reserve guest stack\n");
		abort();
	}
	tibPtr->Tib.StackLimit = toGuestPtr(guestLimit);
	tibPtr->Tib.StackBase = toGuestPtr(guestBase);
	tibPtr->CurrentStackPointer = guestBase;
	DEBUG_LOG("initializeTibStackInfo: using guest stack base=%p limit=%p\n", (void *)(uintptr_t)tibPtr->Tib.StackBase,
			  (void *)(uintptr_t)tibPtr->Tib.StackLimit);
}

bool wibo_installTibForCurrentThread(TEB *tibPtr) {
	if (!tibPtr) {
		return false;
	}
	currentThreadTeb = tibPtr;
	if (!tebThreadSetup(tibPtr)) {
		return false;
	}
	initFpState();
	return true;
}

void wibo_uninstallTebForCurrentThread(void) {
	TEB *teb = currentThreadTeb;
	currentThreadTeb = NULL;
	tebThreadTeardown(teb);
}

static const char *getExeName(const char *argv0) {
	if (!argv0) {
		return "wibo";
	}
	const char *slash = strrchr(argv0, '/');
	return slash ? slash + 1 : argv0;
}

static void printHelp(const char *argv0, bool error) {
	const char *exeName = getExeName(argv0);
	FILE *out = error ? stderr : stdout;
	if (error) {
		fprintf(out, "See '%s --help' for usage information.\n", exeName);
		return;
	}
	fprintf(out, "wibo %s\n\n", kVersionString);
	fprintf(out, "Usage:\n");
	fprintf(out, "  %s [options] <program.exe> [arguments...]\n", exeName);
	fprintf(out, "  %s path [subcommand options] <path> [path...]\n", exeName);
	fprintf(out, "\n");
	fprintf(out, "General Options:\n");
	fprintf(out, "  -h, --help            Show this help message and exit\n");
	fprintf(out, "  -V, --version         Show version information and exit\n");
	fprintf(out, "\n");
	fprintf(out, "Runtime Options:\n");
	fprintf(out, "  -C, --chdir DIR       Change working directory before launching the program\n");
	fprintf(out, "  -D, --debug           Enable debug logging (equivalent to WIBO_DEBUG=1)\n");
	fprintf(out, "      --cmdline STRING  Use STRING as the exact guest command line\n");
	fprintf(out, "                        (includes the program name, e.g. \"test.exe a b c\")\n");
	fprintf(out, "      --                Stop option parsing; following arguments are used\n");
	fprintf(out, "                        verbatim as the guest command line, including the\n");
	fprintf(out, "                        program name\n");
	fprintf(out, "\n");
	fprintf(out, "Subcommands:\n");
	fprintf(out, "  path                  Convert between host and Windows-style paths\n");
	fprintf(out, "                        (see '%s path --help' for details)\n", exeName);
	fprintf(out, "\n");
	fprintf(out, "Examples:\n");
	fprintf(out, "  # Normal usage\n");
	fprintf(out, "  %s path/to/test.exe a b c\n", exeName);
	fprintf(out, "  %s -C path/to test.exe a b c\n", exeName);
	fprintf(out, "\n");
	fprintf(out, "  # Advanced: full control over the guest command line\n");
	fprintf(out, "  %s path/to/test.exe -- test.exe a b c\n", exeName);
	fprintf(out, "  %s --cmdline 'test.exe a b c' path/to/test.exe\n", exeName);
	fprintf(out, "  %s -- test.exe a b c\n", exeName);
}

static void printPathHelp(const char *argv0, bool error) {
	const char *exeName = getExeName(argv0);
	FILE *out = error ? stderr : stdout;
	if (error) {
		fprintf(out, "See '%s path --help' for usage information.\n", exeName);
		return;
	}
	fprintf(out, "Usage:\n");
	fprintf(out, "  %s path [options] <path> [path...]\n", exeName);
	fprintf(out, "\n");
	fprintf(out, "Path Options (exactly one required):\n");
	fprintf(out, "  -u, --unix       Convert Windows paths to host (Unix-style) paths\n");
	fprintf(out, "  -w, --windows    Convert host (Unix-style) paths to Windows paths\n");
	fprintf(out, "\n");
	fprintf(out, "General Options:\n");
	fprintf(out, "  -h, --help       Show this help message and exit\n");
	fprintf(out, "\n");
	fprintf(out, "Examples:\n");
	fprintf(out, "  %s path -u 'Z:\\home\\user'\n", exeName);
	fprintf(out, "  %s path -w /home/user\n", exeName);
}

static int handlePathCommand(int argc, char **argv, const char *argv0) {
	bool convertToUnix = false;
	bool convertToWindows = false;
	ConstStrVec inputs = {0};

	for (int i = 0; i < argc; ++i) {
		const char *arg = argv[i];
		if (strcmp(arg, "-u") == 0 || strcmp(arg, "--unix") == 0) {
			convertToUnix = true;
			continue;
		}
		if (strcmp(arg, "-w") == 0 || strcmp(arg, "--windows") == 0) {
			convertToWindows = true;
			continue;
		}
		if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
			printPathHelp(argv0, false);
			vec_free(&inputs);
			return 0;
		}
		if (arg[0] == '-' && arg[1] != '\0') {
			fprintf(stderr, "Error: unknown option '%s'.\n", arg);
			printPathHelp(argv0, true);
			vec_free(&inputs);
			return 1;
		}
		vec_push(&inputs, arg);
	}

	if (convertToUnix == convertToWindows) {
		if (!convertToUnix) {
			printPathHelp(argv0, false);
		} else {
			fprintf(stderr, "Error: cannot specify both --unix and --windows for path conversion.\n");
			printPathHelp(argv0, true);
		}
		vec_free(&inputs);
		return 1;
	}
	if (inputs.len == 0) {
		fprintf(stderr, "Error: no paths specified for conversion.\n");
		printPathHelp(argv0, true);
		vec_free(&inputs);
		return 1;
	}

	for (size_t i = 0; i < inputs.len; i++) {
		const char *input = inputs.data[i];
		char *converted = convertToUnix ? files_pathFromWindows(input) : files_pathToWindows(input);
		if (!converted) {
			fprintf(stderr, "Error: failed to convert path '%s'.\n", input);
			vec_free(&inputs);
			return 1;
		}
		fprintf(stdout, "%s\n", converted);
		free(converted);
	}

	vec_free(&inputs);
	return 0;
}

// Appends arg to cmd, quoting it when needed and escaping quotes and backslashes that precede them.
static void appendQuotedArg(Buf *cmd, const char *arg) {
	bool needQuotes = strpbrk(arg, "\" \t\n") != NULL;
	if (needQuotes)
		Buf_appendc(cmd, '"');
	size_t backslashes = 0;
	for (const char *p = arg;; p++) {
		char c = *p;
		if (c == '\\') {
			backslashes++;
			continue;
		}

		// Backslashes are doubled *before quotes*
		for (size_t j = 0; j < backslashes; j++) {
			Buf_appendc(cmd, '\\');
			if (c == '\0' || c == '"')
				Buf_appendc(cmd, '\\');
		}
		backslashes = 0;

		if (c == '\0')
			break;
		if (c == '"')
			Buf_appendc(cmd, '\\');
		Buf_appendc(cmd, c);
	}
	if (needQuotes)
		Buf_appendc(cmd, '"');
}

int main(int argc, char **argv) {
	wibo_bundleInit();

	// A bundled executable runs its main program with literal arguments so that wibo's own options cannot
	// shadow the program's. A child spawned by a bundled wibo carries WIBO_BUNDLE_CHILD_ARG, which we strip so
	// the rest parses as a normal command line.
	bool literalArgs = false;
	if (argc >= 2 && strcmp(argv[1], WIBO_BUNDLE_CHILD_ARG) == 0) {
		char **childArgv = xmalloc(argc * sizeof *childArgv);
		childArgv[0] = argv[0];
		for (int i = 2; i < argc; ++i) {
			childArgv[i - 1] = argv[i];
		}
		childArgv[argc - 1] = NULL;
		argc -= 1;
		argv = childArgv;
	} else if (wibo_bundleActive()) {
		char **bundleArgv = xmalloc((argc + 2) * sizeof *bundleArgv);
		bundleArgv[0] = argv[0];
		bundleArgv[1] = wibo_bundleMainPath();
		for (int i = 1; i < argc; ++i) {
			bundleArgv[i + 1] = argv[i];
		}
		bundleArgv[argc + 1] = NULL;
		argc += 1;
		argv = bundleArgv;
		literalArgs = true;
	}

	if (!literalArgs && argc >= 2 && strcmp(argv[1], "path") == 0) {
		return handlePathCommand(argc - 2, argv + 2, argv[0]);
	}

	const char *chdirPath = NULL;
	bool optionDebug = false;
	bool parsingOptions = !literalArgs;
	int programIndex = -1;
	const char *cmdLine = NULL;

	for (int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		if (parsingOptions) {
			if (strcmp(arg, "--") == 0) {
				parsingOptions = false;
				continue;
			}
			if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
				printHelp(argv[0], false);
				return 0;
			}
			if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
				fprintf(stdout, "wibo %s\n", kVersionString);
				return 0;
			}
			if (strncmp(arg, "--cmdline=", 10) == 0) {
				cmdLine = arg + 10;
				continue;
			}
			if (strcmp(arg, "--cmdline") == 0) {
				if (i + 1 >= argc) {
					fprintf(stderr, "Error: '%s' requires a command line argument.\n", arg);
					printHelp(argv[0], true);
					return 1;
				}
				cmdLine = argv[++i];
				continue;
			}
			if (strcmp(arg, "-D") == 0 || strcmp(arg, "--debug") == 0) {
				optionDebug = true;
				continue;
			}
			if (strncmp(arg, "--chdir=", 8) == 0) {
				chdirPath = arg + 8;
				continue;
			}
			if (strcmp(arg, "-C") == 0 || strcmp(arg, "--chdir") == 0) {
				if (i + 1 >= argc) {
					fprintf(stderr, "Error: '%s' requires a directory argument.\n", arg);
					printHelp(argv[0], true);
					return 1;
				}
				chdirPath = argv[++i];
				continue;
			}
			if (strncmp(arg, "-C", 2) == 0 && arg[2] != '\0') {
				chdirPath = arg + 2;
				continue;
			}
			if (arg[0] == '-' && arg[1] != '\0') {
				fprintf(stderr, "Error: unknown option '%s'.\n", arg);
				printHelp(argv[0], true);
				return 1;
			}
		}

		programIndex = i;
		break;
	}

	if (!cmdLine) {
		cmdLine = "";
	}
	bool hasCmdLine = cmdLine[0] != '\0';

	if (programIndex == -1 && !hasCmdLine) {
		if (argc == 1) {
			printHelp(argv[0], false);
			return 0;
		}
		fprintf(stderr, "Error: no program or command line specified.\n");
		printHelp(argv[0], true);
		return 1;
	}

	if (chdirPath && chdirPath[0] != '\0') {
		if (chdir(chdirPath) != 0) {
			fprintf(stderr, "Failed to chdir to %s: %s\n", chdirPath, strerror(errno));
			return 1;
		}
	}

	if (optionDebug || getenv("WIBO_DEBUG")) {
		wibo_debugEnabled = true;
	}

	const char *debugIndentEnv = getenv("WIBO_DEBUG_INDENT");
	if (debugIndentEnv) {
		wibo_debugIndent = strtoul(debugIndentEnv, NULL, 10);
	}

	files_init();

	// Create PEB
	PEB *peb = wibo_heap_guestMalloc(sizeof(PEB), true);
	peb->ProcessParameters = toGuestPtr(wibo_heap_guestMalloc(sizeof(RTL_USER_PROCESS_PARAMETERS), true));

	// Create TIB
	TEB *tib = wibo_heap_guestMalloc(sizeof(TEB), true);
	wibo_tls_initializeTib(tib);
	tib->Tib.Self = toGuestPtr(tib);
	tib->Peb = toGuestPtr(peb);
	wibo_processPeb = peb;
	wibo_initializeTibStackInfo(tib);
	if (!wibo_installTibForCurrentThread(tib)) {
		perror("Failed to setup x86 segments and TEB");
		return 1;
	}

	// Determine the guest program name
	StringVec guestArgs = wibo_splitCommandLine(cmdLine);
	const char *programName = NULL;
	if (programIndex != -1) {
		programName = argv[programIndex];
	} else if (guestArgs.len > 0) {
		programName = guestArgs.data[0];
	}
	if (!programName || programName[0] == '\0') {
		fprintf(stderr, "No guest program specified\n");
		return 1;
	}

	// Resolve the guest program path
	char *resolvedGuestPath = wibo_resolveExecutable(programName, true);
	if (!resolvedGuestPath) {
		fprintf(stderr, "Failed to resolve path to guest program %s\n", programName);
		return 1;
	}

	// Build guest arguments
	int argIndex = -1;
	bool skipProgramName = false;
	if (programIndex != -1 && argc > programIndex + 1) {
		argIndex = programIndex + 1;
		// With "test.exe -- test 1 2 3", treat everything after -- as the full command line
		if (!literalArgs && strcmp(argv[argIndex], "--") == 0) {
			argIndex++;
			skipProgramName = true;
		}
	}
	if (guestArgs.len == 0 && !skipProgramName) {
		vec_push(&guestArgs, files_pathToWindows(resolvedGuestPath));
	}
	if (argIndex != -1) {
		for (int i = argIndex; i < argc; ++i) {
			vec_push(&guestArgs, xstrdup(argv[i]));
		}
	}

	// Build a command line
	Buf cmdBuf = {0};
	if (!hasCmdLine) {
		for (size_t i = 0; i < guestArgs.len; ++i) {
			if (i != 0) {
				Buf_appendc(&cmdBuf, ' ');
			}
			appendQuotedArg(&cmdBuf, guestArgs.data[i]);
		}
	} else {
		Buf_appendstr(&cmdBuf, cmdLine);
	}
	// The command line is kept NUL-terminated; the buffer's storage is handed over to wibo_commandLine.
	wibo_commandLine = Buf_cstr(&cmdBuf);
	wibo_commandLineW = stringToWideString(wibo_commandLine, SIZE_MAX);
	DEBUG_LOG("Command line: %s\n", wibo_commandLine);

	wibo_guestExecutablePath = resolvedGuestPath;

	// Build argv/argc
	char **guestArgv = xmalloc((guestArgs.len + 1) * sizeof *guestArgv);
	for (size_t i = 0; i < guestArgs.len; i++) {
		guestArgv[i] = guestArgs.data[i];
	}
	guestArgv[guestArgs.len] = NULL;
	wibo_argv = guestArgv;
	wibo_argc = (int)guestArgs.len;

	wibo_initializeModuleRegistry();

	FILE *f = wibo_openImage(resolvedGuestPath);
	if (!f) {
		fprintf(stderr, "Failed to open file %s: %s\n", resolvedGuestPath, strerror(errno));
		return 1;
	}

	Executable *executable = Executable_new();
	if (!Executable_loadPEFile(executable, f, true)) {
		fclose(f);
		Executable_free(executable);
		fprintf(stderr, "Failed to load PE image %s\n", resolvedGuestPath);
		return 1;
	}
	fclose(f);

	const EntryProc entryPoint = (EntryProc)executable->entryPoint;
	if (!entryPoint) {
		fprintf(stderr, "Executable %s has no entry point\n", resolvedGuestPath);
		Executable_free(executable);
		return 1;
	}
	if (executable->is64Bit != GUEST_IS_64BIT) {
		fprintf(stderr, "Executable %s is %s; this wibo runtime only runs %s images\n", resolvedGuestPath,
		        executable->is64Bit ? "PE32+ (Win64)" : "PE32", GUEST_IS_64BIT ? "PE32+ (Win64)" : "PE32");
		Executable_free(executable);
		return 1;
	}

	wibo_mainModule = wibo_registerProcessModule(executable, resolvedGuestPath, programName);
	if (!wibo_mainModule || !wibo_mainModule->executable) {
		fprintf(stderr, "Failed to register process module\n");
		return 1;
	}
	DEBUG_LOG("Registered main module %s at %p\n", wibo_mainModule->normalizedName,
			  wibo_mainModule->executable->imageBase);

	if (!Executable_resolveImports(wibo_mainModule->executable)) {
		fprintf(stderr, "Failed to resolve imports for main module (DLL initialization failure?)\n");
		abort();
	}
	if (!wibo_initializeModuleTls(wibo_mainModule)) {
		fprintf(stderr, "Failed to initialize TLS for main module\n");
		return 1;
	}

	// Reset last error
	kernel32_setLastError(0);
	kernel32_installFaultHandlers();

	// Invoke the damn thing
	call_EntryProc(entryPoint);
	DEBUG_LOG("We came back\n");
	wibo_shutdownModuleRegistry();
	wibo_tls_cleanupTib(tib);

	return 1;
}
