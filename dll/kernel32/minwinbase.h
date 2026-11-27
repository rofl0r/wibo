#pragma once

#include "types.h"

typedef struct SECURITY_ATTRIBUTES {
	DWORD nLength;
	GUEST_PTR lpSecurityDescriptor;
	BOOL bInheritHandle;
} SECURITY_ATTRIBUTES;

typedef SECURITY_ATTRIBUTES *PSECURITY_ATTRIBUTES;
typedef SECURITY_ATTRIBUTES *LPSECURITY_ATTRIBUTES;

typedef struct FILETIME {
	DWORD dwLowDateTime;
	DWORD dwHighDateTime;
} FILETIME;

typedef FILETIME *PFILETIME;
typedef FILETIME *LPFILETIME;

typedef struct SYSTEMTIME {
	WORD wYear;
	WORD wMonth;
	WORD wDayOfWeek;
	WORD wDay;
	WORD wHour;
	WORD wMinute;
	WORD wSecond;
	WORD wMilliseconds;
} SYSTEMTIME;

typedef SYSTEMTIME *PSYSTEMTIME;
typedef SYSTEMTIME *LPSYSTEMTIME;

typedef enum FINDEX_INFO_LEVELS { FindExInfoStandard, FindExInfoBasic, FindExInfoMaxInfoLevel } FINDEX_INFO_LEVELS;

typedef enum FINDEX_SEARCH_OPS {
	FindExSearchNameMatch,
	FindExSearchLimitToDirectories,
	FindExSearchLimitToDevices,
	FindExSearchMaxSearchOp
} FINDEX_SEARCH_OPS;

typedef enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel } GET_FILEEX_INFO_LEVELS;

#define FILE_ATTRIBUTE_DIRECTORY ((DWORD)0x00000010)
#define FILE_ATTRIBUTE_NORMAL ((DWORD)0x00000080)

#define MAX_PATH ((DWORD)260)

typedef struct WIN32_FIND_DATAA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD dwReserved0;
	DWORD dwReserved1;
	CHAR cFileName[MAX_PATH];
	CHAR cAlternateFileName[14];
} WIN32_FIND_DATAA;

typedef struct WIN32_FIND_DATAW {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD dwReserved0;
	DWORD dwReserved1;
	WCHAR cFileName[MAX_PATH];
	WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW;

typedef struct WIN32_FILE_ATTRIBUTE_DATA {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

typedef WIN32_FIND_DATAA *PWIN32_FIND_DATAA;
typedef WIN32_FIND_DATAA *LPWIN32_FIND_DATAA;
typedef WIN32_FIND_DATAW *PWIN32_FIND_DATAW;
typedef WIN32_FIND_DATAW *LPWIN32_FIND_DATAW;
typedef WIN32_FILE_ATTRIBUTE_DATA *PWIN32_FILE_ATTRIBUTE_DATA;
typedef WIN32_FILE_ATTRIBUTE_DATA *LPWIN32_FILE_ATTRIBUTE_DATA;

typedef struct _OVERLAPPED {
	ULONG_PTR Internal;
	ULONG_PTR InternalHigh;
	union {
		struct {
			DWORD Offset;
			DWORD OffsetHigh;
		};
		GUEST_PTR Pointer;
	};
	HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;
