#pragma once

#include "common.h"

#include <stddef.h>
#include <stdint.h>

#define kInvalidTlsIndex 0xFFFFFFFFu
#define kTlsMaxSlotCount 1088

void wibo_tls_initializeTib(TEB *tib);
void wibo_tls_cleanupTib(TEB *tib);

void wibo_tls_forEachTib(void (*callback)(TEB *, void *), void *context);

bool wibo_tls_ensureModulePointerCapacity(size_t capacity);
bool wibo_tls_setModulePointer(TEB *tib, size_t index, GUEST_PTR value);
void wibo_tls_clearModulePointer(TEB *tib, size_t index);

DWORD wibo_tls_reserveSlot(void);
bool wibo_tls_releaseSlot(DWORD index);
bool wibo_tls_isSlotAllocated(DWORD index);

GUEST_PTR wibo_tls_getTibValue(TEB *tib, DWORD index);
bool wibo_tls_setTibValue(TEB *tib, DWORD index, GUEST_PTR value);

GUEST_PTR wibo_tls_getValue(DWORD index);
bool wibo_tls_setValue(DWORD index, GUEST_PTR value);
