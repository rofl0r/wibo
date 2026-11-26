#pragma once

#include "types.h"

typedef struct COORD {
	SHORT X;
	SHORT Y;
} COORD;

typedef struct SMALL_RECT {
	SHORT Left;
	SHORT Top;
	SHORT Right;
	SHORT Bottom;
} SMALL_RECT;

typedef struct CONSOLE_SCREEN_BUFFER_INFO {
	COORD dwSize;
	COORD dwCursorPosition;
	WORD wAttributes;
	SMALL_RECT srWindow;
	COORD dwMaximumWindowSize;
} CONSOLE_SCREEN_BUFFER_INFO;

typedef struct INPUT_RECORD INPUT_RECORD;

typedef BOOL(_CC_STDCALL *PHANDLER_ROUTINE)(DWORD CtrlType);

BOOL WINAPI GetConsoleMode(HANDLE hConsoleHandle, LPDWORD lpMode);
BOOL WINAPI SetConsoleMode(HANDLE hConsoleHandle, DWORD dwMode);
UINT WINAPI GetConsoleCP(void);
UINT WINAPI GetConsoleOutputCP(void);
BOOL WINAPI SetConsoleCtrlHandler(PHANDLER_ROUTINE HandlerRoutine, BOOL Add);
BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE hConsoleOutput, CONSOLE_SCREEN_BUFFER_INFO *lpConsoleScreenBufferInfo);
BOOL WINAPI WriteConsoleW(HANDLE hConsoleOutput, LPCWSTR lpBuffer, DWORD nNumberOfCharsToWrite,
						  LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved);
DWORD WINAPI GetConsoleTitleA(LPSTR lpConsoleTitle, DWORD nSize);
DWORD WINAPI GetConsoleTitleW(LPWSTR lpConsoleTitle, DWORD nSize);
BOOL WINAPI PeekConsoleInputA(HANDLE hConsoleInput, INPUT_RECORD *lpBuffer, DWORD nLength,
							  LPDWORD lpNumberOfEventsRead);
BOOL WINAPI ReadConsoleInputA(HANDLE hConsoleInput, INPUT_RECORD *lpBuffer, DWORD nLength,
							  LPDWORD lpNumberOfEventsRead);
BOOL WINAPI VerifyConsoleIoHandle(HANDLE handle);
