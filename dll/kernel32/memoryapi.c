#include "memoryapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "heap.h"
#include "internal.h"
#include "strutil.h"
#include "types.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const size_t kVirtualAllocationGranularity = 64 * 1024;
static const uintptr_t kProcessAddressLimit = 0x80000000;
static const uintptr_t kHighestUserAddress = 0x7FFEFFFF;
static const uintptr_t kSharedUserDataAddr = 0x7FFE0000;
static const uint64_t kMaxOffT = sizeof(off_t) == 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX;

typedef struct {
	ObjectBase base;
	pthread_mutex_t m;
	int fd;
	size_t maxSize;
	DWORD protect;
	bool anonymous;
	bool closed;
} MappingObject;

static void MappingObject_destroy(ObjectBase *base) {
	MappingObject *self = (MappingObject *)base;
	if (self->fd != -1) {
		close(self->fd);
		self->fd = -1;
	}
	pthread_mutex_destroy(&self->m);
	free(self);
}

static MappingObject *MappingObject_alloc(void) {
	MappingObject *self = xcalloc(1, sizeof(*self));
	ObjectBase_init(&self->base, ObjectType_Mapping, MappingObject_destroy);
	pthread_mutex_init(&self->m, NULL);
	self->fd = -1;
	return self;
}

typedef struct {
	uintptr_t viewBase;
	size_t viewLength;
	uintptr_t allocationBase;
	size_t allocationLength;
	MappingObject *owner; // holds a reference while the view is registered
	DWORD protect;
	DWORD allocationProtect;
	DWORD type;
} ViewInfo;

// Kept sorted by key (the view base address), as an ordered map would be.
typedef struct {
	uintptr_t key;
	ViewInfo info;
} ViewEntry;

typedef VEC(ViewEntry) ViewEntryVec;

static ViewEntryVec g_viewInfo;
static pthread_mutex_t g_viewInfoMutex = PTHREAD_MUTEX_INITIALIZER;

// First index whose key is not less than `key`.
static size_t viewLowerBound(uintptr_t key) {
	size_t lo = 0;
	size_t hi = g_viewInfo.len;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (g_viewInfo.data[mid].key < key) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

// First index whose key is greater than `key`.
static size_t viewUpperBound(uintptr_t key) {
	size_t lo = 0;
	size_t hi = g_viewInfo.len;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (g_viewInfo.data[mid].key <= key) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return lo;
}

static uintptr_t alignDown(uintptr_t value, size_t alignment) {
	const uintptr_t mask = (uintptr_t)alignment - 1;
	return value & ~mask;
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

typedef struct {
	bool read;
	bool write;
	bool execute;
	bool copy;
} FileMapAccess;

static FileMapAccess fileMapAccessFromDesiredAccess(DWORD desiredAccess) {
	// FILE_MAP_ALL_ACCESS includes FILE_MAP_COPY's bit, but maps as a writable shared view.
	const bool allAccess = (desiredAccess & FILE_MAP_ALL_ACCESS) == FILE_MAP_ALL_ACCESS;
	FileMapAccess access = {0};
	access.read = allAccess || (desiredAccess & (FILE_MAP_READ | FILE_MAP_WRITE | FILE_MAP_COPY)) != 0;
	access.write = allAccess || (desiredAccess & FILE_MAP_WRITE) != 0;
	access.execute = (desiredAccess & FILE_MAP_EXECUTE) != 0;
	access.copy = !allAccess && (desiredAccess & FILE_MAP_COPY) != 0;
	if (access.copy) {
		access.write = true;
	}
	return access;
}

static DWORD desiredAccessToProtect(DWORD desiredAccess, DWORD mappingProtect) {
	FileMapAccess access = fileMapAccessFromDesiredAccess(desiredAccess);
	const bool supportsWrite = mappingProtect == PAGE_READWRITE || mappingProtect == PAGE_EXECUTE_READWRITE ||
							   mappingProtect == PAGE_WRITECOPY || mappingProtect == PAGE_EXECUTE_WRITECOPY;
	const bool supportsCopy = mappingProtect == PAGE_WRITECOPY || mappingProtect == PAGE_EXECUTE_WRITECOPY;

	if (access.copy && !supportsCopy) {
		access.copy = false;
	}
	if (access.write && !supportsWrite) {
		if (supportsCopy) {
			access.copy = true;
			access.write = false;
		} else {
			access.write = false;
		}
	}
	if (!access.read && (mappingProtect == PAGE_READONLY || mappingProtect == PAGE_EXECUTE_READ ||
						 mappingProtect == PAGE_WRITECOPY || mappingProtect == PAGE_EXECUTE_WRITECOPY)) {
		access.read = true;
	}

	DWORD protect = PAGE_NOACCESS;
	if (access.copy && supportsCopy) {
		protect = access.execute ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY;
	} else if (access.execute) {
		if (access.write) {
			protect = PAGE_EXECUTE_READWRITE;
		} else if (access.read) {
			protect = PAGE_EXECUTE_READ;
		} else {
			protect = PAGE_EXECUTE;
		}
	} else {
		if (access.write) {
			protect = PAGE_READWRITE;
		} else if (access.read) {
			protect = PAGE_READONLY;
		}
	}
	if ((mappingProtect & PAGE_NOCACHE) != 0) {
		protect |= PAGE_NOCACHE;
	}
	if ((mappingProtect & PAGE_GUARD) != 0) {
		protect |= PAGE_GUARD;
	}
	if ((mappingProtect & PAGE_WRITECOMBINE) != 0) {
		protect |= PAGE_WRITECOMBINE;
	}
	return protect;
}

static bool mappedViewRegionForAddress(uintptr_t request, uintptr_t pageBase, MEMORY_BASIC_INFORMATION *info) {
	pthread_mutex_lock(&g_viewInfoMutex);
	bool found = false;
	const size_t pageSize = wibo_heap_systemPageSize();
	for (size_t i = 0; i < g_viewInfo.len; i++) {
		const ViewInfo *view = &g_viewInfo.data[i].info;
		if (view->viewLength == 0) {
			continue;
		}
		uintptr_t allocationStart = view->allocationBase;
		uintptr_t allocationEnd = allocationStart + view->allocationLength;
		if (pageBase < allocationStart || pageBase >= allocationEnd) {
			continue;
		}
		uintptr_t viewStart = view->viewBase;
		uintptr_t viewEnd = view->viewBase + view->viewLength;
		if (request != 0 && (request < viewStart || request >= viewEnd)) {
			continue;
		}
		uintptr_t blockStart = viewStart;
		uintptr_t blockEnd = alignUp(viewEnd, pageSize);
		info->BaseAddress = toGuestPtr((void *)blockStart);
		info->AllocationBase = toGuestPtr((void *)view->viewBase);
		info->AllocationProtect = view->allocationProtect;
		info->RegionSize = blockEnd > blockStart ? blockEnd - blockStart : 0;
		info->State = MEM_COMMIT;
		info->Protect = view->protect;
		info->Type = view->type;
		found = true;
		break;
	}
	pthread_mutex_unlock(&g_viewInfoMutex);
	return found;
}

static bool protectAllowsFileGrowth(DWORD protect) {
	return protect == PAGE_READWRITE || protect == PAGE_EXECUTE_READWRITE;
}

static bool fileSizeFromFd(int fd, uint64_t *size) {
	struct stat st = {0};
	if (fstat(fd, &st) != 0) {
		kernel32_setLastErrorFromErrno();
		return false;
	}
	if (st.st_size < 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	*size = (uint64_t)st.st_size;
	return true;
}

static DWORD fileGrowthErrorFromErrno(int err) {
	switch (err) {
	case ENOSPC:
#ifdef EDQUOT
	case EDQUOT:
#endif
#ifdef EFBIG
	case EFBIG:
#endif
		return ERROR_DISK_FULL;
	default:
		return wibo_winErrorFromErrno(err);
	}
}

static bool growFileForMapping(int fd, uint64_t size) {
	if (size > kMaxOffT) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	if (ftruncate(fd, (off_t)size) != 0) {
		kernel32_setLastError(fileGrowthErrorFromErrno(errno));
		return false;
	}
	return true;
}

HANDLE WINAPI CreateFileMappingA(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect,
								 DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCSTR lpName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateFileMappingA(%p, %p, %u, %u, %u, %s)\n", hFile, lpFileMappingAttributes, flProtect,
			  dwMaximumSizeHigh, dwMaximumSizeLow, lpName ? lpName : "(null)");
	(void)lpFileMappingAttributes;
	(void)lpName;

	HANDLE ret = NO_HANDLE;
	FileObject *file = NULL;
	MappingObject *mapping = NULL;
	uint64_t size = ((uint64_t)dwMaximumSizeHigh << 32) | dwMaximumSizeLow;
	if (flProtect != PAGE_READONLY && flProtect != PAGE_READWRITE && flProtect != PAGE_WRITECOPY) {
		DEBUG_LOG("CreateFileMappingA: unsupported protection 0x%x\n", flProtect);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}

	mapping = MappingObject_alloc();
	mapping->protect = flProtect;

	if (hFile == INVALID_HANDLE_VALUE) {
		mapping->anonymous = true;
		mapping->fd = -1;
		if (size == 0) {
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			goto done;
		}
		mapping->maxSize = size;
	} else {
		file = (FileObject *)Handles_getWithFlag(wibo_handles(), hFile, Of_File, NULL);
		if (!file || !FsObject_valid(&file->base)) {
			kernel32_setLastError(ERROR_INVALID_HANDLE);
			goto done;
		}
		int dupFd = fcntl(file->base.fd, F_DUPFD_CLOEXEC, 0);
		if (dupFd == -1) {
			kernel32_setLastErrorFromErrno();
			goto done;
		}
		mapping->fd = dupFd;
		uint64_t fileSize = 0;
		if (!fileSizeFromFd(dupFd, &fileSize)) {
			goto done;
		}
		if (size == 0) {
			if (fileSize == 0) {
				kernel32_setLastError(ERROR_FILE_INVALID);
				goto done;
			}
			size = fileSize;
		} else if (size > fileSize && protectAllowsFileGrowth(flProtect)) {
			if (!growFileForMapping(dupFd, size)) {
				goto done;
			}
			DEBUG_LOG("CreateFileMappingA: grew backing file from %llu to %llu bytes\n",
					  (unsigned long long)fileSize, (unsigned long long)size);
		}
		if (size > (uint64_t)SIZE_MAX) {
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			goto done;
		}
		mapping->maxSize = (size_t)size;
	}

	ret = Handles_alloc(wibo_handles(), &mapping->base, 0, 0);
	mapping = NULL;

done:
	if (file) {
		ObjectBase_unref(&file->base.base);
	}
	if (mapping) {
		ObjectBase_unref(&mapping->base);
	}
	return ret;
}

HANDLE WINAPI CreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect,
								 DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateFileMappingW -> ");
	char *name = lpName ? wideStringToString(lpName, -1) : NULL;
	HANDLE ret = CreateFileMappingA(hFile, lpFileMappingAttributes, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow, name);
	free(name);
	return ret;
}

// Borrows `mapping`; on success the registered view takes its own reference.
static LPVOID mapViewOfFileInternal(MappingObject *mapping, DWORD dwDesiredAccess, uint64_t offset,
									SIZE_T dwNumberOfBytesToMap, LPVOID baseAddress) {
	if (!mapping) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	if (mapping->closed) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	if (mapping->anonymous && offset != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	size_t maxSize = mapping->maxSize;
	uint64_t length = (uint64_t)dwNumberOfBytesToMap;
	if (length == 0) {
		if (maxSize == 0 || offset > maxSize) {
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return NULL;
		}
		length = maxSize - offset;
	}
	if (length == 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	if (maxSize != 0 && offset + length > maxSize) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}

	FileMapAccess access = fileMapAccessFromDesiredAccess(dwDesiredAccess);
	int prot = PROT_READ;
	if (mapping->protect == PAGE_READWRITE) {
		if (access.write || access.copy) {
			prot |= PROT_WRITE;
		}
	} else {
		if (access.write && !access.copy) {
			kernel32_setLastError(ERROR_ACCESS_DENIED);
			return NULL;
		}
		if (access.copy) {
			prot |= PROT_WRITE;
		}
	}
	if (access.execute) {
		prot |= PROT_EXEC;
	}

	int flags = (mapping->anonymous ? MAP_ANONYMOUS : 0) | (access.copy ? MAP_PRIVATE : MAP_SHARED);
	const size_t pageSize = wibo_heap_systemPageSize();
	off_t alignedOffset = mapping->anonymous ? 0 : (off_t)(offset & ~(uint64_t)(pageSize - 1));
	size_t offsetDelta = (size_t)(offset - (uint64_t)alignedOffset);
	uint64_t requestedLength = length + offsetDelta;
	if (requestedLength < length) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}
	size_t mapLength = (size_t)requestedLength;
	if ((uint64_t)mapLength != requestedLength) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return NULL;
	}

	int mmapFd = mapping->anonymous ? -1 : mapping->fd;
	void *requestedBase = NULL;
	// Claim the range in the guest mapping table before mapping.
	VmStatus reserveStatus;
	if (baseAddress) {
		uintptr_t baseAddr = (uintptr_t)baseAddress;
		if (baseAddr == 0 || (baseAddr % kVirtualAllocationGranularity) != 0) {
			kernel32_setLastError(ERROR_INVALID_ADDRESS);
			return NULL;
		}
		// Windows rejects a base past the user address range. Wine doesn't enforce the limit
		// and maps the view, so this differs from Wine.
		if (baseAddr > kHighestUserAddress) {
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return NULL;
		}
		// Windows always maps the shared user data page at 0x7FFE0000, so a view covering it fails like any overlap.
		if (baseAddr >= kSharedUserDataAddr || length > kSharedUserDataAddr - baseAddr) {
			kernel32_setLastError(ERROR_INVALID_ADDRESS);
			return NULL;
		}
		if (offsetDelta > baseAddr) {
			kernel32_setLastError(ERROR_INVALID_ADDRESS);
			return NULL;
		}
		uintptr_t mapBaseAddr = baseAddr - offsetDelta;
		if ((mapBaseAddr & (pageSize - 1)) != 0) {
			kernel32_setLastError(ERROR_INVALID_ADDRESS);
			return NULL;
		}
		requestedBase = (void *)mapBaseAddr;
		reserveStatus = wibo_heap_reserveViewRangeAt(requestedBase, mapLength);
	} else {
		reserveStatus = wibo_heap_reserveViewRange(mapLength, 0, 0, &requestedBase);
	}
	if (reserveStatus != VmStatus_Success) {
		kernel32_setLastError(wibo_heap_win32ErrorFromVmStatus(reserveStatus));
		return NULL;
	}

	errno = 0;
	void *mapBase = mmap(requestedBase, mapLength, prot, flags | MAP_FIXED, mmapFd, alignedOffset);
	if (mapBase == MAP_FAILED) {
		int err = errno;
		if (baseAddress && (err == ENOMEM || err == EEXIST || err == EINVAL || err == EPERM)) {
			kernel32_setLastError(ERROR_INVALID_ADDRESS);
		} else {
			kernel32_setLastError(wibo_winErrorFromErrno(err));
		}
		wibo_heap_releaseViewRange(requestedBase, mapLength);
		return NULL;
	}
	void *viewPtr = (uint8_t *)mapBase + offsetDelta;
	if (baseAddress && viewPtr != baseAddress) {
		munmap(mapBase, mapLength);
		kernel32_setLastError(ERROR_INVALID_ADDRESS);
		wibo_heap_releaseViewRange(requestedBase, mapLength);
		return NULL;
	}
	uintptr_t viewLength = (uintptr_t)length;
	uintptr_t alignedViewLength = alignUp(viewLength, pageSize);
	if (alignedViewLength == UINTPTR_MAX) {
		alignedViewLength = viewLength;
	}
	DWORD protect = mapping->protect;
	ViewInfo view = {0};
	view.viewBase = (uintptr_t)viewPtr;
	view.viewLength = (size_t)alignedViewLength;
	view.allocationBase = (uintptr_t)mapBase;
	view.allocationLength = mapLength;
	view.owner = mapping;
	view.protect = desiredAccessToProtect(dwDesiredAccess, protect);
	view.allocationProtect = protect;
	view.type = MEM_MAPPED;
	wibo_heap_registerViewRange(mapBase, mapLength, protect, view.protect);
	pthread_mutex_lock(&g_viewInfoMutex);
	// Like emplace, an existing entry for the same base is kept and the new view is dropped.
	size_t idx = viewLowerBound(view.viewBase);
	if (idx == g_viewInfo.len || g_viewInfo.data[idx].key != view.viewBase) {
		ObjectBase_ref(&mapping->base);
		vec_reserve(&g_viewInfo, 1);
		memmove(&g_viewInfo.data[idx + 1], &g_viewInfo.data[idx], (g_viewInfo.len - idx) * sizeof *g_viewInfo.data);
		g_viewInfo.data[idx].key = view.viewBase;
		g_viewInfo.data[idx].info = view;
		g_viewInfo.len++;
	}
	pthread_mutex_unlock(&g_viewInfoMutex);
	return viewPtr;
}

LPVOID WINAPI MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
							DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MapViewOfFile(%p, 0x%x, %u, %u, %zu)\n", hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh,
			  dwFileOffsetLow, dwNumberOfBytesToMap);

	MappingObject *mapping = (MappingObject *)Handles_getAs(wibo_handles(), hFileMappingObject, ObjectType_Mapping, NULL);
	if (!mapping) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	uint64_t offset = ((uint64_t)dwFileOffsetHigh << 32) | dwFileOffsetLow;
	LPVOID ret = mapViewOfFileInternal(mapping, dwDesiredAccess, offset, dwNumberOfBytesToMap, NULL);
	ObjectBase_unref(&mapping->base);
	return ret;
}

LPVOID WINAPI MapViewOfFileEx(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
							  DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap, LPVOID lpBaseAddress) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MapViewOfFileEx(%p, 0x%x, %u, %u, %zu, %p)\n", hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh,
			  dwFileOffsetLow, dwNumberOfBytesToMap, lpBaseAddress);

	MappingObject *mapping = (MappingObject *)Handles_getAs(wibo_handles(), hFileMappingObject, ObjectType_Mapping, NULL);
	if (!mapping) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return NULL;
	}
	uint64_t offset = ((uint64_t)dwFileOffsetHigh << 32) | dwFileOffsetLow;
	LPVOID ret = mapViewOfFileInternal(mapping, dwDesiredAccess, offset, dwNumberOfBytesToMap, lpBaseAddress);
	ObjectBase_unref(&mapping->base);
	return ret;
}

BOOL WINAPI UnmapViewOfFile(LPCVOID lpBaseAddress) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("UnmapViewOfFile(%p)\n", lpBaseAddress);
	pthread_mutex_lock(&g_viewInfoMutex);
	uintptr_t key = (uintptr_t)lpBaseAddress;
	size_t idx = viewLowerBound(key);
	if (idx == g_viewInfo.len || g_viewInfo.data[idx].key != key) {
		pthread_mutex_unlock(&g_viewInfoMutex);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ViewInfo view = g_viewInfo.data[idx].info;
	vec_erase(&g_viewInfo, idx);
	pthread_mutex_unlock(&g_viewInfoMutex);
	wibo_heap_releaseViewRange((void *)view.allocationBase, view.allocationLength);
	ObjectBase_unref(&view.owner->base);
	return TRUE;
}

BOOL WINAPI FlushViewOfFile(LPCVOID lpBaseAddress, SIZE_T dwNumberOfBytesToFlush) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FlushViewOfFile(%p, %zu)\n", lpBaseAddress, dwNumberOfBytesToFlush);

	if (!lpBaseAddress) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	uintptr_t address = (uintptr_t)lpBaseAddress;
	uintptr_t viewBase = 0;
	size_t viewLength = 0;
	uintptr_t allocationBase = 0;
	size_t allocationLength = 0;

	pthread_mutex_lock(&g_viewInfoMutex);
	size_t ub = viewUpperBound(address);
	if (ub == 0) {
		pthread_mutex_unlock(&g_viewInfoMutex);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const ViewInfo *view = &g_viewInfo.data[ub - 1].info;
	if (address < view->viewBase || address >= view->viewBase + view->viewLength) {
		pthread_mutex_unlock(&g_viewInfoMutex);
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	viewBase = view->viewBase;
	viewLength = view->viewLength;
	allocationBase = view->allocationBase;
	allocationLength = view->allocationLength;
	pthread_mutex_unlock(&g_viewInfoMutex);

	size_t offsetIntoView = (size_t)(address - viewBase);
	size_t bytesToFlush = dwNumberOfBytesToFlush;
	size_t maxFlush = viewLength - offsetIntoView;
	if (bytesToFlush == 0 || bytesToFlush > maxFlush) {
		bytesToFlush = maxFlush;
	}
	if (bytesToFlush == 0) {
		return TRUE;
	}

	uintptr_t flushStart = address;
	uintptr_t flushEnd = flushStart + bytesToFlush;
	const size_t pageSize = wibo_heap_systemPageSize();
	uintptr_t alignedStart = alignDown(flushStart, pageSize);
	uintptr_t alignedEnd = alignUp(flushEnd, pageSize);
	if (alignedEnd == UINTPTR_MAX) {
		alignedEnd = flushEnd;
	}

	uintptr_t mappingEnd = allocationBase + allocationLength;
	if (alignedEnd > mappingEnd) {
		alignedEnd = mappingEnd;
	}
	if (alignedEnd < alignedStart) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	size_t length = (size_t)(alignedEnd - alignedStart);
	if (length == 0) {
		length = pageSize;
	}

	if (msync((void *)alignedStart, length, MS_SYNC) != 0) {
		kernel32_setLastError(wibo_winErrorFromErrno(errno));
		return FALSE;
	}

	return TRUE;
}

LPVOID WINAPI VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VirtualAlloc(%p, %zu, %u, %u)\n", lpAddress, dwSize, flAllocationType, flProtect);

	void *base = lpAddress;
	size_t size = (size_t)dwSize;
	VmStatus status = wibo_heap_virtualAlloc(&base, &size, flAllocationType, flProtect, MEM_PRIVATE);
	if (status != VmStatus_Success) {
		DWORD err = wibo_heap_win32ErrorFromVmStatus(status);
		DEBUG_LOG("-> failed (status=%u, err=%u)\n", (unsigned)status, err);
		kernel32_setLastError(err);
		return NULL;
	}
	DEBUG_LOG("-> success (base=%p, size=%zu)\n", base, size);
	return base;
}

BOOL WINAPI VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VirtualFree(%p, %zu, %u)\n", lpAddress, dwSize, dwFreeType);
	VmStatus status = wibo_heap_virtualFree(lpAddress, (size_t)dwSize, dwFreeType);
	if (status != VmStatus_Success) {
		DWORD err = wibo_heap_win32ErrorFromVmStatus(status);
		DEBUG_LOG("-> failed (status=%u, err=%u)\n", (unsigned)status, err);
		kernel32_setLastError(err);
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VirtualProtect(%p, %zu, %u)\n", lpAddress, dwSize, flNewProtect);
	VmStatus status = wibo_heap_virtualProtect(lpAddress, (size_t)dwSize, flNewProtect, lpflOldProtect);
	if (status != VmStatus_Success) {
		DWORD err = wibo_heap_win32ErrorFromVmStatus(status);
		DEBUG_LOG("-> failed (status=%u, err=%u)\n", (unsigned)status, err);
		kernel32_setLastError(err);
		return FALSE;
	}
	return TRUE;
}

SIZE_T WINAPI VirtualQuery(LPCVOID lpAddress, PMEMORY_BASIC_INFORMATION lpBuffer, SIZE_T dwLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VirtualQuery(%p, %p, %zu)\n", lpAddress, lpBuffer, dwLength);
	if (!lpBuffer || dwLength < sizeof(MEMORY_BASIC_INFORMATION)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER\n");
		return 0;
	}

	memset(lpBuffer, 0, sizeof(MEMORY_BASIC_INFORMATION));
	const size_t pageSize = wibo_heap_systemPageSize();
	uintptr_t request = lpAddress ? (uintptr_t)lpAddress : 0;
	uintptr_t pageBase = alignDown(request, pageSize);
	if (pageBase >= kProcessAddressLimit) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG("-> ERROR_INVALID_PARAMETER (beyond address space)\n");
		return 0;
	}

	MEMORY_BASIC_INFORMATION info = {0};
	if (mappedViewRegionForAddress(request, pageBase, &info)) {
		*lpBuffer = info;
		return sizeof(MEMORY_BASIC_INFORMATION);
	}

	VmStatus status = wibo_heap_virtualQuery(lpAddress, &info);
	if (status == VmStatus_Success) {
		*lpBuffer = info;
		return sizeof(MEMORY_BASIC_INFORMATION);
	}

	DEBUG_LOG("VirtualQuery fallback failed status=%u\n", (unsigned)status);
	kernel32_setLastError(wibo_heap_win32ErrorFromVmStatus(status));
	DEBUG_LOG("-> VirtualQuery failed (status=%u)\n", (unsigned)status);
	return 0;
}

BOOL WINAPI GetProcessWorkingSetSize(HANDLE hProcess, PSIZE_T lpMinimumWorkingSetSize,
									 PSIZE_T lpMaximumWorkingSetSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetProcessWorkingSetSize(%p, %p, %p)\n", hProcess, lpMinimumWorkingSetSize, lpMaximumWorkingSetSize);
	(void)hProcess;
	if (!lpMinimumWorkingSetSize || !lpMaximumWorkingSetSize) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*lpMinimumWorkingSetSize = 32 * 1024 * 1024;  // 32 MiB stub
	*lpMaximumWorkingSetSize = 128 * 1024 * 1024; // 128 MiB stub
	return TRUE;
}

BOOL WINAPI SetProcessWorkingSetSize(HANDLE hProcess, SIZE_T dwMinimumWorkingSetSize, SIZE_T dwMaximumWorkingSetSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetProcessWorkingSetSize(%p, %zu, %zu)\n", hProcess, dwMinimumWorkingSetSize, dwMaximumWorkingSetSize);
	(void)hProcess;
	(void)dwMinimumWorkingSetSize;
	(void)dwMaximumWorkingSetSize;
	return TRUE;
}
