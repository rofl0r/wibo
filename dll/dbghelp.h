#pragma once

#include "types.h"

BOOL WINAPI SymInitialize(HANDLE hProcess, LPCSTR UserSearchPath, BOOL fInvadeProcess);
DWORD WINAPI SymSetOptions(DWORD SymOptions);
BOOL WINAPI SymGetModuleInfo64(HANDLE hProcess, ULONGLONG qwAddr, PVOID ModuleInfo);
ULONGLONG WINAPI SymGetModuleBase64(HANDLE hProcess, ULONGLONG qwAddr);
BOOL WINAPI SymGetSymFromAddr64(HANDLE hProcess, ULONGLONG qwAddr, ULONGLONG *pdwDisplacement, PVOID Symbol);
BOOL WINAPI SymGetLineFromAddr64(HANDLE hProcess, ULONGLONG qwAddr, PDWORD pdwDisplacement, PVOID Line);
PVOID WINAPI SymFunctionTableAccess64(HANDLE hProcess, ULONGLONG AddrBase);
BOOL WINAPI StackWalk64(DWORD MachineType, HANDLE hProcess, HANDLE hThread, PVOID StackFrame, PVOID ContextRecord,
						PVOID ReadMemoryRoutine, PVOID FunctionTableAccessRoutine, PVOID GetModuleBaseRoutine,
						PVOID TranslateAddress);
BOOL WINAPI EnumerateLoadedModules64(HANDLE hProcess, PVOID EnumLoadedModulesCallback, PVOID UserContext);
BOOL WINAPI MiniDumpWriteDump(HANDLE hProcess, DWORD ProcessId, HANDLE hFile, DWORD DumpType, PVOID ExceptionParam,
							  PVOID UserStreamParam, PVOID CallbackParam);
