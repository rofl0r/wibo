#pragma once

#include "unwind.h"

#include <stddef.h>
#include <stdint.h>

// AMD64 CONTEXT layout (winnt.h, 1232 bytes, 16-byte aligned).
#define CONTEXT_AMD64 0x00100000
#define CONTEXT_AMD64_CONTROL (CONTEXT_AMD64 | 0x1)
#define CONTEXT_AMD64_INTEGER (CONTEXT_AMD64 | 0x2)
#define CONTEXT_AMD64_SEGMENTS (CONTEXT_AMD64 | 0x4)
#define CONTEXT_AMD64_FLOATING_POINT (CONTEXT_AMD64 | 0x8)
#define CONTEXT_AMD64_FULL (CONTEXT_AMD64_CONTROL | CONTEXT_AMD64_INTEGER | CONTEXT_AMD64_FLOATING_POINT)

typedef struct {
	uint64_t Low;
	uint64_t High;
} WiboM128;

typedef struct {
	uint16_t ControlWord;
	uint16_t StatusWord;
	uint8_t TagWord;
	uint8_t Reserved1;
	uint16_t ErrorOpcode;
	uint32_t ErrorOffset;
	uint16_t ErrorSelector;
	uint16_t Reserved2;
	uint32_t DataOffset;
	uint16_t DataSelector;
	uint16_t Reserved3;
	uint32_t MxCsr;
	uint32_t MxCsrMask;
	WiboM128 FloatRegisters[8];
	WiboM128 XmmRegisters[16];
	uint8_t Reserved4[96];
} WiboXmmSaveArea;

typedef struct {
	uint64_t P1Home;
	uint64_t P2Home;
	uint64_t P3Home;
	uint64_t P4Home;
	uint64_t P5Home;
	uint64_t P6Home;
	uint32_t ContextFlags;
	uint32_t MxCsr;
	uint16_t SegCs;
	uint16_t SegDs;
	uint16_t SegEs;
	uint16_t SegFs;
	uint16_t SegGs;
	uint16_t SegSs;
	uint32_t EFlags;
	uint64_t Dr0;
	uint64_t Dr1;
	uint64_t Dr2;
	uint64_t Dr3;
	uint64_t Dr6;
	uint64_t Dr7;
	uint64_t Rax;
	uint64_t Rcx;
	uint64_t Rdx;
	uint64_t Rbx;
	uint64_t Rsp;
	uint64_t Rbp;
	uint64_t Rsi;
	uint64_t Rdi;
	uint64_t R8;
	uint64_t R9;
	uint64_t R10;
	uint64_t R11;
	uint64_t R12;
	uint64_t R13;
	uint64_t R14;
	uint64_t R15;
	uint64_t Rip;
	WiboXmmSaveArea FltSave;
	WiboM128 VectorRegister[26];
	uint64_t VectorControl;
	uint64_t DebugControl;
	uint64_t LastBranchToRip;
	uint64_t LastBranchFromRip;
	uint64_t LastExceptionToRip;
	uint64_t LastExceptionFromRip;
} WiboWin64Context __attribute__((aligned(16)));

_Static_assert(sizeof(WiboWin64Context) == 0x4d0, "AMD64 CONTEXT size");
_Static_assert(offsetof(WiboWin64Context, Rsp) == 0x98, "AMD64 CONTEXT Rsp offset");
_Static_assert(offsetof(WiboWin64Context, Rip) == 0xf8, "AMD64 CONTEXT Rip offset");
_Static_assert(offsetof(WiboWin64Context, FltSave) + offsetof(WiboXmmSaveArea, XmmRegisters) == 0x1a0,
			   "AMD64 CONTEXT Xmm0 offset");

static inline void wibo_contextToUnwind(const WiboWin64Context *context, WiboContext64 *out) {
	const uint64_t gprs[16] = {context->Rax, context->Rcx,	context->Rdx, context->Rbx, context->Rsp, context->Rbp,
							   context->Rsi, context->Rdi,	context->R8,  context->R9,	context->R10, context->R11,
							   context->R12, context->R13,	context->R14, context->R15};
	for (int i = 0; i < 16; i++) {
		out->gpr[i] = gprs[i];
		out->xmm[i][0] = context->FltSave.XmmRegisters[i].Low;
		out->xmm[i][1] = context->FltSave.XmmRegisters[i].High;
	}
	out->rip = context->Rip;
}

static inline void wibo_contextFromUnwind(const WiboContext64 *in, WiboWin64Context *context) {
	uint64_t *gprs[16] = {&context->Rax, &context->Rcx,	&context->Rdx, &context->Rbx, &context->Rsp, &context->Rbp,
						  &context->Rsi, &context->Rdi,	&context->R8,  &context->R9,  &context->R10, &context->R11,
						  &context->R12, &context->R13,	&context->R14, &context->R15};
	for (int i = 0; i < 16; i++) {
		*gprs[i] = in->gpr[i];
		context->FltSave.XmmRegisters[i].Low = in->xmm[i][0];
		context->FltSave.XmmRegisters[i].High = in->xmm[i][1];
	}
	context->Rip = in->rip;
}

// Values returned by a language handler. They match the winnt.h EXCEPTION_DISPOSITION enumeration, except
// WiboExceptionUnsupportedUnwind, which a handler returns when it needs an unwind that wibo does not perform yet.
typedef enum {
	WiboExceptionContinueExecution = 0,
	WiboExceptionContinueSearch = 1,
	WiboExceptionNestedException = 2,
	WiboExceptionCollidedUnwind = 3,
	WiboExceptionUnsupportedUnwind = 4,
} WiboExceptionDisposition;

// DISPATCHER_CONTEXT (winnt.h, 0x50 bytes). Handlers receive a pointer to it as their fourth argument.
typedef struct {
	uint64_t ControlPc;
	uint64_t ImageBase;
	const WiboRuntimeFunction *FunctionEntry;
	uint64_t EstablisherFrame;
	uint64_t TargetIp;
	WiboWin64Context *ContextRecord;
	uint64_t LanguageHandler;
	void *HandlerData;
	void *HistoryTable;
	uint32_t ScopeIndex;
	uint32_t Fill0;
} WiboDispatcherContext;

_Static_assert(sizeof(WiboDispatcherContext) == 0x50, "DISPATCHER_CONTEXT size");
_Static_assert(offsetof(WiboDispatcherContext, ContextRecord) == 0x28, "DISPATCHER_CONTEXT ContextRecord offset");
_Static_assert(offsetof(WiboDispatcherContext, ScopeIndex) == 0x48, "DISPATCHER_CONTEXT ScopeIndex offset");
