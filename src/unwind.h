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

// True when pc lies inside a module registered with a function table, including code with no unwind entry.
bool wibo_isRegisteredCode(uint64_t pc);

#define UNW_FLAG_EHANDLER 0x1
#define UNW_FLAG_UHANDLER 0x2
#define UNW_FLAG_CHAININFO 0x4

// x64 register numbers follow the CONTEXT/ModRM order: rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15.
#define WIBO_REG_RSP 4
#define WIBO_REG_RBP 5

typedef struct {
	uint64_t gpr[16];
	uint64_t rip;
	uint64_t xmm[16][2];
} WiboContext64;

typedef struct {
	bool hasFunction;          // false for leaf frames without unwind info
	uint8_t handlerFlags;      // UNW_FLAG_* bits of the unwind info (EHANDLER = 1, UHANDLER = 2)
	uint64_t imageBase;
	uint64_t establisherFrame; // frame base used by SEH handlers to address locals
	uint64_t languageHandler;  // absolute address, 0 when the frame has no handler
	uint64_t handlerData;      // absolute address of the handler-specific data
} WiboFrameInfo;

// Unwinds one frame of ctx in place into its caller. isCallerFrame selects pc-1 for the lookup, as needed for
// return addresses. Returns false when the unwind info uses an opcode this unwinder does not implement.
bool wibo_virtualUnwind(WiboContext64 *ctx, bool isCallerFrame, WiboFrameInfo *info);

// Same as wibo_virtualUnwind, but for a RUNTIME_FUNCTION the caller already found. function == NULL unwinds a leaf.
bool wibo_unwindWithFunction(WiboContext64 *ctx, uint64_t controlPc, uint64_t imageBase,
							 const WiboRuntimeFunction *function, WiboFrameInfo *info);
