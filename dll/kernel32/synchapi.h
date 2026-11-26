#pragma once

#include "minwinbase.h"
#include "types.h"

#define WAIT_OBJECT_0 ((DWORD)0x00000000)
#define WAIT_ABANDONED ((DWORD)0x00000080)
#define WAIT_TIMEOUT ((DWORD)0x00000102)
#define WAIT_FAILED ((DWORD)0xFFFFFFFF)
#define INFINITE ((DWORD)0xFFFFFFFF)
#define MAXIMUM_WAIT_OBJECTS ((DWORD)64)

#define INIT_ONCE_CHECK_ONLY ((DWORD)0x00000001UL)
#define INIT_ONCE_ASYNC ((DWORD)0x00000002UL)
#define INIT_ONCE_INIT_FAILED ((DWORD)0x00000004UL)
#define INIT_ONCE_CTX_RESERVED_BITS ((DWORD)2)

#define RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO ((DWORD)0x01000000UL)
#define RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN ((DWORD)0x02000000UL)
#define RTL_CRITICAL_SECTION_FLAG_STATIC_INIT ((DWORD)0x04000000UL)
#define RTL_CRITICAL_SECTION_FLAG_RESOURCE_TYPE ((DWORD)0x08000000UL)
#define RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO ((DWORD)0x10000000UL)
#define RTL_CRITICAL_SECTION_ALL_FLAG_BITS ((DWORD)0xff000000UL)
#define RTL_CRITICAL_SECTION_FLAG_RESERVED ((DWORD)0xe0000000UL)

typedef struct RTL_CRITICAL_SECTION_DEBUG {
	WORD Type;
	WORD CreatorBackTraceIndex;
	GUEST_PTR CriticalSection;
	LIST_ENTRY ProcessLocksList;
	DWORD EntryCount;
	DWORD ContentionCount;
	DWORD Flags;
	WORD CreatorBackTraceIndexHigh;
	WORD SpareWORD;
	DWORD Spare[2];
} RTL_CRITICAL_SECTION_DEBUG, *PRTL_CRITICAL_SECTION_DEBUG;

typedef struct RTL_CRITICAL_SECTION {
	GUEST_PTR DebugInfo;
	LONG LockCount;
	LONG RecursionCount;
	HANDLE OwningThread;
	HANDLE LockSemaphore;
	ULONG_PTR SpinCount;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION, *LPCRITICAL_SECTION, *PCRITICAL_SECTION;

_Static_assert(sizeof(RTL_CRITICAL_SECTION) == (sizeof(GUEST_PTR) == 8 ? 40 : 24),
			   "RTL_CRITICAL_SECTION size must match the guest pointer width");

typedef union RTL_RUN_ONCE {
	GUEST_PTR Ptr;
} RTL_RUN_ONCE, *PRTL_RUN_ONCE;

typedef RTL_RUN_ONCE INIT_ONCE;
typedef INIT_ONCE *PINIT_ONCE;
typedef INIT_ONCE *LPINIT_ONCE;

#define INIT_ONCE_STATIC_INIT ((INIT_ONCE){GUEST_NULL})

typedef BOOL(_CC_STDCALL *PINIT_ONCE_FN)(PINIT_ONCE, LPVOID, GUEST_PTR *);

typedef union RTL_SRWLOCK {
	ULONG Value;
} RTL_SRWLOCK, SRWLOCK, *PSRWLOCK, *PRTL_SRWLOCK;

#define SRWLOCK_INIT ((SRWLOCK){GUEST_NULL})

void WINAPI Sleep(DWORD dwMilliseconds);
HANDLE WINAPI CreateMutexA(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCSTR lpName);
HANDLE WINAPI CreateMutexW(LPSECURITY_ATTRIBUTES lpMutexAttributes, BOOL bInitialOwner, LPCWSTR lpName);
BOOL WINAPI ReleaseMutex(HANDLE hMutex);
HANDLE WINAPI CreateEventA(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState,
						   LPCSTR lpName);
HANDLE WINAPI CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState,
						   LPCWSTR lpName);
BOOL WINAPI SetEvent(HANDLE hEvent);
BOOL WINAPI ResetEvent(HANDLE hEvent);
HANDLE WINAPI CreateSemaphoreA(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
							   LPCSTR lpName);
HANDLE WINAPI CreateSemaphoreW(LPSECURITY_ATTRIBUTES lpSemaphoreAttributes, LONG lInitialCount, LONG lMaximumCount,
							   LPCWSTR lpName);
BOOL WINAPI ReleaseSemaphore(HANDLE hSemaphore, LONG lReleaseCount, PLONG lpPreviousCount);
DWORD WINAPI WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
DWORD WINAPI WaitForSingleObjectEx(HANDLE hHandle, DWORD dwMilliseconds, BOOL bAlertable);
DWORD WINAPI WaitForMultipleObjectsEx(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds,
									  BOOL bAlertable);
DWORD WINAPI WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);
void WINAPI InitializeCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL WINAPI InitializeCriticalSectionEx(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount, DWORD Flags);
BOOL WINAPI InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION lpCriticalSection, DWORD dwSpinCount);
void WINAPI DeleteCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void WINAPI EnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
void WINAPI LeaveCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL WINAPI TryEnterCriticalSection(LPCRITICAL_SECTION lpCriticalSection);
BOOL WINAPI InitOnceExecuteOnce(PINIT_ONCE lpInitOnce, PINIT_ONCE_FN lpInitFn, LPVOID lpParameter,
								GUEST_PTR *lpContext);
BOOL WINAPI InitOnceBeginInitialize(LPINIT_ONCE lpInitOnce, DWORD dwFlags, PBOOL fPending, GUEST_PTR *lpContext);
BOOL WINAPI InitOnceComplete(LPINIT_ONCE lpInitOnce, DWORD dwFlags, LPVOID lpContext);
void WINAPI InitializeSRWLock(PSRWLOCK SRWLock);
void WINAPI AcquireSRWLockShared(PSRWLOCK SRWLock);
void WINAPI ReleaseSRWLockShared(PSRWLOCK SRWLock);
void WINAPI AcquireSRWLockExclusive(PSRWLOCK SRWLock);
void WINAPI ReleaseSRWLockExclusive(PSRWLOCK SRWLock);
BOOLEAN WINAPI TryAcquireSRWLockExclusive(PSRWLOCK SRWLock);
BOOLEAN WINAPI TryAcquireSRWLockShared(PSRWLOCK SRWLock);
BOOL WINAPI WaitOnAddress(volatile VOID *Address, PVOID CompareAddress, SIZE_T AddressSize, DWORD dwMilliseconds);
void WINAPI WakeByAddressSingle(PVOID Address);
void WINAPI WakeByAddressAll(PVOID Address);

