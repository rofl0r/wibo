#pragma once

#include "types.h"
#include "minwinbase.h"

BOOL WINAPI CreatePipe(PHANDLE hReadPipe, PHANDLE hWritePipe, LPSECURITY_ATTRIBUTES lpPipeAttributes, DWORD nSize);
HANDLE WINAPI CreateNamedPipeA(LPCSTR lpName, DWORD dwOpenMode, DWORD dwPipeMode, DWORD nMaxInstances,
								 DWORD nOutBufferSize, DWORD nInBufferSize, DWORD nDefaultTimeOut,
								 LPSECURITY_ATTRIBUTES lpSecurityAttributes);
BOOL WINAPI ConnectNamedPipe(HANDLE hNamedPipe, LPOVERLAPPED lpOverlapped);
bool kernel32_tryCreateFileNamedPipeA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
									  LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
									  DWORD dwFlagsAndAttributes, HANDLE *outHandle);
