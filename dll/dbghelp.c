#include "dbghelp.h"

#include "common.h"
#include "context.h"
#include "modules.h"

// Minimal Dbghelp: enough for guests that load it for crash/stack reporting to get a
// consistent "no symbol support" answer instead of a load failure. No real unwinding.

BOOL WINAPI SymInitialize(HANDLE hProcess, LPCSTR UserSearchPath, BOOL fInvadeProcess) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymInitialize(%p, %p, %d)\n", (void *)(uintptr_t)hProcess, UserSearchPath, fInvadeProcess);
	return TRUE;
}

DWORD WINAPI SymSetOptions(DWORD SymOptions) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymSetOptions(0x%x)\n", SymOptions);
	return 0;
}

BOOL WINAPI SymGetModuleInfo64(HANDLE hProcess, ULONGLONG qwAddr, PVOID ModuleInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymGetModuleInfo64(%p, 0x%llx, %p)\n", (void *)(uintptr_t)hProcess, qwAddr, ModuleInfo);
	return FALSE;
}

ULONGLONG WINAPI SymGetModuleBase64(HANDLE hProcess, ULONGLONG qwAddr) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymGetModuleBase64(%p, 0x%llx)\n", (void *)(uintptr_t)hProcess, qwAddr);
	return 0;
}

BOOL WINAPI SymGetSymFromAddr64(HANDLE hProcess, ULONGLONG qwAddr, ULONGLONG *pdwDisplacement, PVOID Symbol) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymGetSymFromAddr64(%p, 0x%llx, %p, %p)\n", (void *)(uintptr_t)hProcess, qwAddr, pdwDisplacement,
			  Symbol);
	return FALSE;
}

BOOL WINAPI SymGetLineFromAddr64(HANDLE hProcess, ULONGLONG qwAddr, PDWORD pdwDisplacement, PVOID Line) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymGetLineFromAddr64(%p, 0x%llx, %p, %p)\n", (void *)(uintptr_t)hProcess, qwAddr, pdwDisplacement, Line);
	return FALSE;
}

PVOID WINAPI SymFunctionTableAccess64(HANDLE hProcess, ULONGLONG AddrBase) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SymFunctionTableAccess64(%p, 0x%llx)\n", (void *)(uintptr_t)hProcess, AddrBase);
	return NULL;
}

BOOL WINAPI StackWalk64(DWORD MachineType, HANDLE hProcess, HANDLE hThread, PVOID StackFrame, PVOID ContextRecord,
						PVOID ReadMemoryRoutine, PVOID FunctionTableAccessRoutine, PVOID GetModuleBaseRoutine,
						PVOID TranslateAddress) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("StackWalk64(%u, %p, %p, %p, %p, %p, %p, %p, %p)\n", MachineType, (void *)(uintptr_t)hProcess,
			  (void *)(uintptr_t)hThread, StackFrame, ContextRecord, ReadMemoryRoutine, FunctionTableAccessRoutine,
			  GetModuleBaseRoutine, TranslateAddress);
	return FALSE;
}

BOOL WINAPI EnumerateLoadedModules64(HANDLE hProcess, PVOID EnumLoadedModulesCallback, PVOID UserContext) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EnumerateLoadedModules64(%p, %p, %p)\n", (void *)(uintptr_t)hProcess, EnumLoadedModulesCallback,
			  UserContext);
	return FALSE;
}

BOOL WINAPI MiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, DWORD DumpType, PVOID ExceptionParam,
							  PVOID UserStreamParam, PVOID CallbackParam) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MiniDumpWriteDump(%p, %u, %p, 0x%x, %p, %p, %p)\n", (void *)(uintptr_t)hProcess, ProcessId,
			  (void *)(uintptr_t)hFile, DumpType, ExceptionParam, UserStreamParam, CallbackParam);
	return FALSE;
}

#include "dbghelp_trampolines.h"

const ModuleStub lib_dbghelp = {
	.names =
		(const char *[]){
			"dbghelp",
			NULL,
		},
	.byName = dbghelpThunkByName,
};
