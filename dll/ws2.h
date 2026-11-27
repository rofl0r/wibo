#pragma once

#include "types.h"

typedef struct WSADATA {
	WORD wVersion;
	WORD wHighVersion;
	CHAR szDescription[257];
	CHAR szSystemStatus[129];
	WORD iMaxSockets;
	WORD iMaxUdpDg;
	GUEST_PTR lpVendorInfo;
} WSADATA;

// gethostname, gethostbyname and select are exported under those names; the C identifiers avoid clashing with libc.
int WINAPI WSAStartup(WORD wVersionRequired, WSADATA *lpWSAData);
int WINAPI WSACleanup(void);
int WINAPI WSAGetLastError(void);
int WINAPI ws2_gethostname(LPSTR name, int namelen);
GUEST_PTR WINAPI ws2_gethostbyname(LPCSTR name);
int WINAPI ws2_select(int nfds, LPVOID readfds, LPVOID writefds, LPVOID exceptfds, const void *timeout);
