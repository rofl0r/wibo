#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Mirrors the x64 RUNTIME_FUNCTION layout (.pdata entry); all fields are RVAs.
typedef struct {
	uint32_t beginAddress;
	uint32_t endAddress;
	uint32_t unwindInfoAddress;
} WiboRuntimeFunction;

typedef struct WiboFunctionTable WiboFunctionTable;

// Copies the entries (sorted by beginAddress) so lookups do not depend on section protection.
// The caller must check that the entries lie within the image. Returns NULL on invalid arguments.
WiboFunctionTable *wibo_registerFunctionTable(const void *imageBase, size_t imageSize,
											  const WiboRuntimeFunction *entries, size_t count);
void wibo_unregisterFunctionTable(WiboFunctionTable *table);

// Finds the runtime function covering pc in any registered module.
bool wibo_lookupRuntimeFunction(uint64_t pc, uint64_t *imageBase, WiboRuntimeFunction *out);
