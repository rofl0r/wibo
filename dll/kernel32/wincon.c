#include "wincon.h"

#include "context.h"
#include "errors.h"
#include "files.h"
#include "handles.h"
#include "internal.h"
#include "strutil.h"

#include <unistd.h>

BOOL WINAPI GetConsoleMode(HANDLE hConsoleHandle, LPDWORD lpMode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetConsoleMode(%p)\n", hConsoleHandle);
	if (lpMode) {
		*lpMode = 0;
	}
	return TRUE;
}

BOOL WINAPI SetConsoleMode(HANDLE hConsoleHandle, DWORD dwMode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetConsoleMode(%p, 0x%x)\n", hConsoleHandle, dwMode);
	(void)hConsoleHandle;
	(void)dwMode;
	return TRUE;
}

UINT WINAPI GetConsoleCP(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetConsoleCP() -> 65001\n");
	return 65001; // UTF-8
}

UINT WINAPI GetConsoleOutputCP(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetConsoleOutputCP() -> 65001\n");
	return 65001; // UTF-8
}

BOOL WINAPI SetConsoleCtrlHandler(PHANDLER_ROUTINE HandlerRoutine, BOOL Add) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetConsoleCtrlHandler(%p, %u)\n", (const void *)HandlerRoutine, Add);
	(void)HandlerRoutine;
	(void)Add;
	return TRUE;
}

BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE hConsoleOutput, CONSOLE_SCREEN_BUFFER_INFO *lpConsoleScreenBufferInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetConsoleScreenBufferInfo(%p, %p)\n", hConsoleOutput, lpConsoleScreenBufferInfo);
	(void)hConsoleOutput;
	if (!lpConsoleScreenBufferInfo) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	lpConsoleScreenBufferInfo->dwSize = (COORD){80, 25};
	lpConsoleScreenBufferInfo->dwCursorPosition = (COORD){0, 0};
	lpConsoleScreenBufferInfo->wAttributes = 0;
	lpConsoleScreenBufferInfo->srWindow = (SMALL_RECT){0, 0, 79, 24};
	lpConsoleScreenBufferInfo->dwMaximumWindowSize = (COORD){80, 25};
	return TRUE;
}

BOOL WINAPI WriteConsoleW(HANDLE hConsoleOutput, LPCWSTR lpBuffer, DWORD nNumberOfCharsToWrite,
						  LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WriteConsoleW(%p, %p, %u, %p, %p)\n", hConsoleOutput, lpBuffer, nNumberOfCharsToWrite,
			  lpNumberOfCharsWritten, lpReserved);
	(void)lpReserved;
	if (lpNumberOfCharsWritten) {
		*lpNumberOfCharsWritten = 0;
	}
	if (!lpBuffer && nNumberOfCharsToWrite != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	FileObject *file = (FileObject *)Handles_getAs(wibo_handles(), hConsoleOutput, ObjectType_File, NULL);
	if (file->base.fd == STDOUT_FILENO || file->base.fd == STDERR_FILENO) {
		char *str = wideStringToString(lpBuffer, (int)nNumberOfCharsToWrite);
		IOResult io = files_write(file, str, nNumberOfCharsToWrite, NULL, true);
		free(str);
		ObjectBase_unref(&file->base.base);
		if (lpNumberOfCharsWritten) {
			*lpNumberOfCharsWritten = (DWORD)io.bytesTransferred;
		}
		if (io.unixError != 0) {
			kernel32_setLastError(wibo_winErrorFromErrno(io.unixError));
			return FALSE;
		}
		return TRUE;
	}

	ObjectBase_unref(&file->base.base);
	kernel32_setLastError(ERROR_INVALID_HANDLE);
	return FALSE;
}

DWORD WINAPI GetConsoleTitleA(LPSTR lpConsoleTitle, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetConsoleTitleA(%p, %u)\n", lpConsoleTitle, nSize);
	if (lpConsoleTitle && nSize > 0) {
		lpConsoleTitle[0] = '\0';
	}
	kernel32_setLastError(ERROR_SUCCESS);
	return 0;
}

DWORD WINAPI GetConsoleTitleW(LPWSTR lpConsoleTitle, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetConsoleTitleW(%p, %u)\n", lpConsoleTitle, nSize);
	if (lpConsoleTitle && nSize > 0) {
		lpConsoleTitle[0] = 0;
	}
	kernel32_setLastError(ERROR_SUCCESS);
	return 0;
}

BOOL WINAPI PeekConsoleInputA(HANDLE hConsoleInput, INPUT_RECORD *lpBuffer, DWORD nLength,
							  LPDWORD lpNumberOfEventsRead) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: PeekConsoleInputA(%p, %p, %u)\n", hConsoleInput, lpBuffer, nLength);
	(void)hConsoleInput;
	(void)lpBuffer;
	(void)nLength;
	if (lpNumberOfEventsRead) {
		*lpNumberOfEventsRead = 0;
	}
	return TRUE;
}

BOOL WINAPI ReadConsoleInputA(HANDLE hConsoleInput, INPUT_RECORD *lpBuffer, DWORD nLength,
							  LPDWORD lpNumberOfEventsRead) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: ReadConsoleInputA(%p, %p, %u)\n", hConsoleInput, lpBuffer, nLength);
	(void)hConsoleInput;
	(void)lpBuffer;
	(void)nLength;
	if (lpNumberOfEventsRead) {
		*lpNumberOfEventsRead = 0;
	}
	return TRUE;
}

BOOL WINAPI VerifyConsoleIoHandle(HANDLE handle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: VerifyConsoleIoHandle(%p)\n", handle);
	(void)handle;
	return FALSE;
}
