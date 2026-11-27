#include "test_assert.h"
#include <stdlib.h>
#include <windows.h>

// Win64 structured exception handling through __C_specific_handler. mingw's x64 front ends do not accept __try, so the
// guards are hand-assembled: each guard calls its body (rcx) inside one scope-table entry whose filter is the named C
// function. The .pdata/.xdata records are written out explicitly so the scope table sits right after the handler RVA.

static volatile LONG g_continueHits;
static volatile LONG g_searchHits;
static volatile LONG g_afterRaise;
static volatile LONG g_value;

LONG continueFilter(EXCEPTION_POINTERS *info, void *frame) {
	(void)info;
	(void)frame;
	g_continueHits++;
	return EXCEPTION_CONTINUE_EXECUTION;
}

LONG searchFilter(EXCEPTION_POINTERS *info, void *frame) {
	(void)info;
	(void)frame;
	g_searchHits++;
	return EXCEPTION_CONTINUE_SEARCH;
}

LONG fixPointerFilter(EXCEPTION_POINTERS *info, void *frame) {
	(void)frame;
	if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	// The faulting instruction is re-executed with the registers from ContextRecord, so the fix goes there.
	info->ContextRecord->Rax = (DWORD64)&g_value;
	g_continueHits++;
	return EXCEPTION_CONTINUE_EXECUTION;
}

#define DEFINE_GUARD(name, filter)                                                  \
	__asm__(                                                                        \
		".text\n"                                                                   \
		".globl " #name "\n"                                                        \
		#name ":\n"                                                                 \
		"  subq $40, %rsp\n"                                                        \
		#name "_begin:\n"                                                           \
		"  call *%rcx\n"                                                            \
		"  nop\n"                                                                   \
		#name "_end:\n"                                                             \
		"  addq $40, %rsp\n"                                                        \
		"  ret\n"                                                                   \
		#name "_func_end:\n"                                                        \
		".section .xdata,\"dr\"\n"                                                  \
		".p2align 2\n"                                                              \
		#name "_unwind:\n"                                                          \
		"  .byte 0x09, 4, 1, 0\n"                                                   \
		"  .byte 4, 0x42, 0, 0\n"                                                   \
		"  .rva __C_specific_handler\n"                                             \
		"  .long 1\n"                                                               \
		"  .rva " #name "_begin, " #name "_end, " #filter ", " #name "_end\n"       \
		".section .pdata,\"dr\"\n"                                                  \
		".p2align 2\n"                                                              \
		"  .rva " #name ", " #name "_func_end, " #name "_unwind\n"                  \
		".text\n")

DEFINE_GUARD(guardContinue, continueFilter);
DEFINE_GUARD(guardSearch, searchFilter);
DEFINE_GUARD(guardFixPointer, fixPointerFilter);

void guardContinue(void (*body)(void));
void guardSearch(void (*body)(void));
void guardFixPointer(void (*body)(void));

static void raiseBody(void) {
	RaiseException(0x1234, 0, 0, NULL);
	g_afterRaise++;
}

static void faultBody(void) {
	__asm__ volatile("xorl %%eax, %%eax\n\t"
					 "movl $7, (%%rax)\n\t"
					 :
					 :
					 : "rax", "memory");
	g_afterRaise++;
}

static void searchThenRaiseBody(void) {
	guardSearch(raiseBody);
}

static void searchThenFaultBody(void) {
	guardSearch(faultBody);
}

static volatile LONG g_comparisons;
static volatile LONG g_raised;

static int raisingCompare(const void *a, const void *b) {
	const int *x = a;
	const int *y = b;
	g_comparisons++;
	if (!g_raised) {
		g_raised = 1;
		RaiseException(0x1234, 0, 0, NULL);
	}
	return *x - *y;
}

static void qsortBody(void) {
	int values[4] = {4, 3, 2, 1};
	qsort(values, 4, sizeof(values[0]), raisingCompare);
	TEST_CHECK(values[0] == 1 && values[3] == 4);
}

static void resetState(void) {
	g_continueHits = 0;
	g_searchHits = 0;
	g_afterRaise = 0;
	g_value = 0;
}

int main(void) {
	resetState();
	guardContinue(raiseBody);
	TEST_CHECK_EQ(1, g_continueHits);
	TEST_CHECK_EQ(0, g_searchHits);
	TEST_CHECK_EQ(1, g_afterRaise);

	resetState();
	guardContinue(searchThenRaiseBody);
	TEST_CHECK_EQ(1, g_searchHits);
	TEST_CHECK_EQ(1, g_continueHits);
	TEST_CHECK_EQ(1, g_afterRaise);

	resetState();
	guardFixPointer(faultBody);
	TEST_CHECK_EQ(7, g_value);
	TEST_CHECK_EQ(1, g_continueHits);
	TEST_CHECK_EQ(1, g_afterRaise);

	resetState();
	guardFixPointer(searchThenFaultBody);
	TEST_CHECK_EQ(7, g_value);
	TEST_CHECK_EQ(1, g_continueHits);
	TEST_CHECK_EQ(1, g_afterRaise);

	resetState();
	g_raised = 0;
	g_comparisons = 0;
	guardContinue(qsortBody);
	TEST_CHECK(g_comparisons > 0);
	TEST_CHECK_EQ(1, g_raised);
	return 0;
}
