#pragma once

#include "minwinbase.h"
#include "types.h"

typedef struct SYSTEM_INFO {
	union {
		DWORD dwOemId;
		struct {
			WORD wProcessorArchitecture;
			WORD wReserved;
		};
	};
	DWORD dwPageSize;
	GUEST_PTR lpMinimumApplicationAddress;
	GUEST_PTR lpMaximumApplicationAddress;
	DWORD_PTR dwActiveProcessorMask;
	DWORD dwNumberOfProcessors;
	DWORD dwProcessorType;
	DWORD dwAllocationGranularity;
	WORD wProcessorLevel;
	WORD wProcessorRevision;
} SYSTEM_INFO;

typedef SYSTEM_INFO *LPSYSTEM_INFO;

typedef struct OSVERSIONINFOA {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	char szCSDVersion[128];
} OSVERSIONINFOA;

typedef OSVERSIONINFOA *LPOSVERSIONINFOA;

typedef struct OSVERSIONINFOW {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	WCHAR szCSDVersion[128];
} OSVERSIONINFOW;

typedef OSVERSIONINFOW *LPOSVERSIONINFOW;

// C has no struct inheritance, so the EX variants repeat the base fields with the Windows SDK layout.
typedef struct OSVERSIONINFOEXA {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	char szCSDVersion[128];
	WORD wServicePackMajor;
	WORD wServicePackMinor;
	WORD wSuiteMask;
	BYTE wProductType;
	BYTE wReserved;
} OSVERSIONINFOEXA;

typedef OSVERSIONINFOEXA *LPOSVERSIONINFOEXA;

typedef struct OSVERSIONINFOEXW {
	DWORD dwOSVersionInfoSize;
	DWORD dwMajorVersion;
	DWORD dwMinorVersion;
	DWORD dwBuildNumber;
	DWORD dwPlatformId;
	WCHAR szCSDVersion[128];
	WORD wServicePackMajor;
	WORD wServicePackMinor;
	WORD wSuiteMask;
	BYTE wProductType;
	BYTE wReserved;
} OSVERSIONINFOEXW;

typedef OSVERSIONINFOEXW *LPOSVERSIONINFOEXW;

void WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo);
void WINAPI GetSystemTime(LPSYSTEMTIME lpSystemTime);
void WINAPI GetLocalTime(LPSYSTEMTIME lpSystemTime);
void WINAPI GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime);
DWORD WINAPI GetTickCount(void);
ULONGLONG WINAPI GetTickCount64(void);
DWORD WINAPI GetVersion(void);
BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA lpVersionInformation);
BOOL WINAPI GetVersionExW(LPOSVERSIONINFOW lpVersionInformation);
