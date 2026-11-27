#include "test_assert.h"
#include <stdint.h>
#include <windows.h>

// Win64 unwinding through __C_specific_handler: EXECUTE_HANDLER landings, __finally funclets run during an unwind, and
// RtlUnwind from guest code. Guards are hand-assembled as in test_seh64.c. Each guard also records its establisher
// frame and landing address in slots, so the body can name the guard it wants to unwind to. Each .xdata record sets
// both EHANDLER and UHANDLER, as a frame that owns __finally scopes must.

static volatile LONG g_landings;
static volatile LONG g_execHits;
static volatile LONG g_finallyRuns;
static volatile LONG g_finallyAbnormal;
static volatile LONG g_afterRaise;
static volatile LONG g_afterUnwind;

LONG executeFilter(EXCEPTION_POINTERS *info, void *frame) {
	(void)info;
	(void)frame;
	g_execHits++;
	return EXCEPTION_EXECUTE_HANDLER;
}

void finallyFunclet(unsigned char abnormal, void *frame) {
	(void)frame;
	g_finallyRuns++;
	g_finallyAbnormal = abnormal;
}

#define DEFINE_GUARD(name, handler, jumpEntry)                                              \
	__asm__(                                                                                \
		".text\n"                                                                           \
		".globl " #name "\n"                                                                \
		#name ":\n"                                                                         \
		"  subq $40, %rsp\n"                                                                \
		"  movq %rsp, " #name "_frame(%rip)\n"                                              \
		"  leaq " #name "_landing(%rip), %rax\n"                                            \
		"  movq %rax, " #name "_ip(%rip)\n"                                                 \
		#name "_begin:\n"                                                                   \
		"  call *%rcx\n"                                                                    \
		"  xorl %eax, %eax\n"                                                               \
		#name "_end:\n"                                                                     \
		"  addq $40, %rsp\n"                                                                \
		"  ret\n"                                                                           \
		#name "_landing:\n"                                                                 \
		"  incl g_landings(%rip)\n"                                                         \
		"  addq $40, %rsp\n"                                                                \
		"  ret\n"                                                                           \
		#name "_func_end:\n"                                                                \
		".section .xdata,\"dr\"\n"                                                          \
		".p2align 2\n"                                                                      \
		#name "_unwind:\n"                                                                  \
		"  .byte 0x19, 4, 1, 0\n"                                                           \
		"  .byte 4, 0x42, 0, 0\n"                                                           \
		"  .rva __C_specific_handler\n"                                                     \
		"  .long 1\n"                                                                       \
		"  .rva " #name "_begin, " #name "_end, " #handler "\n"                             \
		"  " jumpEntry "\n"                                                                 \
		".section .pdata,\"dr\"\n"                                                          \
		".p2align 2\n"                                                                      \
		"  .rva " #name ", " #name "_func_end, " #name "_unwind\n"                          \
		".data\n"                                                                           \
		".p2align 3\n"                                                                      \
		".globl " #name "_frame\n"                                                          \
		#name "_frame: .quad 0\n"                                                           \
		".globl " #name "_ip\n"                                                             \
		#name "_ip: .quad 0\n"                                                              \
		".text\n")

// The landing scope jumps to the guard's landing label; a finally-only scope has JumpTarget 0.
DEFINE_GUARD(guardExec, executeFilter, ".rva guardExec_landing");
DEFINE_GUARD(guardFinally, finallyFunclet, ".long 0");
DEFINE_GUARD(guardTarget, executeFilter, ".rva guardTarget_landing");

extern volatile uint64_t guardTarget_frame;
extern volatile uint64_t guardTarget_ip;

LONG_PTR guardExec(void (*body)(void));
LONG_PTR guardFinally(void (*body)(void));
LONG_PTR guardTarget(void (*body)(void));

static void raiseBody(void) {
	RaiseException(0x1234, 0, 0, NULL);
	g_afterRaise++;
}

static void finallyThenRaise(void) {
	guardFinally(raiseBody);
}

static void unwindBody(void) {
	RtlUnwind((PVOID)(uintptr_t)guardTarget_frame, (PVOID)(uintptr_t)guardTarget_ip, NULL, (PVOID)0x77);
	g_afterUnwind++;
}

static void finallyThenUnwind(void) {
	guardFinally(unwindBody);
	g_afterUnwind++;
}

static void resetState(void) {
	g_landings = 0;
	g_execHits = 0;
	g_finallyRuns = 0;
	g_finallyAbnormal = 0;
	g_afterRaise = 0;
	g_afterUnwind = 0;
}

int main(void) {
	resetState();
	LONG_PTR rc = guardExec(raiseBody);
	// The value seen at an __except landing is not part of the contract (MSVC landing code does not read it), so only
	// the landing itself is checked.
	TEST_CHECK_EQ(1, g_landings);
	TEST_CHECK_EQ(1, g_execHits);
	TEST_CHECK_EQ(0, g_afterRaise);

	resetState();
	rc = guardExec(finallyThenRaise);
	TEST_CHECK_EQ(1, g_landings);
	TEST_CHECK_EQ(1, g_finallyRuns);
	TEST_CHECK_EQ(1, g_finallyAbnormal);
	TEST_CHECK_EQ(0, g_afterRaise);

	// RtlUnwind documents ReturnValue as the value the target sees in RAX.
	resetState();
	rc = guardTarget(finallyThenUnwind);
	TEST_CHECK_EQ(0x77, rc);
	TEST_CHECK_EQ(1, g_landings);
	TEST_CHECK_EQ(1, g_finallyRuns);
	TEST_CHECK_EQ(1, g_finallyAbnormal);
	TEST_CHECK_EQ(0, g_afterUnwind);
	return 0;
}
