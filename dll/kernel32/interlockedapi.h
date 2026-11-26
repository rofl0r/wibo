#pragma once

#include "types.h"

typedef struct SLIST_ENTRY {
	GUEST_PTR Next;
} SLIST_ENTRY, *PSLIST_ENTRY;

typedef struct SLIST_HEADER {
	GUEST_PTR Head;
	unsigned short Depth;
	unsigned short Sequence;
} SLIST_HEADER, *PSLIST_HEADER;

LONG WINAPI InterlockedIncrement(LONG volatile *Addend);
LONG WINAPI InterlockedDecrement(LONG volatile *Addend);
LONG WINAPI InterlockedExchange(LONG volatile *Target, LONG Value);
LONG WINAPI InterlockedCompareExchange(LONG volatile *Destination, LONG Exchange, LONG Comperand);
long long WINAPI InterlockedCompareExchange64(long long volatile *Destination, long long Exchange, long long Comperand);
void WINAPI InitializeSListHead(PSLIST_HEADER ListHead);
PSLIST_ENTRY WINAPI InterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry);
PSLIST_ENTRY WINAPI InterlockedPopEntrySList(PSLIST_HEADER ListHead);
