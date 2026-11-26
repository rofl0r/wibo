#include "ws2.h"

#include "common.h"
#include "context.h"
#include "modules.h"

#include <string.h>
#include <unistd.h>

#define SOCKET_ERROR (-1)
#define WSAEFAULT 10014
#define WSAHOST_NOT_FOUND 11001
#define WSANOTINITIALISED 10093

static __thread int g_lastError = 0;
static int g_startupCount = 0;

static void setLastError(int error) { g_lastError = error; }

static WORD makeVersion(BYTE major, BYTE minor) { return (WORD)(major | (minor << 8)); }

static bool requireStarted(void) {
	if (g_startupCount > 0) {
		return true;
	}
	setLastError(WSANOTINITIALISED);
	return false;
}

int WINAPI WSAStartup(WORD wVersionRequired, WSADATA *lpWSAData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSAStartup(0x%x, %p)\n", wVersionRequired, lpWSAData);
	if (!lpWSAData) {
		setLastError(WSAEFAULT);
		return WSAEFAULT;
	}

	memset(lpWSAData, 0, sizeof(*lpWSAData));
	lpWSAData->wVersion = wVersionRequired;
	lpWSAData->wHighVersion = makeVersion(2, 2);
	strncpy(lpWSAData->szDescription, "wibo fake Winsock", sizeof(lpWSAData->szDescription) - 1);
	strncpy(lpWSAData->szSystemStatus, "Running", sizeof(lpWSAData->szSystemStatus) - 1);
	lpWSAData->iMaxSockets = 0x7fff;
	lpWSAData->iMaxUdpDg = 65467; // 65535 - max IPv4 header (60) - UDP header (8)

	++g_startupCount;
	setLastError(0);
	return 0;
}

int WINAPI WSACleanup(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSACleanup()\n");
	if (g_startupCount <= 0) {
		setLastError(WSANOTINITIALISED);
		return SOCKET_ERROR;
	}

	--g_startupCount;
	setLastError(0);
	return 0;
}

int WINAPI WSAGetLastError(void) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSAGetLastError() -> %d\n", g_lastError);
	return g_lastError;
}

int WINAPI ws2_gethostname(LPSTR name, int namelen) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("gethostname(%p, %d)\n", name, namelen);
	if (!requireStarted()) {
		return SOCKET_ERROR;
	}
	if (!name || namelen <= 0) {
		setLastError(WSAEFAULT);
		return SOCKET_ERROR;
	}

	char host[256] = {0};
	if (gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0') {
		strncpy(host, "localhost", sizeof(host) - 1);
	}

	size_t length = strlen(host);
	if ((size_t)namelen <= length) {
		setLastError(WSAEFAULT);
		return SOCKET_ERROR;
	}

	memcpy(name, host, length + 1);
	setLastError(0);
	return 0;
}

GUEST_PTR WINAPI ws2_gethostbyname(LPCSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("gethostbyname(%s)\n", name ? name : "(null)");
	if (!requireStarted()) {
		return GUEST_NULL;
	}
	setLastError(WSAHOST_NOT_FOUND);
	return GUEST_NULL;
}

int WINAPI ws2_select(int nfds, LPVOID readfds, LPVOID writefds, LPVOID exceptfds, const void *timeout) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("select(%d, %p, %p, %p, %p)\n", nfds, readfds, writefds, exceptfds, timeout);
	if (!requireStarted()) {
		return SOCKET_ERROR;
	}
	(void)nfds;
	(void)readfds;
	(void)writefds;
	(void)exceptfds;
	(void)timeout;
	setLastError(0);
	return 0;
}

#include "ws2_trampolines.h"

static void *resolveByOrdinal(uint16_t ordinal) {
	// GHS 5.3.22 imports WS2_32.dll with the legacy winsock ordinal table.
	// Keep these mappings tied to observed call sites rather than modern WS2_32 export ordinals.
	switch (ordinal) {
	case 18:
		return (void *)thunk_ws2_select;
	case 52:
		return (void *)thunk_ws2_gethostbyname;
	case 57:
		return (void *)thunk_ws2_gethostname;
	case 115:
		return (void *)thunk_ws2_WSAStartup;
	case 116:
		return (void *)thunk_ws2_WSACleanup;
	}
	return NULL;
}

const ModuleStub lib_ws2 = {
	.names =
		(const char *[]){
			"WS2_32",
			NULL,
		},
	.byName = ws2ThunkByName,
	.byOrdinal = resolveByOrdinal,
};
