#include "test_assert.h"
#include <string.h>
#include <windows.h>

// Exercises RtlLookupFunctionEntry and RtlVirtualUnwind on x64 .pdata/.xdata. probe() records the pc, rsp and rbp of
// its caller, so each target can be unwound one frame without relying on exception dispatch.

static volatile uintptr_t g_probePc;
static volatile uintptr_t g_probeRsp;
static volatile uintptr_t g_probeRbp;
static volatile uintptr_t g_frameReturn;
static volatile uintptr_t g_frameRbp;
static volatile uintptr_t g_allocReturn;

static __attribute__((noinline)) void probe(void) {
	uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
	g_probePc = (uintptr_t)__builtin_return_address(0);
	g_probeRsp = frame + 16;
	g_probeRbp = *(uintptr_t *)frame;
}

// Unwinds the pc/rsp/rbp captured by probe() one frame. Must run while the probing frame is still live, since its
// return address slot is overwritten once it returns.
static DWORD64 checkProbeUnwind(uintptr_t expectedPc) {
	CONTEXT ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.ContextFlags = CONTEXT_FULL;
	ctx.Rip = g_probePc;
	ctx.Rsp = g_probeRsp;
	ctx.Rbp = g_probeRbp;
	DWORD64 imageBase = 0;
	PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx.Rip, &imageBase, NULL);
	TEST_CHECK(function != NULL);
	DWORD64 establisher = 0;
	PVOID handlerData = NULL;
	PEXCEPTION_ROUTINE handler =
		RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, function, &ctx, &handlerData, &establisher, NULL);
	TEST_CHECK(handler == NULL);
	TEST_CHECK_U64_EQ(expectedPc, ctx.Rip);
	return ctx.Rsp;
}

// Frame-pointer function: unwinds through UWOP_SET_FPREG and UWOP_PUSH_NONVOL.
static __attribute__((noinline)) void frameTarget(void) {
	g_frameReturn = (uintptr_t)__builtin_return_address(0);
	g_frameRbp = (uintptr_t)__builtin_frame_address(0);
	probe();
	DWORD64 rsp = checkProbeUnwind(g_frameReturn);
	TEST_CHECK_U64_EQ(g_frameRbp + 16, rsp);
	__asm__ volatile("" ::: "memory");
}

// Large stack frame: unwinds through UWOP_ALLOC_LARGE.
static __attribute__((noinline)) void allocTarget(void) {
	volatile char buffer[300];
	g_allocReturn = (uintptr_t)__builtin_return_address(0);
	buffer[0] = 1;
	probe();
	checkProbeUnwind(g_allocReturn);
	buffer[1] = buffer[0];
}

static void checkLookup(void (*target)(void)) {
	DWORD64 imageBase = 0;
	PRUNTIME_FUNCTION function = RtlLookupFunctionEntry((DWORD64)(uintptr_t)target, &imageBase, NULL);
	TEST_CHECK(function != NULL);
	TEST_CHECK_U64_EQ((DWORD64)(uintptr_t)GetModuleHandleW(NULL), imageBase);
	DWORD64 rva = (DWORD64)(uintptr_t)target - imageBase;
	TEST_CHECK(rva >= function->BeginAddress && rva < function->EndAddress);
}

int main(void) {
	checkLookup(frameTarget);
	checkLookup(allocTarget);

	// Stack addresses belong to no module and must not resolve.
	volatile int local = 0;
	DWORD64 imageBase = 0;
	TEST_CHECK(RtlLookupFunctionEntry((DWORD64)(uintptr_t)&local, &imageBase, NULL) == NULL);

	frameTarget();
	allocTarget();

	printf("unwind64 ok\n");
	return 0;
}
