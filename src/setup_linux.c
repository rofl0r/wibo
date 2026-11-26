#include "setup.h"

#include "common.h"
#include "types.h"

#include <asm/ldt.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static pthread_mutex_t g_tebSetupMutex = PTHREAD_MUTEX_INITIALIZER;
static int g_threadAreaEntry = -1;
#ifdef __x86_64__
static uint16_t g_codeSelector = 0;
static uint16_t g_dataSelector = 0;
#endif
enum {
	kMaxLdtEntries = 8192,
	kBitsPerWord = 32,
};
static uint32_t g_ldtBitmap[kMaxLdtEntries / kBitsPerWord];
static bool g_ldtBitmapInitialized = false;
static int g_ldtHint = 0;

static struct user_desc createLdtEntry(uint32_t entryNumber, uint32_t base, uint32_t size, bool code) {
	struct user_desc desc;
	// Must memset to zero to avoid uninitialized padding bytes
	memset(&desc, 0, sizeof(desc));

	desc.entry_number = entryNumber;
	desc.base_addr = base;

	uint32_t limit;
	if (size > 0xFFFFF) {
		// Page granularity (like your DESC_GRAN_PAGE case)
		limit = (size - 1) >> 12;
		desc.limit_in_pages = 1;
	} else {
		// Byte granularity
		limit = size - 1;
		desc.limit_in_pages = 0;
	}
	desc.limit = limit;
	desc.seg_32bit = 1;
	desc.contents = code ? MODIFY_LDT_CONTENTS_CODE : MODIFY_LDT_CONTENTS_DATA;
	desc.read_exec_only = 0;
	desc.seg_not_present = 0;
	desc.useable = 1;
	return desc;
}

static int modifyLdtRead(struct user_desc *entries, int bytes) {
	return (int)syscall(SYS_modify_ldt, 2, entries, bytes);
}

static int modifyLdtWrite(const struct user_desc *desc) {
	return (int)syscall(SYS_modify_ldt, 1, desc, sizeof(*desc));
}

static uint16_t createGdtSelector(int entryNumber) {
	return (uint16_t)((entryNumber << 3) | USER_PRIVILEGE);
}

static uint16_t createLdtSelector(int entryNumber) {
	return (uint16_t)((entryNumber << 3) | 0x4 /* TI=1 */ | USER_PRIVILEGE);
}

static bool isLdtEntryValid(int entry) { return entry >= 0 && entry < kMaxLdtEntries; }

static void markLdtEntryUsed(int entry) {
	if (!isLdtEntryValid(entry)) {
		return;
	}
	g_ldtBitmap[entry / kBitsPerWord] |= (1u << (entry % kBitsPerWord));
}

static void markLdtEntryFree(int entry) {
	if (!isLdtEntryValid(entry)) {
		return;
	}
	g_ldtBitmap[entry / kBitsPerWord] &= ~(1u << (entry % kBitsPerWord));
}

static bool isLdtEntryUsed(int entry) {
	if (!isLdtEntryValid(entry)) {
		return true;
	}
	return (g_ldtBitmap[entry / kBitsPerWord] & (1u << (entry % kBitsPerWord))) != 0;
}

static void initializeLdtBitmapLocked(void) {
	if (g_ldtBitmapInitialized) {
		return;
	}
	g_ldtBitmapInitialized = true;
	struct user_desc table[kMaxLdtEntries];
	memset(table, 0, sizeof(table));
	int bytes = modifyLdtRead(table, sizeof(table));
	if (bytes < 0) {
		DEBUG_LOG("setup_linux: modify_ldt(read) failed during bitmap init (%s), assuming empty table\n",
				  strerror(errno));
		return;
	}
	int count = bytes / (int)sizeof(struct user_desc);
	if (count > kMaxLdtEntries) {
		DEBUG_LOG("setup_linux: modify_ldt(read) returned too many entries (%d), truncating to %d\n", count,
				  kMaxLdtEntries);
		count = kMaxLdtEntries;
	}
	for (int i = 0; i < count; ++i) {
		const struct user_desc *d = &table[i];
		struct user_desc unused;
		memset(&unused, 0, sizeof(struct user_desc));
		bool allZero = memcmp(d, &unused, sizeof(struct user_desc)) == 0;
		if (!allZero && !d->seg_not_present) {
			markLdtEntryUsed(i);
		}
	}
}

static int tryAllocateLdtEntry(int start) {
	for (int entry = start; entry < kMaxLdtEntries; ++entry) {
		if (!isLdtEntryUsed(entry)) {
			markLdtEntryUsed(entry);
			g_ldtHint = entry + 1;
			if (g_ldtHint >= kMaxLdtEntries) {
				g_ldtHint = 0;
			}
			DEBUG_LOG("setup_linux: Allocating LDT entry %d\n", entry);
			return entry;
		}
	}
	return -1;
}

static int allocateLdtEntryLocked(void) {
	initializeLdtBitmapLocked();
	int entry = tryAllocateLdtEntry(g_ldtHint);
	if (entry >= 0) {
		return entry;
	}
	entry = tryAllocateLdtEntry(0);
	if (entry >= 0) {
		return entry;
	}
	errno = ENOSPC;
	return -1;
}

static void freeLdtEntryLocked(int entryNumber) {
	if (!g_ldtBitmapInitialized || !isLdtEntryValid(entryNumber)) {
		return;
	}
	markLdtEntryFree(entryNumber);
	if (entryNumber < g_ldtHint) {
		g_ldtHint = entryNumber;
	}
}

#ifdef __x86_64__
static bool segmentSetupLocked(TEB *teb) {
	// Create code LDT entry
	if (g_codeSelector == 0) {
		int entryNumber = allocateLdtEntryLocked();
		if (entryNumber < 0) {
			return false;
		}
		struct user_desc codeLdt = createLdtEntry(entryNumber, 0, 0xFFFFFFFF, true);
		int ret = modifyLdtWrite(&codeLdt);
		if (ret == 0) {
			g_codeSelector = createLdtSelector(entryNumber);
			DEBUG_LOG("setup_linux: Code LDT selector 0x%x\n", g_codeSelector);
		} else {
			freeLdtEntryLocked(entryNumber);
			DEBUG_LOG("setup_linux: Failed to create code LDT entry, trying default (0x23)\n");
			g_codeSelector = 0x23;
		}
	}
	// Create data LDT entry
	if (g_dataSelector == 0) {
		int entryNumber = allocateLdtEntryLocked();
		if (entryNumber < 0) {
			return false;
		}
		struct user_desc dataLdt = createLdtEntry(entryNumber, 0, 0xFFFFFFFF, false);
		int ret = modifyLdtWrite(&dataLdt);
		if (ret == 0) {
			g_dataSelector = createLdtSelector(entryNumber);
			DEBUG_LOG("setup_linux: Data LDT selector 0x%x\n", g_dataSelector);
		} else {
			freeLdtEntryLocked(entryNumber);
			DEBUG_LOG("setup_linux: Failed to create data LDT entry, trying default (0x2b)\n");
			g_dataSelector = 0x2b;
		}
	}
	teb->CodeSelector = g_codeSelector;
	teb->DataSelector = g_dataSelector;
	return true;
}
#endif

#if defined(__x86_64__)

#include <cpuid.h>

// Implemented in setup.S
void installSelectors(TEB *teb);
int setThreadArea64(int entryNumber, TEB *teb);

// setThreadArea64 issues set_thread_area through `int 0x80`. Some seccomp
// sandboxes block that compat path: AWS Lambda, for instance, kills the process
// with SIGSYS when it runs. We can't just call it and recover, because the
// filter may use a non-catchable KILL action rather than SECCOMP_RET_TRAP. So
// probe it once in a forked child — if the child is killed by the filter (or the
// syscall otherwise fails), treat set_thread_area as unavailable and let
// tebThreadSetup fall back to modify_ldt. Callers hold g_tebSetupMutex and the
// result is cached, so this forks at most once, during startup.
static bool setThreadAreaAvailable(TEB *teb) {
	static int cached = -1;
	if (cached >= 0) {
		return cached != 0;
	}
	pid_t pid = fork();
	if (pid == 0) {
		_exit(setThreadArea64(g_threadAreaEntry, teb) >= 0 ? 0 : 1);
	}
	if (pid < 0) {
		cached = 1; // can't probe; assume available and keep the existing behaviour
		return true;
	}
	int status = 0;
	pid_t w;
	do {
		w = waitpid(pid, &status, 0);
	} while (w < 0 && errno == EINTR);
	cached = (w == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : 0;
	if (!cached) {
		DEBUG_LOG("setup_linux: set_thread_area blocked by seccomp, using LDT\n");
	}
	return cached != 0;
}

static bool tebThreadSetupLocked(TEB *teb) {
	// Check for FSBASE/GSBASE instruction support
	unsigned int regs[4];
	unsigned int cpuidMax = __get_cpuid_max(0, NULL);
	if (cpuidMax >= 0x7 && __get_cpuid_count(0x7, 0, &regs[0], &regs[1], &regs[2], &regs[3])) {
		teb->HasFsGsBase = !!(regs[1] & 1);
	}
	DEBUG_LOG("setup_linux: FSBASE/GSBASE instruction support: %s\n", teb->HasFsGsBase ? "yes" : "no");

	// Create code and data LDT entries
	if (!segmentSetupLocked(teb)) {
		return false;
	}

	// Install ds/es selectors
	installSelectors(teb);

	if (g_threadAreaEntry != -2 && setThreadAreaAvailable(teb)) {
		int ret = setThreadArea64(g_threadAreaEntry, teb);
		if (ret >= 0) {
			if (g_threadAreaEntry != ret) {
				g_threadAreaEntry = ret;
				DEBUG_LOG("setup_linux: allocated thread-local GDT entry=%d base=%p\n", g_threadAreaEntry, (void *)teb);
			} else {
				DEBUG_LOG("setup_linux: reused thread-local GDT entry=%d base=%p\n", g_threadAreaEntry, (void *)teb);
			}
			teb->CurrentFsSelector = createGdtSelector(ret);
		} else {
			DEBUG_LOG("setup_linux: set_thread_area failed (%s), falling back to LDT\n", strerror(errno));
			g_threadAreaEntry = -2; // Don't bother trying again
		}
	}
	if (teb->CurrentFsSelector == 0) {
		int entryNumber = allocateLdtEntryLocked();
		if (entryNumber < 0) {
			return false;
		}
		struct user_desc fsLdt = createLdtEntry(entryNumber, toGuestPtr(teb), sizeof(TEB), false);
		int ret = modifyLdtWrite(&fsLdt);
		if (ret != 0) {
			freeLdtEntryLocked(entryNumber);
			return false;
		}
		teb->CurrentFsSelector = createLdtSelector(entryNumber);
	}

	DEBUG_LOG("setup_linux: Using FS selector 0x%x\n", teb->CurrentFsSelector);
	return true;
}

#elif defined(__i386__)

static bool tebThreadSetupLocked(TEB *teb) {
	if (g_threadAreaEntry != -2) {
		struct user_desc desc;
		memset(&desc, 0, sizeof(desc));
		desc.entry_number = g_threadAreaEntry;
		desc.base_addr = (uintptr_t)teb;
		desc.limit = (unsigned int)(sizeof(TEB) - 1);
		desc.seg_32bit = 1;
		desc.contents = 0;
		desc.read_exec_only = 0;
		desc.limit_in_pages = 0;
		desc.seg_not_present = 0;
		desc.useable = 1;
		if (syscall(SYS_set_thread_area, &desc) == 0) {
			if (g_threadAreaEntry != (int)desc.entry_number) {
				g_threadAreaEntry = (int)desc.entry_number;
				DEBUG_LOG("setup_linux: allocated thread-local GDT entry=%d base=%p\n", g_threadAreaEntry, (void *)teb);
			} else {
				DEBUG_LOG("setup_linux: reused thread-local GDT entry=%d base=%p\n", g_threadAreaEntry, (void *)teb);
			}
			teb->CurrentFsSelector = createGdtSelector(desc.entry_number);
		} else {
			DEBUG_LOG("setup_linux: set_thread_area failed (%s), falling back to LDT\n", strerror(errno));
			g_threadAreaEntry = -2; // Don't bother trying again
		}
	}
	if (teb->CurrentFsSelector == 0) {
		int entryNumber = allocateLdtEntryLocked();
		if (entryNumber < 0) {
			return false;
		}
		struct user_desc fsLdt = createLdtEntry(entryNumber, toGuestPtr(teb), sizeof(TEB), false);
		int ret = modifyLdtWrite(&fsLdt);
		if (ret != 0) {
			freeLdtEntryLocked(entryNumber);
			return false;
		}
		teb->CurrentFsSelector = createLdtSelector(entryNumber);
	}

	DEBUG_LOG("setup_linux: Using FS selector 0x%x\n", teb->CurrentFsSelector);
	return true;
}

#endif

bool tebThreadSetup(TEB *teb) {
	pthread_mutex_lock(&g_tebSetupMutex);
	bool ok = tebThreadSetupLocked(teb);
	pthread_mutex_unlock(&g_tebSetupMutex);
	return ok;
}

bool tebThreadTeardown(TEB *teb) {
	if (teb->CurrentFsSelector & 0x4 /* TI=1 */) {
		pthread_mutex_lock(&g_tebSetupMutex);
		freeLdtEntryLocked(teb->CurrentFsSelector >> 3);
		pthread_mutex_unlock(&g_tebSetupMutex);
	}
	teb->CurrentFsSelector = 0;
	return true;
}
