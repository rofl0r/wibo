#include "heap.h"

#include "common.h"
#include "cutil.h"
#include "errors.h"
#include "processes.h"
#include "types.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __linux__
// Alpine hack: rename duplicate prctl_mm_map (sys/prctl.h also includes it)
#define prctl_mm_map _prctl_mm_map
#include <linux/prctl.h>
#undef prctl_mm_map
#include <sys/prctl.h>
#endif

// Pre-initialization logging macros
#define LOG_OUT(msg) write(STDOUT_FILENO, msg, strlen(msg))
#define LOG_ERR(msg) write(STDERR_FILENO, msg, strlen(msg))

static const uintptr_t kLowMemoryStart = 0x00110000UL; // 1 MiB + 64 KiB
static const uintptr_t kHeapMax = 0x70000000UL;
#if defined(WIBO_GUEST64)
// Win64 guests use native 64-bit addresses. kTwoGB is the exclusive ceiling of the guest arena.
static const uintptr_t kTopDownStart = 0x7E0000000000UL;
static const uintptr_t kTwoGB = 0x7F0000000000UL;
#elif defined(__APPLE__)
// On macOS, our program is mapped at 0x7E001000
static const uintptr_t kTopDownStart = 0x7D000000UL;
static const uintptr_t kTwoGB = 0x7E000000UL;
#else
static const uintptr_t kTopDownStart = 0x7F000000UL; // Just below 2GB
static const uintptr_t kTwoGB = 0x80000000UL;
#endif
static const size_t kVirtualAllocationGranularity = 64UL * 1024UL;
static const uintptr_t kDefaultMmapMinAddr = 0x10000u;

#define GUEST_ARENA_SIZE ((size_t)64 << 20)
#define LARGE_ALLOC_THRESHOLD ((size_t)8 << 20)
#define NO_INDEX SIZE_MAX

#ifdef __linux__
#define MAPS_BUFFER_SIZE 0x10000
#define MAX_NUM_MAPPINGS 128
static MEMORY_BASIC_INFORMATION g_initialMappings[MAX_NUM_MAPPINGS];
static size_t g_numInitialMappings = 0;
#endif
static uintptr_t g_mmapMinAddr = kDefaultMmapMinAddr;

typedef struct {
	uintptr_t base;
	MEMORY_BASIC_INFORMATION info;
} MappingEntry;
typedef VEC(MappingEntry) MappingVec;

// Sorted by base. Records every host and guest mapping the loader knows about.
static MappingVec g_mappings;
static bool g_mappingsReady = false;
static pthread_mutex_t g_mappingsMutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
	uintptr_t base;
	size_t size;
	DWORD allocationProtect;
	DWORD type;
	DWORD *pageProtect; // One entry per page; 0 means decommitted.
} VirtualAllocation;
typedef VEC(VirtualAllocation) VirtualAllocationVec;

// Sorted by base. Guarded by g_mappingsMutex.
static VirtualAllocationVec g_virtualAllocations;

static uintptr_t alignDown(uintptr_t value, size_t alignment) {
	return value & ~((uintptr_t)alignment - 1);
}

static uintptr_t alignUp(uintptr_t value, size_t alignment) {
	const uintptr_t mask = (uintptr_t)alignment - 1;
	if (mask == UINTPTR_MAX) {
		return value;
	}
	if (value > UINTPTR_MAX - mask) {
		return UINTPTR_MAX;
	}
	return (value + mask) & ~mask;
}

static bool addOverflows(uintptr_t base, size_t amount) {
	return base > UINTPTR_MAX - (uintptr_t)amount;
}

#ifdef __linux__
static uintptr_t readMmapMinAddr(void) {
	char buf[64];
	int fd = open("/proc/sys/vm/mmap_min_addr", O_RDONLY | O_CLOEXEC, 0);
	if (fd < 0) {
		return kDefaultMmapMinAddr;
	}
	ssize_t rd = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (rd <= 0) {
		return kDefaultMmapMinAddr;
	}
	buf[rd] = '\0';
	uintptr_t value = 0;
	const char *p = buf;
	while (*p >= '0' && *p <= '9') {
		value = value * 10 + (uintptr_t)(*p - '0');
		++p;
	}
	if (p == buf) {
		LOG_ERR("heap: failed to parse mmap_min_addr\n");
		return kDefaultMmapMinAddr;
	}
	if (value < kDefaultMmapMinAddr) {
		value = kDefaultMmapMinAddr;
	}
	return value;
}
#endif

static void setVirtualAllocationName(void *ptr, size_t len, const char *name) {
#ifdef __linux__
	if (name) {
		prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, ptr, len, name);
	}
#else
	(void)ptr;
	(void)len;
	(void)name;
#endif
}

#ifdef __linux__
static void reserveGuestRange(uintptr_t start, uintptr_t end) {
	if (start >= end) {
		return;
	}
	size_t len = end - start;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#ifdef MAP_FIXED_NOREPLACE
	flags |= MAP_FIXED_NOREPLACE;
#endif
	void *ptr = mmap((void *)start, len, PROT_NONE, flags, -1, 0);
	if (ptr == MAP_FAILED || (uintptr_t)ptr != start) {
		LOG_ERR("heap: failed to reserve guest address space\n");
		_exit(1);
	}
	setVirtualAllocationName(ptr, len, "wibo reserved");
}
#endif

static int posixProtectFromWin32(DWORD flProtect) {
	switch (flProtect & 0xFF) {
	case PAGE_NOACCESS:
		return PROT_NONE;
	case PAGE_READONLY:
		return PROT_READ;
	case PAGE_READWRITE:
	case PAGE_WRITECOPY:
		return PROT_READ | PROT_WRITE;
	case PAGE_EXECUTE:
		return PROT_EXEC;
	case PAGE_EXECUTE_READ:
		return PROT_READ | PROT_EXEC;
	case PAGE_EXECUTE_READWRITE:
	case PAGE_EXECUTE_WRITECOPY:
		return PROT_READ | PROT_WRITE | PROT_EXEC;
	default:
		DEBUG_LOG("heap: unhandled flProtect %u, defaulting to RW\n", flProtect);
		return PROT_READ | PROT_WRITE;
	}
}

static VmStatus vmStatusFromErrno(int err) {
	switch (err) {
	case ENOMEM:
		return VmStatus_NoMemory;
	case EACCES:
	case EPERM:
		return VmStatus_NoAccess;
	case EINVAL:
		return VmStatus_InvalidParameter;
	case EBUSY:
		return VmStatus_Rejected;
	default:
		return VmStatus_UnknownError;
	}
}

// Index of the first mapping with base >= addr (or > addr when !inclusive).
static size_t mappingBound(uintptr_t addr, bool inclusive) {
	size_t lo = 0;
	size_t hi = g_mappings.len;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		uintptr_t base = g_mappings.data[mid].base;
		if (inclusive ? base < addr : base <= addr) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

// Inserts or overwrites the entry for base. Does not check g_mappingsReady.
static void mappingPut(uintptr_t base, const MEMORY_BASIC_INFORMATION *info) {
	size_t idx = mappingBound(base, true);
	if (idx < g_mappings.len && g_mappings.data[idx].base == base) {
		g_mappings.data[idx].info = *info;
		return;
	}
	vec_reserve(&g_mappings, 1);
	memmove(&g_mappings.data[idx + 1], &g_mappings.data[idx], (g_mappings.len - idx) * sizeof *g_mappings.data);
	g_mappings.data[idx].base = base;
	g_mappings.data[idx].info = *info;
	g_mappings.len++;
}

static bool overlapsExistingMappingLocked(uintptr_t base, size_t length) {
	if (!g_mappingsReady || length == 0) {
		return false;
	}
	if (addOverflows(base, length - 1)) {
		return true;
	}
	uintptr_t end = base + length;
	size_t i = mappingBound(base, false);
	if (i > 0) {
		--i;
	}
	for (; i < g_mappings.len; ++i) {
		const MappingEntry *entry = &g_mappings.data[i];
		if (entry->info.RegionSize == 0) {
			continue;
		}
		uintptr_t mapStart = entry->base;
		uintptr_t mapEnd = mapStart + (uintptr_t)entry->info.RegionSize;
		if (mapEnd <= base) {
			continue;
		}
		if (mapStart >= end) {
			break;
		}
		return true;
	}
	return false;
}

static void recordGuestMappingLocked(uintptr_t base, size_t size, DWORD allocationProtect, DWORD state, DWORD protect,
									 DWORD type) {
	if (!g_mappingsReady) {
		return;
	}
	MEMORY_BASIC_INFORMATION info = {
		.BaseAddress = toGuestPtr((void *)base),
		.AllocationBase = toGuestPtr((void *)base),
		.AllocationProtect = allocationProtect,
		.RegionSize = size,
		.State = state,
		.Protect = protect,
		.Type = type,
	};
	mappingPut(base, &info);
}

static void eraseGuestMappingLocked(uintptr_t base) {
	if (!g_mappingsReady) {
		return;
	}
	size_t idx = mappingBound(base, true);
	if (idx < g_mappings.len && g_mappings.data[idx].base == base) {
		vec_erase(&g_mappings, idx);
	}
}

// Index of the first region with base >= addr.
static size_t virtualAllocationBound(uintptr_t addr) {
	size_t lo = 0;
	size_t hi = g_virtualAllocations.len;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (g_virtualAllocations.data[mid].base < addr) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

// Index of the region whose base is exactly addr, or NO_INDEX.
static size_t virtualAllocationFind(uintptr_t addr) {
	size_t idx = virtualAllocationBound(addr);
	if (idx < g_virtualAllocations.len && g_virtualAllocations.data[idx].base == addr) {
		return idx;
	}
	return NO_INDEX;
}

// Index of the region containing address, or NO_INDEX.
static size_t findRegionIndex(uintptr_t address) {
	size_t lo = 0;
	size_t hi = g_virtualAllocations.len;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (g_virtualAllocations.data[mid].base <= address) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo == 0) {
		return NO_INDEX;
	}
	const VirtualAllocation *region = &g_virtualAllocations.data[lo - 1];
	if (address >= region->base + region->size) {
		return NO_INDEX;
	}
	return lo - 1;
}

// Pointers into g_virtualAllocations are invalidated by the next insert or erase.
static VirtualAllocation *lookupRegion(uintptr_t address) {
	size_t idx = findRegionIndex(address);
	return idx == NO_INDEX ? NULL : &g_virtualAllocations.data[idx];
}

static VirtualAllocation *virtualAllocationInsert(VirtualAllocation allocation) {
	size_t idx = virtualAllocationBound(allocation.base);
	if (idx < g_virtualAllocations.len && g_virtualAllocations.data[idx].base == allocation.base) {
		free(g_virtualAllocations.data[idx].pageProtect);
		g_virtualAllocations.data[idx] = allocation;
		return &g_virtualAllocations.data[idx];
	}
	vec_reserve(&g_virtualAllocations, 1);
	memmove(&g_virtualAllocations.data[idx + 1], &g_virtualAllocations.data[idx],
			(g_virtualAllocations.len - idx) * sizeof *g_virtualAllocations.data);
	g_virtualAllocations.data[idx] = allocation;
	g_virtualAllocations.len++;
	return &g_virtualAllocations.data[idx];
}

static void virtualAllocationErase(size_t idx) {
	free(g_virtualAllocations.data[idx].pageProtect);
	vec_erase(&g_virtualAllocations, idx);
}

static bool rangeWithinRegion(const VirtualAllocation *region, uintptr_t start, size_t length) {
	const uintptr_t end = region->base + region->size;
	if (length == 0) {
		return start >= region->base && start <= end;
	}
	if (start < region->base) {
		return false;
	}
	if (addOverflows(start, length)) {
		return false;
	}
	return (start + length) <= end;
}

static void setPageProtect(VirtualAllocation *region, uintptr_t start, size_t length, DWORD protect) {
	if (length == 0) {
		return;
	}
	const size_t pageSize = wibo_heap_systemPageSize();
	const size_t firstPage = (start - region->base) / pageSize;
	const size_t pageCount = length / pageSize;
	for (size_t i = 0; i < pageCount; ++i) {
		region->pageProtect[firstPage + i] = protect;
	}
}

static void refreshGuestMappingLocked(const VirtualAllocation *region) {
	if (!g_mappingsReady) {
		return;
	}
	const size_t pageCount = region->size / wibo_heap_systemPageSize();
	bool allCommitted = true;
	bool anyCommitted = false;
	DWORD firstProtect = 0;
	bool uniformProtect = true;
	for (size_t i = 0; i < pageCount; ++i) {
		DWORD pageProtect = region->pageProtect[i];
		if (pageProtect == 0) {
			allCommitted = false;
			continue;
		}
		anyCommitted = true;
		if (firstProtect == 0) {
			firstProtect = pageProtect;
		} else if (firstProtect != pageProtect) {
			uniformProtect = false;
		}
	}
	DWORD state = allCommitted && anyCommitted ? MEM_COMMIT : MEM_RESERVE;
	DWORD protect = PAGE_NOACCESS;
	if (state == MEM_COMMIT && uniformProtect && firstProtect != 0) {
		protect = firstProtect;
	}
	DWORD allocationProtect = region->allocationProtect != 0 ? region->allocationProtect : PAGE_NOACCESS;
	recordGuestMappingLocked(region->base, region->size, allocationProtect, state, protect, region->type);
}

static bool mapAtAddrLocked(uintptr_t addr, size_t size, const char *name, void **outPtr) {
	void *p = mmap((void *)addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (p == MAP_FAILED) {
		return false;
	}
	setVirtualAllocationName(p, size, name);
	recordGuestMappingLocked((uintptr_t)p, size, PAGE_READWRITE, MEM_RESERVE, PAGE_READWRITE, MEM_PRIVATE);
	if (outPtr) {
		*outPtr = p;
	}
	return true;
}

// Chooses a granularity-aligned placement of alignedSize within [gapStart, gapEnd).
static bool gapCandidate(uintptr_t gapStart, uintptr_t gapEnd, uintptr_t alignedSize, bool preferTop,
						 uintptr_t *result) {
	if (gapEnd <= gapStart) {
		return false;
	}
	uintptr_t lower = alignUp(gapStart, kVirtualAllocationGranularity);
	if (lower >= gapEnd) {
		return false;
	}
	if (!preferTop) {
		if (lower + alignedSize <= gapEnd) {
			*result = lower;
			return true;
		}
		return false;
	}
	if (gapEnd < alignedSize) {
		return false;
	}
	uintptr_t chosen = alignDown(gapEnd - alignedSize, kVirtualAllocationGranularity);
	if (chosen < lower || chosen + alignedSize > gapEnd) {
		return false;
	}
	*result = chosen;
	return true;
}

// Gaps are visited in ascending order, so for preferTop the last hit is the highest candidate.
static void considerGap(uintptr_t gapStart, uintptr_t gapEnd, uintptr_t alignedSize, bool preferTop, bool *found,
						uintptr_t *best) {
	uintptr_t candidate = 0;
	if (!gapCandidate(gapStart, gapEnd, alignedSize, preferTop, &candidate)) {
		return;
	}
	if (!preferTop && *found) {
		return;
	}
	*best = candidate;
	*found = true;
}

static bool findFreeMappingLocked(size_t size, uintptr_t minAddr, uintptr_t maxAddr, bool preferTop,
								  uintptr_t *outAddr) {
	if (outAddr == NULL || size == 0 || !g_mappingsReady) {
		return false;
	}

	const uintptr_t pageSize = wibo_heap_systemPageSize();
	const uintptr_t alignedSize = alignUp((uintptr_t)size, pageSize);
	if (maxAddr <= minAddr || alignedSize > (maxAddr - minAddr)) {
		return false;
	}

	bool found = false;
	uintptr_t best = 0;
	uintptr_t cursor = minAddr;
	for (size_t i = 0; i < g_mappings.len; ++i) {
		uintptr_t mapStart = g_mappings.data[i].base;
		uintptr_t mapEnd = mapStart + (uintptr_t)g_mappings.data[i].info.RegionSize;
		if (mapEnd <= cursor) {
			continue;
		}
		if (mapStart > cursor) {
			considerGap(cursor, mapStart < maxAddr ? mapStart : maxAddr, alignedSize, preferTop, &found, &best);
			if (found && !preferTop) {
				*outAddr = best;
				return true;
			}
		}
		if (mapEnd > cursor) {
			cursor = alignUp(mapEnd, pageSize);
		}
		if (cursor >= maxAddr) {
			break;
		}
	}
	if (cursor < maxAddr) {
		considerGap(cursor, maxAddr, alignedSize, preferTop, &found, &best);
	}
	if (found) {
		*outAddr = best;
		return true;
	}
	return false;
}

static bool mapArena(size_t size, uintptr_t minAddr, uintptr_t maxAddr, bool preferTop, const char *name,
					 void **outPtr, size_t *outSize) {
	pthread_mutex_lock(&g_mappingsMutex);
	const size_t ps = wibo_heap_systemPageSize();
	size = (size + ps - 1) & ~(ps - 1);
	uintptr_t cand = 0;
	void *p = NULL;
	bool ok = false;
	if (findFreeMappingLocked(size, minAddr, maxAddr, preferTop, &cand)) {
		DEBUG_LOG("heap: found free mapping at %lx\n", cand);
		if (mapAtAddrLocked(cand, size, name, &p)) {
			*outPtr = p;
			*outSize = size;
			ok = true;
		}
	}
	pthread_mutex_unlock(&g_mappingsMutex);
	return ok;
}

uintptr_t wibo_heap_systemPageSize(void) {
	static uintptr_t cached = 0;
	if (cached == 0) {
		long detected = sysconf(_SC_PAGESIZE);
		cached = detected <= 0 ? 4096 : (uintptr_t)detected;
	}
	return cached;
}

uintptr_t wibo_heap_allocationGranularity(void) { return kVirtualAllocationGranularity; }

DWORD wibo_heap_win32ErrorFromVmStatus(VmStatus status) {
	switch (status) {
	case VmStatus_Success:
		return ERROR_SUCCESS;
	case VmStatus_InvalidParameter:
		return ERROR_INVALID_PARAMETER;
	case VmStatus_InvalidAddress:
	case VmStatus_Rejected:
		return ERROR_INVALID_ADDRESS;
	case VmStatus_NoAccess:
		return ERROR_NOACCESS;
	case VmStatus_NotSupported:
		return ERROR_NOT_SUPPORTED;
	case VmStatus_NoMemory:
		return ERROR_NOT_ENOUGH_MEMORY;
	case VmStatus_UnknownError:
	default:
		return ERROR_INVALID_PARAMETER;
	}
}

NTSTATUS wibo_heap_ntStatusFromVmStatus(VmStatus status) {
	return wibo_statusFromWinError(wibo_heap_win32ErrorFromVmStatus(status));
}

VmStatus wibo_heap_virtualReset(void *baseAddress, size_t regionSize) {
	if (!baseAddress) {
		return VmStatus_InvalidAddress;
	}
	if (regionSize == 0) {
		return VmStatus_InvalidParameter;
	}
	uintptr_t request = (uintptr_t)baseAddress;
	if (addOverflows(request, regionSize)) {
		return VmStatus_InvalidParameter;
	}
	const uintptr_t pageSize = wibo_heap_systemPageSize();
	uintptr_t start = alignDown(request, pageSize);
	uintptr_t end = alignUp(request + (uintptr_t)regionSize, pageSize);
	size_t length = (size_t)(end - start);
	if (length == 0) {
		return VmStatus_InvalidParameter;
	}
	pthread_mutex_lock(&g_mappingsMutex);
	const VirtualAllocation *region = lookupRegion(start);
	bool valid = region && rangeWithinRegion(region, start, length);
	pthread_mutex_unlock(&g_mappingsMutex);
	if (!valid) {
		return VmStatus_InvalidAddress;
	}
#ifdef MADV_FREE
	int advice = MADV_FREE;
#else
	int advice = MADV_DONTNEED;
#endif
	if (madvise((void *)start, length, advice) != 0) {
		return vmStatusFromErrno(errno);
	}
	return VmStatus_Success;
}

// Requires g_mappingsMutex. Reserves (and optionally commits) a fresh region.
static VmStatus reserveRegionLocked(void *requestedAddress, size_t requestedSize, bool commit, DWORD protect,
									DWORD type, bool topDown, uintptr_t *outBase, size_t *outLength) {
	const uintptr_t pageSize = wibo_heap_systemPageSize();
	uintptr_t base = 0;
	size_t length = 0;
	if (requestedAddress != NULL) {
		uintptr_t request = (uintptr_t)requestedAddress;
		base = alignDown(request, kVirtualAllocationGranularity);
		size_t offset = (size_t)(request - base);
		if (addOverflows(offset, requestedSize)) {
			return VmStatus_InvalidParameter;
		}
		uintptr_t alignedSpan = alignUp((uintptr_t)(requestedSize + offset), pageSize);
		if (alignedSpan == 0) {
			return VmStatus_InvalidParameter;
		}
		length = (size_t)alignedSpan;
		if (base >= kTwoGB || (base + length) > kTwoGB) {
			return VmStatus_InvalidAddress;
		}
		if (overlapsExistingMappingLocked(base, length)) {
			return VmStatus_InvalidAddress;
		}
	} else {
		uintptr_t aligned = alignUp((uintptr_t)requestedSize, pageSize);
		if (aligned == 0) {
			return VmStatus_InvalidParameter;
		}
		length = (size_t)aligned;
		if (!findFreeMappingLocked(length, kLowMemoryStart, kTopDownStart, topDown, &base)) {
			return VmStatus_NoMemory;
		}
		if (base >= kTwoGB || (base + length) > kTwoGB) {
			return VmStatus_NoMemory;
		}
	}

	int prot = commit ? posixProtectFromWin32(protect) : PROT_NONE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
	if (!commit) {
		flags |= MAP_NORESERVE;
	}
	void *mapped = mmap((void *)base, length, prot, flags, -1, 0);
	if (mapped == MAP_FAILED) {
		return vmStatusFromErrno(errno);
	}
	setVirtualAllocationName(mapped, length, type == MEM_IMAGE ? "wibo guest image" : "wibo guest allocated");

	VirtualAllocation allocation = {
		.base = (uintptr_t)mapped,
		.size = length,
		.allocationProtect = protect,
		.type = type,
		.pageProtect = xcalloc(length / pageSize, sizeof(DWORD)),
	};
	if (commit) {
		for (size_t i = 0; i < length / pageSize; ++i) {
			allocation.pageProtect[i] = protect;
		}
	}
	VirtualAllocation *region = virtualAllocationInsert(allocation);
	refreshGuestMappingLocked(region);

	*outBase = (uintptr_t)mapped;
	*outLength = length;
	return VmStatus_Success;
}

// Requires g_mappingsMutex. Commits the uncommitted pages of an existing reservation.
static VmStatus commitRangeLocked(void *requestedAddress, size_t requestedSize, DWORD protect, uintptr_t *outBase,
								  size_t *outLength) {
	if (!requestedAddress) {
		return VmStatus_InvalidAddress;
	}
	const uintptr_t pageSize = wibo_heap_systemPageSize();
	uintptr_t request = (uintptr_t)requestedAddress;
	if (addOverflows(request, requestedSize)) {
		return VmStatus_InvalidParameter;
	}
	uintptr_t start = alignDown(request, pageSize);
	uintptr_t end = alignUp(request + (uintptr_t)requestedSize, pageSize);
	size_t length = (size_t)(end - start);
	if (length == 0) {
		return VmStatus_InvalidParameter;
	}

	VirtualAllocation *region = lookupRegion(start);
	if (!region || !rangeWithinRegion(region, start, length)) {
		return VmStatus_InvalidAddress;
	}

	const size_t firstPage = (start - region->base) / pageSize;
	const size_t pageCount = length / pageSize;
	for (size_t i = 0; i < pageCount;) {
		if (region->pageProtect[firstPage + i] != 0) {
			++i;
			continue;
		}
		size_t runStart = i;
		while (i < pageCount && region->pageProtect[firstPage + i] == 0) {
			++i;
		}
		uintptr_t runBase = start + runStart * pageSize;
		size_t runLength = (i - runStart) * pageSize;
		void *res = mmap((void *)runBase, runLength, posixProtectFromWin32(protect),
						 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (res == MAP_FAILED) {
			return vmStatusFromErrno(errno);
		}
		setVirtualAllocationName(res, runLength, "wibo guest committed");
		setPageProtect(region, runBase, runLength, protect);
	}

	refreshGuestMappingLocked(region);
	*outBase = start;
	*outLength = length;
	return VmStatus_Success;
}

VmStatus wibo_heap_virtualAlloc(void **baseAddress, size_t *regionSize, DWORD allocationType, DWORD protect,
								DWORD type) {
	if (!regionSize) {
		return VmStatus_InvalidParameter;
	}
	size_t requestedSize = *regionSize;
	if (requestedSize == 0) {
		return VmStatus_InvalidParameter;
	}
	void *requestedAddress = baseAddress ? *baseAddress : NULL;

	DWORD unsupportedFlags = allocationType & (MEM_WRITE_WATCH | MEM_PHYSICAL | MEM_LARGE_PAGES | MEM_RESET_UNDO);
	if (unsupportedFlags != 0) {
		return VmStatus_NotSupported;
	}

	bool reserve = (allocationType & MEM_RESERVE) != 0;
	bool commit = (allocationType & MEM_COMMIT) != 0;
	bool reset = (allocationType & MEM_RESET) != 0;
	bool topDown = (allocationType & MEM_TOP_DOWN) != 0;

	if (!reserve && commit && requestedAddress == NULL) {
		reserve = true;
	}

	const uintptr_t pageSize = wibo_heap_systemPageSize();
	if (reset) {
		if (reserve || commit) {
			return VmStatus_InvalidParameter;
		}
		if (requestedAddress == NULL) {
			return VmStatus_InvalidAddress;
		}
		uintptr_t requestVal = (uintptr_t)requestedAddress;
		uintptr_t start = alignDown(requestVal, pageSize);
		uintptr_t end = alignUp(requestVal + (uintptr_t)requestedSize, pageSize);
		size_t length = (size_t)(end - start);
		VmStatus status = wibo_heap_virtualReset(requestedAddress, requestedSize);
		if (status == VmStatus_Success) {
			if (baseAddress) {
				*baseAddress = (void *)start;
			}
			*regionSize = length;
		}
		return status;
	}

	if (!reserve && !commit) {
		return VmStatus_InvalidParameter;
	}

	uintptr_t base = 0;
	size_t length = 0;
	pthread_mutex_lock(&g_mappingsMutex);
	VmStatus status = reserve ? reserveRegionLocked(requestedAddress, requestedSize, commit, protect, type, topDown,
													&base, &length)
							  : commitRangeLocked(requestedAddress, requestedSize, protect, &base, &length);
	pthread_mutex_unlock(&g_mappingsMutex);
	if (status != VmStatus_Success) {
		return status;
	}
	if (baseAddress) {
		*baseAddress = (void *)base;
	}
	*regionSize = length;
	return VmStatus_Success;
}

// Requires g_mappingsMutex.
static VmStatus releaseRegionLocked(void *baseAddress, size_t regionSize) {
	uintptr_t base = (uintptr_t)baseAddress;
	size_t idx = virtualAllocationFind(base);
	if (idx == NO_INDEX) {
		if (regionSize != 0 && findRegionIndex(base) != NO_INDEX) {
			return VmStatus_InvalidParameter;
		}
		return VmStatus_InvalidAddress;
	}
	if (regionSize != 0) {
		return VmStatus_InvalidParameter;
	}
	size_t length = g_virtualAllocations.data[idx].size;
	virtualAllocationErase(idx);
	// Replace with PROT_NONE + MAP_NORESERVE to release physical memory
	void *res = mmap((void *)base, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
	if (res == MAP_FAILED) {
		return vmStatusFromErrno(errno);
	}
	setVirtualAllocationName(res, length, "wibo reserved");
	eraseGuestMappingLocked(base);
	return VmStatus_Success;
}

// Requires g_mappingsMutex.
static VmStatus decommitRangeLocked(void *baseAddress, size_t regionSize) {
	const uintptr_t pageSize = wibo_heap_systemPageSize();
	uintptr_t request = (uintptr_t)baseAddress;
	size_t idx = findRegionIndex(request);
	if (idx == NO_INDEX) {
		return VmStatus_InvalidAddress;
	}
	VirtualAllocation *region = &g_virtualAllocations.data[idx];
	uintptr_t start = alignDown(request, pageSize);
	uintptr_t end = 0;
	if (regionSize == 0) {
		if (request != region->base) {
			return VmStatus_InvalidParameter;
		}
		start = region->base;
		end = region->base + region->size;
	} else {
		if (addOverflows(request, regionSize)) {
			return VmStatus_InvalidParameter;
		}
		end = alignUp(request + (uintptr_t)regionSize, pageSize);
	}
	if (end <= start) {
		return VmStatus_InvalidParameter;
	}
	size_t length = (size_t)(end - start);
	if (!rangeWithinRegion(region, start, length)) {
		return VmStatus_InvalidAddress;
	}
	void *res = mmap((void *)start, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
	if (res == MAP_FAILED) {
		return vmStatusFromErrno(errno);
	}
	setVirtualAllocationName(res, length, "wibo reserved");
	setPageProtect(region, start, length, 0);
	refreshGuestMappingLocked(region);
	return VmStatus_Success;
}

VmStatus wibo_heap_virtualFree(void *baseAddress, size_t regionSize, DWORD freeType) {
	if (!baseAddress) {
		return VmStatus_InvalidAddress;
	}
	if ((freeType & (MEM_COALESCE_PLACEHOLDERS | MEM_PRESERVE_PLACEHOLDER)) != 0) {
		return VmStatus_NotSupported;
	}

	const bool release = (freeType & MEM_RELEASE) != 0;
	const bool decommit = (freeType & MEM_DECOMMIT) != 0;
	if (release == decommit) {
		return VmStatus_InvalidParameter;
	}

	pthread_mutex_lock(&g_mappingsMutex);
	VmStatus status = release ? releaseRegionLocked(baseAddress, regionSize)
							  : decommitRangeLocked(baseAddress, regionSize);
	pthread_mutex_unlock(&g_mappingsMutex);
	return status;
}

// Requires g_mappingsMutex.
static VmStatus protectRangeLocked(uintptr_t start, uintptr_t end, DWORD newProtect, DWORD *oldProtect) {
	const uintptr_t pageSize = wibo_heap_systemPageSize();
	VirtualAllocation *region = lookupRegion(start);
	if (!region || !rangeWithinRegion(region, start, (size_t)(end - start))) {
		return VmStatus_InvalidAddress;
	}

	const size_t firstPage = (start - region->base) / pageSize;
	const size_t pageCount = (end - start) / pageSize;
	if (pageCount == 0) {
		return VmStatus_InvalidParameter;
	}

	DWORD previousProtect = region->pageProtect[firstPage];
	for (size_t i = 0; i < pageCount; ++i) {
		if (region->pageProtect[firstPage + i] == 0) {
			return VmStatus_NoAccess;
		}
	}

	int prot = posixProtectFromWin32(newProtect);
	if (mprotect((void *)start, end - start, prot) != 0) {
		return vmStatusFromErrno(errno);
	}
	for (size_t i = 0; i < pageCount; ++i) {
		region->pageProtect[firstPage + i] = newProtect;
	}
	refreshGuestMappingLocked(region);

	if (oldProtect) {
		*oldProtect = previousProtect;
	}
	return VmStatus_Success;
}

VmStatus wibo_heap_virtualProtect(void *baseAddress, size_t regionSize, DWORD newProtect, DWORD *oldProtect) {
	if (!baseAddress || regionSize == 0) {
		return VmStatus_InvalidParameter;
	}

	const uintptr_t pageSize = wibo_heap_systemPageSize();
	uintptr_t request = (uintptr_t)baseAddress;
	uintptr_t start = alignDown(request, pageSize);
	uintptr_t end = alignUp(request + (uintptr_t)regionSize, pageSize);
	if (end <= start) {
		return VmStatus_InvalidParameter;
	}

	pthread_mutex_lock(&g_mappingsMutex);
	VmStatus status = protectRangeLocked(start, end, newProtect, oldProtect);
	pthread_mutex_unlock(&g_mappingsMutex);
	return status;
}

VmStatus wibo_heap_virtualQuery(const void *address, MEMORY_BASIC_INFORMATION *outInfo) {
	if (!outInfo) {
		return VmStatus_InvalidParameter;
	}

	const uintptr_t pageSize = wibo_heap_systemPageSize();
	uintptr_t request = address ? (uintptr_t)address : 0;
	if (request >= kTwoGB) {
		return VmStatus_InvalidParameter;
	}
	uintptr_t pageBase = alignDown(request, pageSize);

	pthread_mutex_lock(&g_mappingsMutex);
	const VirtualAllocation *region = lookupRegion(pageBase);
	if (!region) {
		uintptr_t regionStart = pageBase;
		size_t next = virtualAllocationBound(pageBase);
		uintptr_t regionEnd = next < g_virtualAllocations.len ? g_virtualAllocations.data[next].base : kTwoGB;
		if (regionEnd <= regionStart) {
			regionEnd = regionStart + pageSize;
		}
		pthread_mutex_unlock(&g_mappingsMutex);
		outInfo->BaseAddress = toGuestPtr((void *)regionStart);
		outInfo->AllocationBase = GUEST_NULL;
		outInfo->AllocationProtect = 0;
		outInfo->RegionSize = regionEnd - regionStart;
		outInfo->State = MEM_FREE;
		outInfo->Protect = PAGE_NOACCESS;
		outInfo->Type = 0;
		return VmStatus_Success;
	}

	const uintptr_t regionLimit = region->base + region->size;
	const size_t pageIndex = (pageBase - region->base) / pageSize;
	const DWORD pageProtect = region->pageProtect[pageIndex];
	const bool committed = pageProtect != 0;
	uintptr_t blockStart = pageBase;
	uintptr_t blockEnd = pageBase + pageSize;
	while (blockStart > region->base) {
		DWORD protect = region->pageProtect[(blockStart - region->base) / pageSize - 1];
		if ((protect != 0) != committed || (committed && protect != pageProtect)) {
			break;
		}
		blockStart -= pageSize;
	}
	while (blockEnd < regionLimit) {
		DWORD protect = region->pageProtect[(blockEnd - region->base) / pageSize];
		if ((protect != 0) != committed || (committed && protect != pageProtect)) {
			break;
		}
		blockEnd += pageSize;
	}
	DWORD allocationProtect = region->allocationProtect != 0 ? region->allocationProtect : PAGE_NOACCESS;
	DWORD finalProtect = committed ? pageProtect : PAGE_NOACCESS;
	DWORD type = region->type;
	uintptr_t allocationBase = region->base;
	pthread_mutex_unlock(&g_mappingsMutex);

	outInfo->BaseAddress = toGuestPtr((void *)blockStart);
	outInfo->AllocationBase = toGuestPtr((void *)allocationBase);
	outInfo->AllocationProtect = allocationProtect;
	outInfo->RegionSize = blockEnd - blockStart;
	outInfo->State = committed ? MEM_COMMIT : MEM_RESERVE;
	outInfo->Protect = finalProtect;
	outInfo->Type = type;
	return VmStatus_Success;
}

VmStatus wibo_heap_reserveViewRange(size_t regionSize, uintptr_t minAddr, uintptr_t maxAddr, void **baseAddress) {
	if (!baseAddress || regionSize == 0) {
		return VmStatus_InvalidParameter;
	}
	const uintptr_t pageSize = wibo_heap_systemPageSize();
	size_t aligned = (size_t)alignUp((uintptr_t)regionSize, pageSize);
	if (aligned == 0) {
		return VmStatus_InvalidParameter;
	}
	if (minAddr == 0) {
		minAddr = kLowMemoryStart;
	}
	if (maxAddr == 0) {
		maxAddr = kTopDownStart;
	}
	if (minAddr >= maxAddr || aligned > (maxAddr - minAddr)) {
		return VmStatus_InvalidParameter;
	}
	uintptr_t candidate = 0;
	pthread_mutex_lock(&g_mappingsMutex);
	bool found = findFreeMappingLocked(aligned, minAddr, maxAddr, false, &candidate);
	if (found) {
		recordGuestMappingLocked(candidate, aligned, PAGE_NOACCESS, MEM_RESERVE, PAGE_NOACCESS, MEM_MAPPED);
	}
	pthread_mutex_unlock(&g_mappingsMutex);
	if (!found) {
		return VmStatus_NoMemory;
	}
	*baseAddress = (void *)candidate;
	return VmStatus_Success;
}

VmStatus wibo_heap_reserveViewRangeAt(void *baseAddress, size_t regionSize) {
	if (!baseAddress || regionSize == 0) {
		return VmStatus_InvalidParameter;
	}
	const uintptr_t pageSize = wibo_heap_systemPageSize();
	size_t aligned = (size_t)alignUp((uintptr_t)regionSize, pageSize);
	if (aligned == 0) {
		return VmStatus_InvalidParameter;
	}
	uintptr_t base = (uintptr_t)baseAddress;
	if (base >= kTwoGB || aligned > (kTwoGB - base)) {
		return VmStatus_InvalidAddress;
	}
	pthread_mutex_lock(&g_mappingsMutex);
	bool overlaps = overlapsExistingMappingLocked(base, aligned);
	if (!overlaps) {
		recordGuestMappingLocked(base, aligned, PAGE_NOACCESS, MEM_RESERVE, PAGE_NOACCESS, MEM_MAPPED);
	}
	pthread_mutex_unlock(&g_mappingsMutex);
	return overlaps ? VmStatus_InvalidAddress : VmStatus_Success;
}

void wibo_heap_registerViewRange(void *baseAddress, size_t regionSize, DWORD allocationProtect, DWORD protect) {
	if (!baseAddress) {
		return;
	}
	const uintptr_t pageSize = wibo_heap_systemPageSize();
	size_t aligned = (size_t)alignUp((uintptr_t)regionSize, pageSize);
	pthread_mutex_lock(&g_mappingsMutex);
	recordGuestMappingLocked((uintptr_t)baseAddress, aligned, allocationProtect, MEM_COMMIT, protect, MEM_MAPPED);
	pthread_mutex_unlock(&g_mappingsMutex);
}

void wibo_heap_releaseViewRange(void *baseAddress, size_t regionSize) {
	if (!baseAddress) {
		return;
	}
	const uintptr_t pageSize = wibo_heap_systemPageSize();
	size_t aligned = (size_t)alignUp((uintptr_t)regionSize, pageSize);
	pthread_mutex_lock(&g_mappingsMutex);
	// Put the PROT_NONE placeholder back, as virtualFree does, so an unrelated host mmap can't land here
	void *res = mmap(baseAddress, aligned, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
	if (res != MAP_FAILED) {
		setVirtualAllocationName(res, aligned, "wibo reserved");
	}
	eraseGuestMappingLocked((uintptr_t)baseAddress);
	pthread_mutex_unlock(&g_mappingsMutex);
}

bool wibo_heap_reserveGuestStack(size_t stackSizeBytes, void **outStackLimit, void **outStackBase) {
	const size_t ps = wibo_heap_systemPageSize();
	size_t total = ((stackSizeBytes + (ps * 2) - 1) & ~(ps - 1));

	void *start = NULL;
	size_t size = 0;
	if (!mapArena(total, kLowMemoryStart, kTwoGB, true, "wibo guest stack", &start, &size)) {
		DEBUG_LOG("heap: reserveGuestStack: failed to map region\n");
		return false;
	}

	// Protect the guard page at the bottom of the mapped region
	if (mprotect(start, ps, PROT_NONE) != 0) {
		// Non-fatal; continue without guard
		DEBUG_LOG("heap: reserveGuestStack: mprotect guard failed\n");
	}

	// Stack grows downwards; limit is after guard, base is top of mapping
	void *limit = (char *)start + ps;
	void *base = (char *)start + size;
	*outStackLimit = limit;
	*outStackBase = base;
	DEBUG_LOG("heap: reserved guest stack limit=%p base=%p (total=%zu KiB)\n", limit, base, size >> 10);
	return true;
}

typedef struct SubHeap SubHeap;

// Live blocks are linked into their owning SubHeap so Heap_destroy can release them.
// Free blocks are chained through `next`.
typedef struct BlockHeader {
	struct BlockHeader *prev;
	struct BlockHeader *next;
	SubHeap *owner;
	size_t size;
	uint32_t shift;
} BlockHeader;

// Keeps payloads 8-byte aligned.
#define HEADER_SIZE ((sizeof(BlockHeader) + 7) & ~(size_t)7)
#define MIN_BLOCK_SHIFT 6
#define MAX_BLOCK_SHIFT 24
#define NUM_BLOCK_CLASSES (MAX_BLOCK_SHIFT - MIN_BLOCK_SHIFT + 1)

typedef struct {
	void *start;
	size_t size;
	size_t bump;
	BlockHeader *freeLists[NUM_BLOCK_CLASSES];
} Arena;
typedef VEC(Arena) ArenaVec;

struct SubHeap {
	uint32_t arenaIdx;
	uint32_t heapTag;
	BlockHeader live;
};

static pthread_mutex_t g_arenasMutex;
static pthread_once_t g_arenasMutexOnce = PTHREAD_ONCE_INIT;
static ArenaVec g_arenas;
static _Atomic uint32_t g_heapTag = 1;

static void initArenasMutex(void) {
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&g_arenasMutex, &attr);
	pthread_mutexattr_destroy(&attr);
}

static void arenasLock(void) {
	pthread_once(&g_arenasMutexOnce, initArenasMutex);
	pthread_mutex_lock(&g_arenasMutex);
}

static void arenasUnlock(void) { pthread_mutex_unlock(&g_arenasMutex); }

static bool createArenaLocked(size_t size) {
	Arena arena = {0};
	if (!mapArena(size, kLowMemoryStart, kHeapMax, true, "wibo heap arena", &arena.start, &arena.size)) {
		DEBUG_LOG("heap: failed to find free mapping for arena\n");
		return false;
	}
	DEBUG_LOG("heap: created arena at %p..%p (%zu MiB)\n", arena.start, (char *)arena.start + arena.size,
			  arena.size >> 20);
	vec_push(&g_arenas, arena);
	return true;
}

static void *payloadOf(BlockHeader *block) { return (char *)block + HEADER_SIZE; }
static BlockHeader *headerOf(const void *ptr) { return (BlockHeader *)((char *)ptr - HEADER_SIZE); }
static size_t blockCapacity(uint32_t shift) { return ((size_t)1 << shift) - HEADER_SIZE; }

static uint32_t blockShiftFor(size_t size) {
	size_t total = size + HEADER_SIZE;
	uint32_t shift = MIN_BLOCK_SHIFT;
	while (((size_t)1 << shift) < total) {
		++shift;
	}
	return shift;
}

static BlockHeader *takeBlockLocked(Arena *arena, uint32_t shift) {
	BlockHeader **freeList = &arena->freeLists[shift - MIN_BLOCK_SHIFT];
	if (*freeList) {
		BlockHeader *block = *freeList;
		*freeList = block->next;
		return block;
	}
	const size_t align = (size_t)1 << MIN_BLOCK_SHIFT;
	size_t off = (arena->bump + align - 1) & ~(align - 1);
	size_t blockSize = (size_t)1 << shift;
	if (off > arena->size || blockSize > arena->size - off) {
		return NULL;
	}
	arena->bump = off + blockSize;
	return (BlockHeader *)((char *)arena->start + off);
}

static void liveInsertLocked(SubHeap *sub, BlockHeader *block) {
	block->owner = sub;
	block->prev = &sub->live;
	block->next = sub->live.next;
	sub->live.next->prev = block;
	sub->live.next = block;
}

static void freeBlockLocked(BlockHeader *block) {
	block->prev->next = block->next;
	block->next->prev = block->prev;
	Arena *arena = &g_arenas.data[block->owner->arenaIdx];
	BlockHeader **freeList = &arena->freeLists[block->shift - MIN_BLOCK_SHIFT];
	block->next = *freeList;
	*freeList = block;
}

// Every returned region is zero-filled, matching the fresh pages Wine hands out.
static void *allocInSubLocked(SubHeap *sub, size_t size) {
	if (size >= LARGE_ALLOC_THRESHOLD) {
		return NULL;
	}
	uint32_t shift = blockShiftFor(size);
	BlockHeader *block = takeBlockLocked(&g_arenas.data[sub->arenaIdx], shift);
	if (!block) {
		return NULL;
	}
	block->size = size;
	block->shift = shift;
	liveInsertLocked(sub, block);
	void *ptr = payloadOf(block);
	memset(ptr, 0, size);
	return ptr;
}

static void *reallocInSubLocked(SubHeap *sub, void *ptr, size_t newSize) {
	BlockHeader *block = headerOf(ptr);
	size_t oldSize = block->size;
	if (newSize <= blockCapacity(block->shift)) {
		block->size = newSize;
		if (newSize > oldSize) {
			memset((char *)ptr + oldSize, 0, newSize - oldSize);
		}
		return ptr;
	}
	void *newPtr = allocInSubLocked(sub, newSize);
	if (!newPtr) {
		return NULL;
	}
	memcpy(newPtr, ptr, oldSize);
	freeBlockLocked(block);
	return newPtr;
}

static void heapBlockFree(void *ptr) {
	arenasLock();
	freeBlockLocked(headerOf(ptr));
	arenasUnlock();
}

static size_t usableSize(const void *ptr) { return blockCapacity(headerOf(ptr)->shift); }

static bool isInHeapRegion(const void *ptr) {
	uintptr_t addr = (uintptr_t)ptr;
	bool found = false;
	arenasLock();
	for (size_t i = 0; i < g_arenas.len && !found; ++i) {
		uintptr_t start = (uintptr_t)g_arenas.data[i].start;
		found = addr >= start && addr - start < g_arenas.data[i].size;
	}
	arenasUnlock();
	return found;
}

static void subHeapDestroy(SubHeap *sub) {
	arenasLock();
	while (sub->live.next != &sub->live) {
		freeBlockLocked(sub->live.next);
	}
	arenasUnlock();
	free(sub);
}

typedef VEC(SubHeap *) HeapVec;

// Each thread gets its own SubHeap per arena it has allocated from
typedef struct {
	uint32_t heapTag;
	// Previously used arena
	uint32_t arenaHint;
	HeapVec heaps;
} HeapInternal;

static __thread HeapInternal g_guestHeap;

static SubHeap *heapForArena(HeapVec *heaps, uint32_t arenaIdx, uint32_t heapTag) {
	if (heaps->len <= arenaIdx) {
		size_t oldLen = heaps->len;
		size_t newLen = (size_t)arenaIdx + 1;
		vec_reserve(heaps, newLen - oldLen);
		memset(&heaps->data[oldLen], 0, (newLen - oldLen) * sizeof *heaps->data);
		heaps->len = newLen;
	}
	if (heaps->data[arenaIdx] == NULL) {
		arenasLock();
		bool arenaExists = arenaIdx < g_arenas.len;
		arenasUnlock();
		if (!arenaExists) {
			return NULL;
		}
		SubHeap *sub = xmalloc(sizeof *sub);
		sub->arenaIdx = arenaIdx;
		sub->heapTag = heapTag;
		sub->live.prev = &sub->live;
		sub->live.next = &sub->live;
		heaps->data[arenaIdx] = sub;
	}
	return heaps->data[arenaIdx];
}

typedef struct {
	void *ptr;
	size_t size;
} HeapOp;
typedef void *(*HeapOpFn)(SubHeap *sub, const HeapOp *op);

static void *heapOpAlloc(SubHeap *sub, const HeapOp *op) {
	arenasLock();
	void *ptr = allocInSubLocked(sub, op->size);
	arenasUnlock();
	return ptr;
}

static void *heapOpRealloc(SubHeap *sub, const HeapOp *op) {
	arenasLock();
	void *ptr = reallocInSubLocked(sub, op->ptr, op->size);
	arenasUnlock();
	return ptr;
}

static void *tryWithArena(HeapInternal *internal, uint32_t arenaIdx, HeapOpFn fn, const HeapOp *op) {
	SubHeap *sub = heapForArena(&internal->heaps, arenaIdx, internal->heapTag);
	if (!sub) {
		return NULL;
	}
	return fn(sub, op);
}

static void *tryWithAnyArena(HeapInternal *internal, HeapOpFn fn, const HeapOp *op) {
	void *ret = tryWithArena(internal, internal->arenaHint, fn, op);
	if (ret) {
		return ret;
	}
	// Loop without locking (arenas won't be removed)
	uint32_t numArenas = (uint32_t)g_arenas.len;
	for (uint32_t i = 0; i < numArenas; ++i) {
		if (i == internal->arenaHint) {
			continue;
		}
		ret = tryWithArena(internal, i, fn, op);
		if (ret) {
			internal->arenaHint = i;
			return ret;
		}
	}
	arenasLock();
	// Was a new arena created while we were looping?
	for (uint32_t i = numArenas; i < g_arenas.len; ++i) {
		ret = tryWithArena(internal, i, fn, op);
		if (ret) {
			internal->arenaHint = i;
			arenasUnlock();
			return ret;
		}
	}
	DEBUG_LOG("heap: no arena available, creating new arena\n");
	if (createArenaLocked(GUEST_ARENA_SIZE)) {
		uint32_t newArenaIdx = (uint32_t)(g_arenas.len - 1);
		ret = tryWithArena(internal, newArenaIdx, fn, op);
		if (ret) {
			internal->arenaHint = newArenaIdx;
			arenasUnlock();
			return ret;
		}
	}
	arenasUnlock();
	return NULL;
}

static void *doAlloc(HeapInternal *internal, size_t size) {
	if (size >= LARGE_ALLOC_THRESHOLD) {
		DEBUG_LOG("heap: large malloc %zu bytes, using virtualAlloc\n", size);
		void *addr = NULL;
		if (wibo_heap_virtualAlloc(&addr, &size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, MEM_PRIVATE) !=
			VmStatus_Success) {
			return NULL;
		}
		return addr;
	}
	const HeapOp op = {.ptr = NULL, .size = size};
	return tryWithAnyArena(internal, heapOpAlloc, &op);
}

static void *doRealloc(HeapInternal *internal, void *ptr, size_t newSize) {
	bool isInHeap = isInHeapRegion(ptr);
	if (newSize >= LARGE_ALLOC_THRESHOLD || !isInHeap) {
		DEBUG_LOG("heap: large realloc %zu bytes, using virtualAlloc\n", newSize);
		size_t oldSize;
		if (isInHeap) {
			oldSize = usableSize(ptr);
		} else {
			// Get size from virtualQuery
			MEMORY_BASIC_INFORMATION info;
			if (wibo_heap_virtualQuery(ptr, &info) != VmStatus_Success) {
				return NULL;
			}
			oldSize = info.RegionSize;
		}
		void *ret = NULL;
		if (newSize >= LARGE_ALLOC_THRESHOLD) {
			if (wibo_heap_virtualAlloc(&ret, &newSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, MEM_PRIVATE) !=
				VmStatus_Success) {
				return NULL;
			}
		} else {
			ret = doAlloc(internal, newSize);
		}
		if (!ret) {
			return NULL;
		}
		// newSize may have been page-rounded by virtualAlloc above
		memcpy(ret, ptr, oldSize < newSize ? oldSize : newSize);
		if (isInHeap) {
			heapBlockFree(ptr);
		} else if (wibo_heap_virtualFree(ptr, 0, MEM_RELEASE) != VmStatus_Success) {
			return NULL;
		}
		return ret;
	}
	const HeapOp op = {.ptr = ptr, .size = newSize};
	return tryWithAnyArena(internal, heapOpRealloc, &op);
}

static bool doFree(void *ptr) {
	if (ptr == NULL) {
		return false;
	}
	if (isInHeapRegion(ptr)) {
		heapBlockFree(ptr);
	} else {
		DEBUG_LOG("heap: free(%p) -> virtualFree\n", ptr);
		if (wibo_heap_virtualFree(ptr, 0, MEM_RELEASE) != VmStatus_Success) {
			return false;
		}
	}
	return true;
}

struct Heap {
	uint32_t threadId;
	HeapInternal internal;
};

Heap *Heap_create(void) {
	Heap *heap = xmalloc(sizeof *heap);
	heap->threadId = wibo_getThreadId();
	heap->internal.heapTag = atomic_fetch_add(&g_heapTag, 1);
	heap->internal.arenaHint = 0;
	heap->internal.heaps.data = NULL;
	heap->internal.heaps.len = 0;
	heap->internal.heaps.cap = 0;
	return heap;
}

void Heap_destroy(Heap *heap) {
	if (!heap) {
		return;
	}
	if (wibo_getThreadId() != heap->threadId) {
		DEBUG_LOG("heap: ~Heap() failed; heap owned by another thread\n");
	} else {
		for (size_t i = 0; i < heap->internal.heaps.len; ++i) {
			if (heap->internal.heaps.data[i]) {
				subHeapDestroy(heap->internal.heaps.data[i]);
			}
		}
	}
	vec_free(&heap->internal.heaps);
	free(heap);
}

void *Heap_malloc(Heap *heap, size_t size) {
	if (wibo_getThreadId() != heap->threadId) {
		DEBUG_LOG("heap: malloc(%zu) failed; heap owned by another thread\n", size);
		return NULL;
	}
	return doAlloc(&heap->internal, size);
}

void *Heap_realloc(Heap *heap, void *ptr, size_t newSize) {
	if (wibo_getThreadId() != heap->threadId) {
		DEBUG_LOG("heap: realloc(%p, %zu) failed; heap owned by another thread\n", ptr, newSize);
		return NULL;
	}
	return doRealloc(&heap->internal, ptr, newSize);
}

// Matches the C++ original: freeing does not check the owning thread.
bool Heap_free(Heap *heap, void *ptr) {
	(void)heap;
	return doFree(ptr);
}

void *wibo_heap_guestMalloc(size_t size) { return doAlloc(&g_guestHeap, size); }

void *wibo_heap_guestRealloc(void *ptr, size_t newSize) { return doRealloc(&g_guestHeap, ptr, newSize); }

bool wibo_heap_guestFree(void *ptr) { return doFree(ptr); }

size_t wibo_heap_guestSize(const void *ptr) {
	if (isInHeapRegion(ptr)) {
		return usableSize(ptr);
	}
	MEMORY_BASIC_INFORMATION info;
	if (wibo_heap_virtualQuery(ptr, &info) != VmStatus_Success) {
		return SIZE_MAX;
	}
	return info.RegionSize;
}

#ifdef __linux__
static void debugPrintMaps(void) {
	char buf[1024];
	int fd = open("/proc/self/maps", O_RDONLY);
	if (fd == -1) {
		LOG_ERR("heap: failed to open /proc/self/maps\n");
		return;
	}
	while (true) {
		ssize_t r = read(fd, buf, sizeof(buf));
		if (r == 0) {
			break;
		} else if (r == -1) {
			LOG_ERR("heap: failed to read /proc/self/maps\n");
			close(fd);
			return;
		}
		(void)write(STDERR_FILENO, buf, (size_t)r);
	}
	close(fd);
}

// Reads /proc/self/maps into buffer without allocating: libc allocations could modify the mappings being read.
static size_t readMaps(char *buffer) {
	int fd = open("/proc/self/maps", O_RDONLY);
	if (fd == -1) {
		perror("heap: failed to open /proc/self/maps");
		exit(1);
	}

	char *cur = buffer;
	char *bufferEnd = buffer + MAPS_BUFFER_SIZE;
	while (cur < bufferEnd) {
		ssize_t ret = read(fd, cur, (size_t)(bufferEnd - cur));
		if (ret == -1) {
			if (errno == EINTR) {
				continue;
			}
			perror("heap: failed to read /proc/self/maps");
			exit(1);
		} else if (ret == 0) {
			break;
		}
		cur += ret;
	}
	close(fd);

	if (cur == bufferEnd) {
		fprintf(stderr, "heap: buffer too small while reading /proc/self/maps\n");
		exit(1);
	}
	*cur = '\0';
	return (size_t)(cur - buffer);
}

// Parses hex digits at *p (before end) and advances *p. Fails on no digits or on overflow.
static bool parseHexUintptr(const char **p, const char *end, uintptr_t *out) {
	const char *cur = *p;
	uintptr_t value = 0;
	size_t digits = 0;
	while (cur < end) {
		char c = *cur;
		uintptr_t d;
		if (c >= '0' && c <= '9') {
			d = (uintptr_t)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			d = (uintptr_t)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			d = (uintptr_t)(c - 'A' + 10);
		} else {
			break;
		}
		if (value > (UINTPTR_MAX >> 4)) {
			return false;
		}
		value = (value << 4) | d;
		++cur;
		++digits;
	}
	if (digits == 0) {
		return false;
	}
	*p = cur;
	*out = value;
	return true;
}

// Maps the lower 2GB of memory to prevent libc from allocating there.
// 32-bit Windows only reserves the lowest 2GB for a process, and some programs rely on heap allocations landing there.
// Between reading /proc/self/maps and reserving, we must not allocate, as that could modify the mappings.
static size_t blockLower2GB(MEMORY_BASIC_INFORMATION mappings[MAX_NUM_MAPPINGS]) {
	char buffer[MAPS_BUFFER_SIZE];
	size_t len = readMaps(buffer);
	const char *procLine = buffer;
	const char *bufferEnd = buffer + len;
	uintptr_t lastMapEnd = g_mmapMinAddr;
	size_t numMappings = 0;
	while (true) {
		const char *newline = memchr(procLine, '\n', (size_t)(bufferEnd - procLine));
		if (newline == NULL) {
			break;
		}

		uintptr_t mapStart = 0;
		const char *lineStart = procLine;
		const char *lineEnd = newline;
		procLine = newline + 1;
		const char *cur = lineStart;
		if (!parseHexUintptr(&cur, lineEnd, &mapStart)) {
			break;
		}
		if (cur >= lineEnd || *cur != '-') {
			continue;
		}
		uintptr_t mapEnd = 0;
		++cur;
		if (!parseHexUintptr(&cur, lineEnd, &mapEnd)) {
			break;
		}
		if (mapStart >= kTwoGB) {
			break;
		}
		if (mapEnd > kTwoGB) {
			mapEnd = kTwoGB;
		}
		if (mapStart == mapEnd || mapStart > mapEnd) {
			continue;
		}

		if (numMappings < MAX_NUM_MAPPINGS) {
			if (numMappings > 0) {
				MEMORY_BASIC_INFORMATION *prevMapping = &mappings[numMappings - 1];
				uintptr_t prevMapStart = (uintptr_t)fromGuestPtr(prevMapping->BaseAddress);
				uintptr_t prevMapEnd = prevMapStart + prevMapping->RegionSize;
				if (mapStart <= prevMapEnd) {
					// Extend the previous mapping
					prevMapping->RegionSize = mapEnd - prevMapStart;
					lastMapEnd = mapEnd;
					continue;
				}
			}
			mappings[numMappings++] = (MEMORY_BASIC_INFORMATION){
				.BaseAddress = toGuestPtr((void *)mapStart),
				.AllocationBase = toGuestPtr((void *)mapStart),
				.AllocationProtect = PAGE_NOACCESS,
				.RegionSize = (SIZE_T)(mapEnd - mapStart),
				.State = MEM_RESERVE,
				.Protect = PAGE_NOACCESS,
				.Type = 0, // external
			};
		}

		// The empty space we want to map out is now between lastMapEnd and mapStart
		reserveGuestRange(lastMapEnd, mapStart);
		lastMapEnd = mapEnd;
	}
	reserveGuestRange(lastMapEnd, kTwoGB);

	return numMappings;
}

static bool g_addressSpaceReserved = false;

static void initializeHostHeap(void) {
	if (!g_addressSpaceReserved) {
		g_mmapMinAddr = readMmapMinAddr();
		g_numInitialMappings = blockLower2GB(g_initialMappings);
		g_addressSpaceReserved = true;
	}
	pthread_once(&g_arenasMutexOnce, initArenasMutex);
}

#ifdef __GLIBC__
// Reserve guest addresses and initialize the heap before shared-library constructors use malloc/free.
__attribute__((section(".preinit_array"), used)) static void (*const kHeapPreinit)(void) = initializeHostHeap;
#endif
#endif

#if defined(__clang__)
__attribute__((constructor(101)))
#else
__attribute__((constructor))
#endif
__attribute__((used)) static void wibo_heap_constructor(void) {
#ifdef __linux__
	// musl does not run .preinit_array; reserve here before this constructor allocates.
	initializeHostHeap();
#endif
	bool debug = getenv("WIBO_DEBUG_HEAP") != NULL;
	if (debug) {
		LOG_OUT("heap: initializing...\n");
#ifdef __linux__
		debugPrintMaps();
#endif
	}
	// Mark DOS area as read-only
	const uintptr_t minAddr = g_mmapMinAddr;
	if (minAddr < kLowMemoryStart) {
		size_t len = (size_t)(kLowMemoryStart - minAddr);
		void *ptr = mmap((void *)minAddr, len, PROT_READ,
						 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
		if (ptr == MAP_FAILED) {
			LOG_ERR("heap: failed to allocate DOS area\n");
		} else {
			setVirtualAllocationName(ptr, len, "wibo DOS area");
		}
	}
	g_mappingsReady = true;
#ifdef __linux__
	for (size_t i = 0; i < g_numInitialMappings; ++i) {
		const MEMORY_BASIC_INFORMATION *mapping = &g_initialMappings[i];
		if (debug) {
			fprintf(stderr, "Existing %zu: BaseAddress=%p, RegionSize=%u\n", i, (void *)(uintptr_t)mapping->BaseAddress,
					(unsigned)mapping->RegionSize);
		}
		mappingPut((uintptr_t)fromGuestPtr(mapping->BaseAddress), mapping);
	}
#endif
}
