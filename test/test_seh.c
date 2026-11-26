#include "test_assert.h"
#include <stdlib.h>
#include <windows.h>

// Raw x86 SEH: the frames below are built with __try1/__except1 from excpt.h, so the try bodies must not depend on
// the compiler keeping locals across the exception.

static volatile LONG g_continueHits;
static volatile LONG g_searchHits;
static volatile LONG g_faultHits;

// Labels are defined in assembly so the fault can resume at a known address with the same stack.
int faultSite(void);
void recoverSite(void);
__asm__(".text\n"
		".globl _faultSite\n"
		"_faultSite:\n"
		"\txorl %eax, %eax\n"
		"\tmovl (%eax), %ecx\n"
		"\tmovl $1, %eax\n"
		"\tret\n"
		".globl _recoverSite\n"
		"_recoverSite:\n"
		"\tmovl $2, %eax\n"
		"\tret\n");

static EXCEPTION_DISPOSITION __cdecl continueHandler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context,
													 void *dispatch) {
	(void)record;
	(void)frame;
	(void)context;
	(void)dispatch;
	InterlockedIncrement(&g_continueHits);
	return ExceptionContinueExecution;
}

static EXCEPTION_DISPOSITION __cdecl searchHandler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context,
												   void *dispatch) {
	(void)record;
	(void)frame;
	(void)context;
	(void)dispatch;
	InterlockedIncrement(&g_searchHits);
	return ExceptionContinueSearch;
}

static EXCEPTION_DISPOSITION __cdecl recoverHandler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context,
													void *dispatch) {
	(void)frame;
	(void)dispatch;
	InterlockedIncrement(&g_faultHits);
	if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
		return ExceptionContinueSearch;
	}
	context->Eip = (DWORD)(uintptr_t)recoverSite;
	return ExceptionContinueExecution;
}

// The raw __try1 pushes the registration with asm, which the compiler does not see, so the try bodies must not pass
// arguments or keep locals in esp-relative slots. Calls go through no-argument helpers, and results go to globals.
__attribute__((noinline)) static void raiseCode(void) {
	RaiseException(0x1234, 0, 0, NULL);
}

__attribute__((noinline)) static void raiseWithContinue(void) {
	__try1(continueHandler)
	raiseCode();
	__except1
}

__attribute__((noinline)) static void raiseNested(void) {
	__try1(continueHandler)
	__try1(searchHandler)
	raiseCode();
	__except1
	__except1
}

static volatile int g_faultResult;

static int runFault(void) {
	__try1(recoverHandler)
	g_faultResult = faultSite();
	__except1
	return g_faultResult;
}

static int __cdecl raiseInComparator(const void *a, const void *b) {
	(void)a;
	(void)b;
	raiseWithContinue();
	return 0;
}

int main(void) {
	raiseWithContinue();
	TEST_CHECK_EQ(1, g_continueHits);

	raiseNested();
	TEST_CHECK_EQ(1, g_searchHits);
	TEST_CHECK_EQ(2, g_continueHits);

	TEST_CHECK_EQ(2, runFault());
	TEST_CHECK_EQ(1, g_faultHits);

	LONG before = g_continueHits;
	int values[2] = {2, 1};
	qsort(values, 2, sizeof(values[0]), raiseInComparator);
	TEST_CHECK(g_continueHits > before);
	return 0;
}
