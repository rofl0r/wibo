#pragma once

#include "minwinbase.h"
#include "types.h"

typedef struct {
	DWORD dwFileAttributes;
	FILETIME ftCreationTime;
	FILETIME ftLastAccessTime;
	FILETIME ftLastWriteTime;
	DWORD dwVolumeSerialNumber;
	DWORD nFileSizeHigh;
	DWORD nFileSizeLow;
	DWORD nNumberOfLinks;
	DWORD nFileIndexHigh;
	DWORD nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION;

typedef BY_HANDLE_FILE_INFORMATION *PBY_HANDLE_FILE_INFORMATION;
typedef BY_HANDLE_FILE_INFORMATION *LPBY_HANDLE_FILE_INFORMATION;

#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5

#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2

#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_COPY_ALLOWED 0x00000002
#define MOVEFILE_DELAY_UNTIL_REBOOT 0x00000004
#define MOVEFILE_WRITE_THROUGH 0x00000008

#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_OFFLINE 0x00001000
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x00002000
#define FILE_ATTRIBUTE_ENCRYPTED 0x00004000

#define DRIVE_UNKNOWN 0
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_REMOVABLE 2
#define DRIVE_FIXED 3
#define DRIVE_REMOTE 4
#define DRIVE_CDROM 5
#define DRIVE_RAMDISK 6

#define FILE_TYPE_UNKNOWN 0x0000
#define FILE_TYPE_DISK 0x0001
#define FILE_TYPE_CHAR 0x0002
#define FILE_TYPE_PIPE 0x0003

#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFF
#define INVALID_FILE_SIZE 0xFFFFFFFF

DWORD WINAPI GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, GUEST_PTR *lpFilePart);
DWORD WINAPI GetFullPathNameW(LPCWSTR lpFileName, DWORD nBufferLength, LPWSTR lpBuffer, GUEST_PTR *lpFilePart);
DWORD WINAPI GetShortPathNameA(LPCSTR lpszLongPath, LPSTR lpszShortPath, DWORD cchBuffer);
DWORD WINAPI GetShortPathNameW(LPCWSTR lpszLongPath, LPWSTR lpszShortPath, DWORD cchBuffer);
UINT WINAPI GetTempFileNameA(LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName);
UINT WINAPI GetTempFileNameW(LPCWSTR lpPathName, LPCWSTR lpPrefixString, UINT uUnique, LPWSTR lpTempFileName);
DWORD WINAPI GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer);
DWORD WINAPI GetTempPathW(DWORD nBufferLength, LPWSTR lpBuffer);
HANDLE WINAPI FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData);
HANDLE WINAPI FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);
HANDLE WINAPI FindFirstFileExA(LPCSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData,
							   FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags);
HANDLE WINAPI FindFirstFileExW(LPCWSTR lpFileName, FINDEX_INFO_LEVELS fInfoLevelId, LPVOID lpFindFileData,
							   FINDEX_SEARCH_OPS fSearchOp, LPVOID lpSearchFilter, DWORD dwAdditionalFlags);
BOOL WINAPI FindNextFileA(HANDLE hFindFile, LPWIN32_FIND_DATAA lpFindFileData);
BOOL WINAPI FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData);
BOOL WINAPI FindClose(HANDLE hFindFile);
DWORD WINAPI GetFileAttributesA(LPCSTR lpFileName);
DWORD WINAPI GetFileAttributesW(LPCWSTR lpFileName);
BOOL WINAPI GetFileAttributesExA(LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
BOOL WINAPI GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);
UINT WINAPI GetDriveTypeA(LPCSTR lpRootPathName);
UINT WINAPI GetDriveTypeW(LPCWSTR lpRootPathName);
BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten,
					  LPOVERLAPPED lpOverlapped);
BOOL WINAPI ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead,
					 LPOVERLAPPED lpOverlapped);
BOOL WINAPI FlushFileBuffers(HANDLE hFile);
HANDLE WINAPI CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
						  LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
						  DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
HANDLE WINAPI CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
						  LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
						  DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL WINAPI DeleteFileA(LPCSTR lpFileName);
BOOL WINAPI DeleteFileW(LPCWSTR lpFileName);
BOOL WINAPI MoveFileA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName);
BOOL WINAPI MoveFileW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName);
BOOL WINAPI MoveFileExA(LPCSTR lpExistingFileName, LPCSTR lpNewFileName, DWORD dwFlags);
BOOL WINAPI MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags);
DWORD WINAPI SetFilePointer(HANDLE hFile, LONG lDistanceToMove, PLONG lpDistanceToMoveHigh, DWORD dwMoveMethod);
BOOL WINAPI SetFilePointerEx(HANDLE hFile, LARGE_INTEGER liDistanceToMove, PLARGE_INTEGER lpNewFilePointer,
							 DWORD dwMoveMethod);
BOOL WINAPI SetEndOfFile(HANDLE hFile);
BOOL WINAPI CreateDirectoryA(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
BOOL WINAPI RemoveDirectoryA(LPCSTR lpPathName);
BOOL WINAPI SetFileAttributesA(LPCSTR lpFileName, DWORD dwFileAttributes);
DWORD WINAPI GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh);
BOOL WINAPI GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime,
						LPFILETIME lpLastWriteTime);
BOOL WINAPI SetFileTime(HANDLE hFile, const FILETIME *lpCreationTime, const FILETIME *lpLastAccessTime,
						const FILETIME *lpLastWriteTime);
BOOL WINAPI GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation);
DWORD WINAPI GetFileType(HANDLE hFile);
LONG WINAPI CompareFileTime(const FILETIME *lpFileTime1, const FILETIME *lpFileTime2);
BOOL WINAPI GetVolumeInformationA(LPCSTR lpRootPathName, LPSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
								  LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength,
								  LPDWORD lpFileSystemFlags, LPSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize);
BOOL WINAPI GetVolumeInformationW(LPCWSTR lpRootPathName, LPWSTR lpVolumeNameBuffer, DWORD nVolumeNameSize,
								  LPDWORD lpVolumeSerialNumber, LPDWORD lpMaximumComponentLength,
								  LPDWORD lpFileSystemFlags, LPWSTR lpFileSystemNameBuffer, DWORD nFileSystemNameSize);
