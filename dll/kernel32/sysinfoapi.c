#include "sysinfoapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "ntdll.h"
#include "timeutil.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#define PROCESSOR_ARCHITECTURE_INTEL ((WORD)0)
#define PROCESSOR_INTEL_PENTIUM ((DWORD)586)

#define kMajorVersion ((DWORD)6)
#define kMinorVersion ((DWORD)2)
#define kBuildNumber ((DWORD)0)

static DWORD_PTR computeSystemProcessorMask(unsigned int cpuCount) {
	const unsigned int maskWidth = (unsigned int)(sizeof(DWORD_PTR) * 8);
	if (cpuCount >= maskWidth) {
		return (DWORD_PTR)~(DWORD_PTR)0;
	}
	DWORD_PTR mask = ((DWORD_PTR)1 << cpuCount) - 1;
	return mask == 0 ? 1 : mask;
}

void WINAPI GetSystemInfo(LPSYSTEM_INFO lpSystemInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemInfo(%p)\n", lpSystemInfo);
	if (!lpSystemInfo) {
		return;
	}

	memset(lpSystemInfo, 0, sizeof(*lpSystemInfo));
	lpSystemInfo->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL;
	lpSystemInfo->dwOemId = lpSystemInfo->wProcessorArchitecture;
	lpSystemInfo->dwProcessorType = PROCESSOR_INTEL_PENTIUM;
	lpSystemInfo->wProcessorLevel = 6; // Pentium

	long pageSize = sysconf(_SC_PAGESIZE);
	if (pageSize <= 0) {
		pageSize = 4096;
	}
	lpSystemInfo->dwPageSize = (DWORD)pageSize;

	lpSystemInfo->lpMinimumApplicationAddress = toGuestPtr((void *)(uintptr_t)0x00010000);
#ifdef _WIN64
	lpSystemInfo->lpMaximumApplicationAddress = toGuestPtr((void *)(uintptr_t)0x00007FFFFFFEFFFFull);
#else
	lpSystemInfo->lpMaximumApplicationAddress = toGuestPtr((void *)(uintptr_t)0x7FFEFFFF);
#endif

	unsigned int cpuCount = 1;
	long reported = sysconf(_SC_NPROCESSORS_ONLN);
	if (reported > 0) {
		cpuCount = (unsigned int)reported;
	}
	lpSystemInfo->dwNumberOfProcessors = cpuCount;
	lpSystemInfo->dwActiveProcessorMask = computeSystemProcessorMask(cpuCount);

	lpSystemInfo->dwAllocationGranularity = 0x10000;
}

void WINAPI GetSystemTime(LPSYSTEMTIME lpSystemTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemTime(%p)\n", lpSystemTime);
	if (!lpSystemTime) {
		return;
	}

	time_t now = time(NULL);
	struct tm tmUtc = {0};
#if defined(_GNU_SOURCE) || defined(__APPLE__)
	gmtime_r(&now, &tmUtc);
#else
	struct tm *tmp = gmtime(&now);
	if (!tmp) {
		return;
	}
	tmUtc = *tmp;
#endif

	lpSystemTime->wYear = (WORD)(tmUtc.tm_year + 1900);
	lpSystemTime->wMonth = (WORD)(tmUtc.tm_mon + 1);
	lpSystemTime->wDayOfWeek = (WORD)tmUtc.tm_wday;
	lpSystemTime->wDay = (WORD)tmUtc.tm_mday;
	lpSystemTime->wHour = (WORD)tmUtc.tm_hour;
	lpSystemTime->wMinute = (WORD)tmUtc.tm_min;
	lpSystemTime->wSecond = (WORD)tmUtc.tm_sec;
	lpSystemTime->wMilliseconds = 0;
}

void WINAPI GetLocalTime(LPSYSTEMTIME lpSystemTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLocalTime(%p)\n", lpSystemTime);
	if (!lpSystemTime) {
		return;
	}

	time_t now = time(NULL);
	struct tm tmLocal = {0};
#if defined(_GNU_SOURCE) || defined(__APPLE__)
	localtime_r(&now, &tmLocal);
#else
	struct tm *tmp = localtime(&now);
	if (!tmp) {
		return;
	}
	tmLocal = *tmp;
#endif

	lpSystemTime->wYear = (WORD)(tmLocal.tm_year + 1900);
	lpSystemTime->wMonth = (WORD)(tmLocal.tm_mon + 1);
	lpSystemTime->wDayOfWeek = (WORD)tmLocal.tm_wday;
	lpSystemTime->wDay = (WORD)tmLocal.tm_mday;
	lpSystemTime->wHour = (WORD)tmLocal.tm_hour;
	lpSystemTime->wMinute = (WORD)tmLocal.tm_min;
	lpSystemTime->wSecond = (WORD)tmLocal.tm_sec;
	lpSystemTime->wMilliseconds = 0;
}

void WINAPI GetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemTimeAsFileTime(%p)\n", lpSystemTimeAsFileTime);
	if (!lpSystemTimeAsFileTime) {
		return;
	}

#if defined(CLOCK_REALTIME)
	struct timespec ts = {0};
	if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
		uint64_t ticks = UNIX_TIME_ZERO;
		ticks += (uint64_t)ts.tv_sec * 10000000ULL;
		ticks += (uint64_t)ts.tv_nsec / 100ULL;
		*lpSystemTimeAsFileTime = fileTimeFromDuration(ticks);
		return;
	}
#endif

	struct timeval tv = {0};
	if (gettimeofday(&tv, NULL) == 0) {
		uint64_t ticks = UNIX_TIME_ZERO;
		ticks += (uint64_t)tv.tv_sec * 10000000ULL;
		ticks += (uint64_t)tv.tv_usec * 10ULL;
		*lpSystemTimeAsFileTime = fileTimeFromDuration(ticks);
		return;
	}

	const FILETIME fallback = {(DWORD)(UNIX_TIME_ZERO & 0xFFFFFFFFULL), (DWORD)(UNIX_TIME_ZERO >> 32)};
	*lpSystemTimeAsFileTime = fallback;
}

DWORD WINAPI GetTickCount(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetTickCount()\n");
#if defined(CLOCK_MONOTONIC)
	struct timespec ts = {0};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		uint64_t milliseconds = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
		DWORD result = (DWORD)(milliseconds & 0xFFFFFFFFULL);
		DEBUG_LOG(" -> %u\n", result);
		return result;
	}
#endif
	struct timeval tv = {0};
	if (gettimeofday(&tv, NULL) == 0) {
		uint64_t milliseconds = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
		DWORD result = (DWORD)(milliseconds & 0xFFFFFFFFULL);
		DEBUG_LOG(" -> %u\n", result);
		return result;
	}
	DEBUG_LOG(" -> 0\n");
	return 0;
}

ULONGLONG WINAPI GetTickCount64(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetTickCount64()\n");
	struct timespec ts = {0};
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		ULONGLONG result = (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)ts.tv_nsec / 1000000ULL;
		DEBUG_LOG(" -> %llu\n", (unsigned long long)result);
		return result;
	}
	DEBUG_LOG(" -> 0\n");
	return 0;
}

DWORD WINAPI GetVersion(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetVersion()\n");
	return kMajorVersion | (kMinorVersion << 8) | (5 << 16) | (kBuildNumber << 24);
}

BOOL WINAPI GetVersionExA(LPOSVERSIONINFOA lpVersionInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetVersionExA(%p)\n", lpVersionInformation);
	if (!lpVersionInformation) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	DWORD size = lpVersionInformation->dwOSVersionInfoSize;
	if (size < sizeof(OSVERSIONINFOA)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	DWORD requestSize = (size >= sizeof(OSVERSIONINFOEXA)) ? sizeof(OSVERSIONINFOEXW) : sizeof(OSVERSIONINFOW);
	OSVERSIONINFOEXW wideInfo = {0};
	wideInfo.dwOSVersionInfoSize = requestSize;
	NTSTATUS status = RtlGetVersion((PRTL_OSVERSIONINFOW)&wideInfo);
	if (status != STATUS_SUCCESS) {
		kernel32_setLastError(wibo_winErrorFromNtStatus(status));
		return FALSE;
	}

	memset(lpVersionInformation, 0, size);
	lpVersionInformation->dwOSVersionInfoSize = size;
	lpVersionInformation->dwMajorVersion = wideInfo.dwMajorVersion;
	lpVersionInformation->dwMinorVersion = wideInfo.dwMinorVersion;
	lpVersionInformation->dwBuildNumber = wideInfo.dwBuildNumber;
	lpVersionInformation->dwPlatformId = wideInfo.dwPlatformId;

	if (size >= sizeof(OSVERSIONINFOEXA)) {
		OSVERSIONINFOEXA *extended = (OSVERSIONINFOEXA *)lpVersionInformation;
		extended->wServicePackMajor = wideInfo.wServicePackMajor;
		extended->wServicePackMinor = wideInfo.wServicePackMinor;
		extended->wSuiteMask = wideInfo.wSuiteMask;
		extended->wProductType = wideInfo.wProductType;
		extended->wReserved = wideInfo.wReserved;
	}

	return TRUE;
}

BOOL WINAPI GetVersionExW(LPOSVERSIONINFOW lpVersionInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetVersionExW(%p)\n", lpVersionInformation);
	if (!lpVersionInformation) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	DWORD size = lpVersionInformation->dwOSVersionInfoSize;
	if (size < sizeof(OSVERSIONINFOW)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	NTSTATUS status = RtlGetVersion((PRTL_OSVERSIONINFOW)lpVersionInformation);
	if (status != STATUS_SUCCESS) {
		kernel32_setLastError(wibo_winErrorFromNtStatus(status));
		return FALSE;
	}

	lpVersionInformation->dwOSVersionInfoSize = size;
	return TRUE;
}
