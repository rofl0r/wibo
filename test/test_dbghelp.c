#include <stdlib.h>
#include <windows.h>

#include "test_assert.h"

// The movi tool loads Dbghelp dynamically and takes a different (unbalanced)
// code path when the load fails, so the load itself must succeed.
typedef BOOL(WINAPI *SymInitializeFn)(HANDLE, PCSTR, BOOL);

int main(void) {
	HMODULE dbghelp = LoadLibraryA("Dbghelp.dll");
	TEST_CHECK_MSG(dbghelp != NULL, "LoadLibraryA(Dbghelp.dll) failed: %lu", GetLastError());

	TEST_CHECK(GetProcAddress(dbghelp, "SymSetOptions") != NULL);
	TEST_CHECK(GetProcAddress(dbghelp, "StackWalk64") != NULL);
	TEST_CHECK(GetProcAddress(dbghelp, "SymGetSymFromAddr64") != NULL);

	SymInitializeFn symInitialize = (SymInitializeFn)GetProcAddress(dbghelp, "SymInitialize");
	TEST_CHECK(symInitialize != NULL);
	TEST_CHECK(symInitialize(GetCurrentProcess(), NULL, FALSE));

	TEST_CHECK(FreeLibrary(dbghelp));
	return EXIT_SUCCESS;
}
