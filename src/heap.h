#pragma once

#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Opaque per-guest-heap state (HeapCreate). The default process heap is wibo_heap_guestMalloc & co.
typedef struct Heap Heap;

typedef enum {
	VmStatus_Success,
	VmStatus_InvalidParameter,
	VmStatus_InvalidAddress,
	VmStatus_NoAccess,
	VmStatus_NotSupported,
	VmStatus_NoMemory,
	VmStatus_Rejected,
	VmStatus_UnknownError,
} VmStatus;

uintptr_t wibo_heap_systemPageSize(void);
uintptr_t wibo_heap_allocationGranularity(void);

Heap *Heap_create(void);
void Heap_destroy(Heap *heap);
// Allocations are zero-filled.
void *Heap_malloc(Heap *heap, size_t size);
void *Heap_realloc(Heap *heap, void *ptr, size_t newSize);
bool Heap_free(Heap *heap, void *ptr);

void *wibo_heap_guestMalloc(size_t size);
void *wibo_heap_guestRealloc(void *ptr, size_t newSize);
bool wibo_heap_guestFree(void *ptr);
// Usable size of a block from wibo_heap_guestMalloc or a virtual allocation base; SIZE_MAX if ptr is not one.
size_t wibo_heap_guestSize(const void *ptr);

VmStatus wibo_heap_virtualAlloc(void **baseAddress, size_t *regionSize, DWORD allocationType, DWORD protect,
								DWORD type);
VmStatus wibo_heap_virtualFree(void *baseAddress, size_t regionSize, DWORD freeType);
VmStatus wibo_heap_virtualProtect(void *baseAddress, size_t regionSize, DWORD newProtect, DWORD *oldProtect);
VmStatus wibo_heap_virtualQuery(const void *address, MEMORY_BASIC_INFORMATION *outInfo);
VmStatus wibo_heap_virtualReset(void *baseAddress, size_t regionSize);

VmStatus wibo_heap_reserveViewRange(size_t regionSize, uintptr_t minAddr, uintptr_t maxAddr, void **baseAddress);
VmStatus wibo_heap_reserveViewRangeAt(void *baseAddress, size_t regionSize);
void wibo_heap_registerViewRange(void *baseAddress, size_t regionSize, DWORD allocationProtect, DWORD protect);
void wibo_heap_releaseViewRange(void *baseAddress, size_t regionSize);

DWORD wibo_heap_win32ErrorFromVmStatus(VmStatus status);
NTSTATUS wibo_heap_ntStatusFromVmStatus(VmStatus status);

bool wibo_heap_reserveGuestStack(size_t stackSizeBytes, void **outStackLimit, void **outStackBase);
