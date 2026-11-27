#include "timezoneapi.h"

#include "context.h"
#include "errors.h"
#include "internal.h"
#include "timeutil.h"

#include <errno.h>
#include <string.h>
#include <time.h>

BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *lpSystemTime, LPFILETIME lpFileTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SystemTimeToFileTime(%p, %p)\n", lpSystemTime, lpFileTime);
	if (!lpSystemTime || !lpFileTime) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	int64_t seconds = 0;
	uint32_t hundreds = 0;
	if (!systemTimeToUnixParts(lpSystemTime, &seconds, &hundreds)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	FILETIME result;
	if (!unixPartsToFileTime(seconds, hundreds, &result)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (fileTimeToDuration(result) >= MAX_VALID_FILETIME) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*lpFileTime = result;
	return TRUE;
}

BOOL WINAPI FileTimeToSystemTime(const FILETIME *lpFileTime, LPSYSTEMTIME lpSystemTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FileTimeToSystemTime(%p, %p)\n", lpFileTime, lpSystemTime);
	if (!lpFileTime || !lpSystemTime) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	uint64_t ticks = fileTimeToDuration(*lpFileTime);
	if (ticks >= MAX_VALID_FILETIME) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	uint64_t daysSince1601 = ticks / TICKS_PER_DAY;
	uint64_t ticksOfDay = ticks % TICKS_PER_DAY;
	uint32_t secondsOfDay = (uint32_t)(ticksOfDay / HUNDRED_NS_PER_SECOND);
	uint32_t hundredNs = (uint32_t)(ticksOfDay % HUNDRED_NS_PER_SECOND);
	int64_t daysSince1970 = (int64_t)daysSince1601 - DAYS_TO_UNIX_EPOCH;
	CivilDate date = civilFromDays(daysSince1970);
	lpSystemTime->wYear = (WORD)date.year;
	lpSystemTime->wMonth = (WORD)date.month;
	lpSystemTime->wDay = (WORD)date.day;
	lpSystemTime->wDayOfWeek = (WORD)((daysSince1601 + 1ULL) % 7ULL);
	lpSystemTime->wHour = (WORD)(secondsOfDay / 3600U);
	lpSystemTime->wMinute = (WORD)((secondsOfDay % 3600U) / 60U);
	lpSystemTime->wSecond = (WORD)(secondsOfDay % 60U);
	lpSystemTime->wMilliseconds = (WORD)(hundredNs / HUNDRED_NS_PER_MILLISECOND);
	return TRUE;
}

BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *lpFileTime, LPFILETIME lpLocalFileTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FileTimeToLocalFileTime(%p, %p)\n", lpFileTime, lpLocalFileTime);
	if (!lpFileTime || !lpLocalFileTime) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	int64_t seconds = 0;
	uint32_t hundreds = 0;
	if (!fileTimeToUnixParts(*lpFileTime, &seconds, &hundreds)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (seconds > (int64_t)TIME_T_MAX || seconds < (int64_t)TIME_T_MIN) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	time_t unixTime = (time_t)seconds;
	struct tm localTm = {0};
#if defined(_POSIX_VERSION)
	if (!localtime_r(&unixTime, &localTm)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
#else
	struct tm *tmp = localtime(&unixTime);
	if (!tmp) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	localTm = *tmp;
#endif
	int64_t localAsUtcSeconds = 0;
	if (!tmToUnixSeconds(&localTm, &localAsUtcSeconds)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	int64_t offsetSeconds = localAsUtcSeconds - seconds;
	int64_t localSeconds = seconds + offsetSeconds;
	FILETIME result;
	if (!unixPartsToFileTime(localSeconds, hundreds, &result)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*lpLocalFileTime = result;
	return TRUE;
}

BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *lpLocalFileTime, LPFILETIME lpFileTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LocalFileTimeToFileTime(%p, %p)\n", lpLocalFileTime, lpFileTime);
	if (!lpLocalFileTime || !lpFileTime) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	uint64_t ticks = fileTimeToDuration(*lpLocalFileTime);
	if (ticks >= MAX_VALID_FILETIME) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	uint32_t hundredNs = (uint32_t)(ticks % HUNDRED_NS_PER_SECOND);
	uint64_t daysSince1601 = ticks / TICKS_PER_DAY;
	uint64_t ticksOfDay = ticks % TICKS_PER_DAY;
	uint32_t secondsOfDay = (uint32_t)(ticksOfDay / HUNDRED_NS_PER_SECOND);
	int64_t daysSince1970 = (int64_t)daysSince1601 - DAYS_TO_UNIX_EPOCH;
	CivilDate date = civilFromDays(daysSince1970);
	struct tm localTm = {0};
	localTm.tm_year = date.year - 1900;
	localTm.tm_mon = (int)date.month - 1;
	localTm.tm_mday = (int)date.day;
	localTm.tm_hour = (int)(secondsOfDay / 3600U);
	localTm.tm_min = (int)((secondsOfDay % 3600U) / 60U);
	localTm.tm_sec = (int)(secondsOfDay % 60U);
	localTm.tm_isdst = -1;
	struct tm tmCopy = localTm;
	errno = 0;
	time_t utcTime = mktime(&tmCopy);
	if (utcTime == (time_t)-1 && errno != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	FILETIME result;
	if (!unixPartsToFileTime((int64_t)utcTime, hundredNs, &result)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*lpFileTime = result;
	return TRUE;
}

BOOL WINAPI DosDateTimeToFileTime(WORD wFatDate, WORD wFatTime, LPFILETIME lpFileTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DosDateTimeToFileTime(%04x, %04x, %p)\n", wFatDate, wFatTime, lpFileTime);
	if (!lpFileTime) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	unsigned day = wFatDate & 0x1F;
	unsigned month = (wFatDate >> 5) & 0x0F;
	unsigned year = ((wFatDate >> 9) & 0x7F) + 1980;
	unsigned second = (wFatTime & 0x1F) * 2;
	unsigned minute = (wFatTime >> 5) & 0x3F;
	unsigned hour = (wFatTime >> 11) & 0x1F;
	if (day == 0 || month == 0 || month > 12 || day > 31 || hour > 23 || minute > 59 || second > 59) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	struct tm tmValue = {0};
	tmValue.tm_year = (int)year - 1900;
	tmValue.tm_mon = (int)month - 1;
	tmValue.tm_mday = (int)day;
	tmValue.tm_hour = (int)hour;
	tmValue.tm_min = (int)minute;
	tmValue.tm_sec = (int)second;
	tmValue.tm_isdst = -1;
	time_t localSeconds = mktime(&tmValue);
	if (localSeconds == (time_t)-1) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	uint64_t ticks = ((uint64_t)localSeconds + UNIX_TIME_ZERO / HUNDRED_NS_PER_SECOND) * 10000000ULL;
	*lpFileTime = fileTimeFromDuration(ticks);
	return TRUE;
}

BOOL WINAPI FileTimeToDosDateTime(const FILETIME *lpFileTime, LPWORD lpFatDate, LPWORD lpFatTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FileTimeToDosDateTime(%p, %p, %p)\n", lpFileTime, lpFatDate, lpFatTime);
	if (!lpFileTime || !lpFatDate || !lpFatTime) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	uint64_t ticks = fileTimeToDuration(*lpFileTime);
	if (ticks < UNIX_TIME_ZERO) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	time_t utcSeconds = (time_t)((ticks / 10000000ULL) - (UNIX_TIME_ZERO / HUNDRED_NS_PER_SECOND));
	struct tm tmValue = {0};
	if (!localtime_r(&utcSeconds, &tmValue)) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	int year = tmValue.tm_year + 1900;
	if (year < 1980 || year > 2107) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*lpFatDate = (WORD)(((year - 1980) << 9) | ((tmValue.tm_mon + 1) << 5) | tmValue.tm_mday);
	*lpFatTime = (WORD)(((tmValue.tm_hour & 0x1F) << 11) | ((tmValue.tm_min & 0x3F) << 5) |
						((tmValue.tm_sec / 2) & 0x1F));
	return TRUE;
}

static void copyTimeZoneName(WCHAR *dest, const char *src) {
	if (!src) {
		dest[0] = 0;
		return;
	}
	for (size_t i = 0; i < 31 && src[i]; ++i) {
		dest[i] = (unsigned char)src[i];
		dest[i + 1] = 0;
	}
}

DWORD WINAPI GetTimeZoneInformation(LPTIME_ZONE_INFORMATION lpTimeZoneInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetTimeZoneInformation(%p)\n", lpTimeZoneInformation);
	if (!lpTimeZoneInformation) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return TIME_ZONE_ID_INVALID;
	}
	memset(lpTimeZoneInformation, 0, sizeof(*lpTimeZoneInformation));
	tzset();
	time_t now = time(NULL);
	struct tm localTm = {0};
#if defined(_GNU_SOURCE) || defined(__APPLE__)
	localtime_r(&now, &localTm);
#else
	struct tm *tmp = localtime(&now);
	if (tmp) {
		localTm = *tmp;
	}
#endif
	long offsetSeconds = 0;
#if defined(__APPLE__) || defined(__linux__)
	offsetSeconds = -localTm.tm_gmtoff;
#else
	extern long timezone;
	offsetSeconds = timezone;
	if (localTm.tm_isdst > 0) {
		extern int daylight;
		if (daylight) {
			offsetSeconds -= 3600;
		}
	}
#endif
	lpTimeZoneInformation->Bias = (LONG)(offsetSeconds / 60);
	copyTimeZoneName(lpTimeZoneInformation->StandardName, tzname[0]);
	const char *daylightName = (daylight && tzname[1]) ? tzname[1] : tzname[0];
	copyTimeZoneName(lpTimeZoneInformation->DaylightName, daylightName);
	DWORD result = TIME_ZONE_ID_UNKNOWN;
	if (daylight && localTm.tm_isdst > 0) {
		lpTimeZoneInformation->DaylightBias = -60;
		result = TIME_ZONE_ID_DAYLIGHT;
	} else {
		result = TIME_ZONE_ID_STANDARD;
	}
	return result;
}
