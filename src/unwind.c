#include "unwind.h"

#include "cutil.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct WiboFunctionTable {
	uint64_t imageBase;
	uint32_t imageSize;
	size_t count;
	WiboRuntimeFunction *entries;
};

typedef VEC(WiboFunctionTable *) FunctionTableVec;

static FunctionTableVec g_tables;
static pthread_mutex_t g_tablesMutex = PTHREAD_MUTEX_INITIALIZER;

static int compareRuntimeFunctions(const void *a, const void *b) {
	const WiboRuntimeFunction *fa = a;
	const WiboRuntimeFunction *fb = b;
	if (fa->beginAddress != fb->beginAddress) {
		return fa->beginAddress < fb->beginAddress ? -1 : 1;
	}
	return 0;
}

WiboFunctionTable *wibo_registerFunctionTable(const void *imageBase, size_t imageSize,
											  const WiboRuntimeFunction *entries, size_t count) {
	if (!imageBase || !entries || count == 0 || imageSize > UINT32_MAX) {
		return NULL;
	}
	WiboFunctionTable *table = xcalloc(1, sizeof(*table));
	table->imageBase = (uint64_t)(uintptr_t)imageBase;
	table->imageSize = (uint32_t)imageSize;
	table->count = count;
	table->entries = xmalloc(count * sizeof(*table->entries));
	memcpy(table->entries, entries, count * sizeof(*table->entries));
	qsort(table->entries, count, sizeof(*table->entries), compareRuntimeFunctions);

	pthread_mutex_lock(&g_tablesMutex);
	vec_push(&g_tables, table);
	pthread_mutex_unlock(&g_tablesMutex);
	return table;
}

void wibo_unregisterFunctionTable(WiboFunctionTable *table) {
	if (!table) {
		return;
	}
	pthread_mutex_lock(&g_tablesMutex);
	for (size_t i = 0; i < g_tables.len; i++) {
		if (g_tables.data[i] == table) {
			vec_erase(&g_tables, i);
			break;
		}
	}
	pthread_mutex_unlock(&g_tablesMutex);
	free(table->entries);
	free(table);
}

static bool lookupInTable(const WiboFunctionTable *table, uint32_t rva, WiboRuntimeFunction *out) {
	size_t lo = 0;
	size_t hi = table->count;
	// Find the last entry whose beginAddress is <= rva.
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (table->entries[mid].beginAddress <= rva) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo == 0) {
		return false;
	}
	const WiboRuntimeFunction *candidate = &table->entries[lo - 1];
	if (rva >= candidate->endAddress) {
		return false;
	}
	*out = *candidate;
	return true;
}

bool wibo_lookupRuntimeFunction(uint64_t pc, uint64_t *imageBase, WiboRuntimeFunction *out) {
	bool found = false;
	pthread_mutex_lock(&g_tablesMutex);
	for (size_t i = 0; i < g_tables.len && !found; i++) {
		const WiboFunctionTable *table = g_tables.data[i];
		if (pc < table->imageBase || pc - table->imageBase >= table->imageSize) {
			continue;
		}
		uint32_t rva = (uint32_t)(pc - table->imageBase);
		if (lookupInTable(table, rva, out)) {
			*imageBase = table->imageBase;
			found = true;
		}
	}
	pthread_mutex_unlock(&g_tablesMutex);
	return found;
}

bool wibo_isRegisteredCode(uint64_t pc) {
	bool found = false;
	pthread_mutex_lock(&g_tablesMutex);
	for (size_t i = 0; i < g_tables.len && !found; i++) {
		const WiboFunctionTable *table = g_tables.data[i];
		found = pc >= table->imageBase && pc - table->imageBase < table->imageSize;
	}
	pthread_mutex_unlock(&g_tablesMutex);
	return found;
}

#define UWOP_PUSH_NONVOL 0
#define UWOP_ALLOC_LARGE 1
#define UWOP_ALLOC_SMALL 2
#define UWOP_SET_FPREG 3
#define UWOP_SAVE_NONVOL 4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_SAVE_XMM128 8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME 10

static uint64_t readU64(uint64_t addr) {
	uint64_t value;
	memcpy(&value, (const void *)(uintptr_t)addr, sizeof(value));
	return value;
}

static uint32_t readU32(uint64_t addr) {
	uint32_t value;
	memcpy(&value, (const void *)(uintptr_t)addr, sizeof(value));
	return value;
}

static void readXmm(uint64_t addr, uint64_t out[2]) {
	memcpy(out, (const void *)(uintptr_t)addr, 2 * sizeof(uint64_t));
}

static uint32_t readRva(uint64_t imageBase, uint32_t rva) {
	return readU32(imageBase + rva);
}

// Number of 2-byte unwind-code slots occupied by the operation starting at codes[0].
static size_t unwindCodeSlots(uint8_t op, uint8_t opInfo) {
	switch (op) {
	case UWOP_ALLOC_LARGE:
		return opInfo == 0 ? 2 : 3;
	case UWOP_SAVE_NONVOL:
	case UWOP_SAVE_XMM128:
		return 2;
	case UWOP_SAVE_NONVOL_FAR:
	case UWOP_SAVE_XMM128_FAR:
		return 3;
	default:
		return 1;
	}
}

static uint8_t readU8(uint64_t addr) {
	return *(const uint8_t *)(uintptr_t)addr;
}

// Returns the primary RUNTIME_FUNCTION that owns the unwind codes and handler for a (possibly chained) entry.
static bool resolvePrimaryFunction(uint64_t imageBase, WiboRuntimeFunction current, WiboRuntimeFunction *primary) {
	for (int depth = 0; depth < 32; depth++) {
		uint32_t infoAddress = current.unwindInfoAddress;
		uint8_t flags = readU8(imageBase + infoAddress) >> 3;
		if (!(flags & UNW_FLAG_CHAININFO)) {
			*primary = current;
			return true;
		}
		uint8_t countOfCodes = readU8(imageBase + infoAddress + 2);
		uint32_t chainAddress = infoAddress + 4 + (uint32_t)(((countOfCodes + 1) & ~1) * 2);
		current.beginAddress = readRva(imageBase, chainAddress);
		current.endAddress = readRva(imageBase, chainAddress + 4);
		current.unwindInfoAddress = readRva(imageBase, chainAddress + 8);
	}
	return false;
}

bool wibo_unwindWithFunction(WiboContext64 *ctx, uint64_t controlPc, uint64_t imageBase,
							 const WiboRuntimeFunction *function, WiboFrameInfo *info) {
	memset(info, 0, sizeof(*info));

	if (!function) {
		// Leaf function without unwind info: the return address sits at the top of the stack.
		info->establisherFrame = ctx->gpr[WIBO_REG_RSP];
		ctx->rip = readU64(ctx->gpr[WIBO_REG_RSP]);
		ctx->gpr[WIBO_REG_RSP] += 8;
		return true;
	}
	info->hasFunction = true;
	info->imageBase = imageBase;

	WiboRuntimeFunction primary;
	if (!resolvePrimaryFunction(imageBase, *function, &primary)) {
		return false;
	}
	uint32_t infoAddress = primary.unwindInfoAddress;
	uint8_t flags = readU8(imageBase + infoAddress) >> 3;
	info->handlerFlags = flags;
	uint8_t frameInfo = readU8(imageBase + infoAddress + 3);
	uint8_t frameRegister = frameInfo & 0xf;
	uint64_t frameOffset = (uint64_t)(frameInfo >> 4) * 16;
	uint8_t countOfCodes = readU8(imageBase + infoAddress + 2);

	// Offset of the pc inside the prolog. Pcs outside the primary entry (e.g. cold code) are past it.
	// beginAddress is an RVA while controlPc is absolute.
	uint64_t controlRva = controlPc - imageBase;
	uint32_t prologOffset = controlRva >= primary.beginAddress && controlRva - primary.beginAddress < UINT32_MAX
								? (uint32_t)(controlRva - primary.beginAddress)
								: UINT32_MAX;

	info->establisherFrame = frameRegister ? ctx->gpr[frameRegister] - frameOffset : ctx->gpr[WIBO_REG_RSP];

	uint64_t codesBase = imageBase + infoAddress + 4;
	bool machineFrame = false;
	for (size_t i = 0; i < countOfCodes;) {
		uint64_t node = codesBase + i * 2;
		uint8_t codeOffset = readU8(node);
		uint8_t op = readU8(node + 1) & 0xf;
		uint8_t opInfo = readU8(node + 1) >> 4;
		size_t slots = unwindCodeSlots(op, opInfo);
		if (i + slots > countOfCodes) {
			return false;
		}
		// Codes are listed newest first; those past the pc's prolog offset have not executed yet.
		if (codeOffset > prologOffset) {
			i += slots;
			continue;
		}
		switch (op) {
		case UWOP_PUSH_NONVOL:
			ctx->gpr[opInfo] = readU64(ctx->gpr[WIBO_REG_RSP]);
			ctx->gpr[WIBO_REG_RSP] += 8;
			break;
		case UWOP_ALLOC_LARGE: {
			uint64_t size;
			if (opInfo == 0) {
				size = (uint64_t)readU8(node + 2) | ((uint64_t)readU8(node + 3) << 8);
				size *= 8;
			} else {
				size = (uint64_t)readU32(node + 2);
			}
			ctx->gpr[WIBO_REG_RSP] += size;
			break;
		}
		case UWOP_ALLOC_SMALL:
			ctx->gpr[WIBO_REG_RSP] += (uint64_t)opInfo * 8 + 8;
			break;
		case UWOP_SET_FPREG:
			ctx->gpr[WIBO_REG_RSP] = ctx->gpr[frameRegister] - frameOffset;
			break;
		case UWOP_SAVE_NONVOL:
			ctx->gpr[opInfo] = readU64(ctx->gpr[WIBO_REG_RSP] + (uint64_t)(readU8(node + 2) | (readU8(node + 3) << 8)) * 8);
			break;
		case UWOP_SAVE_NONVOL_FAR:
			ctx->gpr[opInfo] = readU64(ctx->gpr[WIBO_REG_RSP] + readU32(node + 2));
			break;
		case UWOP_SAVE_XMM128:
			readXmm(ctx->gpr[WIBO_REG_RSP] + (uint64_t)(readU8(node + 2) | (readU8(node + 3) << 8)) * 16,
					ctx->xmm[opInfo]);
			break;
		case UWOP_SAVE_XMM128_FAR:
			readXmm(ctx->gpr[WIBO_REG_RSP] + readU32(node + 2), ctx->xmm[opInfo]);
			break;
		case UWOP_PUSH_MACHFRAME: {
			// Hardware-pushed interrupt frame: [error code] RIP, CS, EFLAGS, RSP, SS.
			uint64_t base = ctx->gpr[WIBO_REG_RSP] + (opInfo ? 8 : 0);
			ctx->rip = readU64(base);
			ctx->gpr[WIBO_REG_RSP] = readU64(base + 24);
			machineFrame = true;
			break;
		}
		default:
			return false;
		}
		i += slots;
	}

	if (!machineFrame) {
		ctx->rip = readU64(ctx->gpr[WIBO_REG_RSP]);
		ctx->gpr[WIBO_REG_RSP] += 8;
	}

	if (flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER)) {
		uint64_t handlerRvaAddress = codesBase + (uint64_t)((countOfCodes + 1) & ~1) * 2;
		info->languageHandler = imageBase + readU32(handlerRvaAddress);
		info->handlerData = handlerRvaAddress + 4;
	}
	return true;
}

bool wibo_virtualUnwind(WiboContext64 *ctx, bool isCallerFrame, WiboFrameInfo *info) {
	uint64_t controlPc = isCallerFrame ? ctx->rip - 1 : ctx->rip;
	uint64_t imageBase = 0;
	WiboRuntimeFunction function;
	if (!wibo_lookupRuntimeFunction(controlPc, &imageBase, &function)) {
		return wibo_unwindWithFunction(ctx, controlPc, 0, NULL, info);
	}
	return wibo_unwindWithFunction(ctx, controlPc, imageBase, &function, info);
}
