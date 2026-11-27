#include "winbase.h"

#include "common.h"
#include "context.h"
#include "cutil.h"
#include "errors.h"
#include "internal.h"
#include "kernel32/internal.h"
#include "strutil.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static const WCHAR kAccountSystem[] = {u'S', u'Y', u'S', u'T', u'E', u'M', u'\0'};
static const WCHAR kDomainNtAuthority[] = {u'N', u'T', u' ', u'A', u'U', u'T', u'H', u'O', u'R', u'I', u'T', u'Y', u'\0'};
static const BYTE kNtAuthority[6] = {0, 0, 0, 0, 0, 5};

static const struct {
	const char *name;
	DWORD lowPart;
} kPredefinedPrivileges[] = {
	{"se_debug_name", 0x14},
	{"se_shutdown_name", 0x13},
};

static pthread_mutex_t g_privilegeMapMutex = PTHREAD_MUTEX_INITIALIZER;
static hashmap *g_privilegeLuidCache;

static bool isLocalSystemSid(const Sid *sid) {
	if (!sid) {
		return false;
	}
	if (sid->Revision != 1 || sid->SubAuthorityCount != 1) {
		return false;
	}
	for (size_t i = 0; i < sizeof(kNtAuthority); ++i) {
		if (sid->IdentifierAuthority.Value[i] != kNtAuthority[i]) {
			return false;
		}
	}
	return sid->SubAuthority[0] == SECURITY_LOCAL_SYSTEM_RID;
}

// Returns a malloc'd lowercase copy of `name`.
static char *normalizePrivilegeName(const char *name) {
	size_t len = strlen(name);
	char *normalized = xmalloc(len + 1);
	for (size_t i = 0; i < len; ++i) {
		normalized[i] = (char)tolower((unsigned char)name[i]);
	}
	normalized[len] = '\0';
	return normalized;
}

static LUID generateDeterministicLuid(const char *normalizedName) {
	uint32_t hash = 2166136261u;
	for (const unsigned char *p = (const unsigned char *)normalizedName; *p; ++p) {
		hash ^= *p;
		hash *= 16777619u;
	}
	if (hash == 0) {
		hash = 1;
	}
	LUID luid = {0};
	luid.LowPart = hash;
	luid.HighPart = 0;
	return luid;
}

static LUID lookupOrGeneratePrivilegeLuid(const char *normalizedName) {
	pthread_mutex_lock(&g_privilegeMapMutex);
	if (!g_privilegeLuidCache) {
		g_privilegeLuidCache = hashmap_create();
	}
	size_t keyLen = strlen(normalizedName);
	LUID *cached = NULL;
	LUID luid = {0};
	if (hashmap_find(g_privilegeLuidCache, normalizedName, keyLen, (void **)&cached)) {
		luid = *cached;
	} else {
		bool predefined = false;
		for (size_t i = 0; i < sizeof(kPredefinedPrivileges) / sizeof(kPredefinedPrivileges[0]); ++i) {
			if (str_eq(normalizedName, kPredefinedPrivileges[i].name)) {
				luid.LowPart = kPredefinedPrivileges[i].lowPart;
				luid.HighPart = 0;
				predefined = true;
				break;
			}
		}
		if (!predefined) {
			luid = generateDeterministicLuid(normalizedName);
		}
		cached = xmalloc(sizeof(*cached));
		*cached = luid;
		hashmap_put(g_privilegeLuidCache, normalizedName, keyLen, cached);
	}
	pthread_mutex_unlock(&g_privilegeMapMutex);
	return luid;
}

BOOL WINAPI LookupAccountSidW(LPCWSTR lpSystemName, PSID Sid, LPWSTR Name, LPDWORD cchName, LPWSTR ReferencedDomainName,
							  LPDWORD cchReferencedDomainName, SID_NAME_USE *peUse) {
	HOST_CONTEXT_GUARD();
	char *systemName = lpSystemName ? wideStringToString(lpSystemName, -1) : NULL;
	DEBUG_LOG("LookupAccountSidW(%s, %p, %p, %p, %p, %p, %p)\n", systemName ? systemName : "(null)", Sid, Name, cchName,
			  ReferencedDomainName, cchReferencedDomainName, peUse);
	free(systemName);
	if (!Sid || !cchName || !cchReferencedDomainName || !peUse) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const struct Sid *sidStruct = (const struct Sid *)Sid;
	if (!isLocalSystemSid(sidStruct)) {
		kernel32_setLastError(ERROR_NONE_MAPPED);
		return FALSE;
	}
	DWORD requiredAccount = (DWORD)wstrlen(kAccountSystem);
	DWORD requiredDomain = (DWORD)wstrlen(kDomainNtAuthority);
	if (!Name || *cchName <= requiredAccount || !ReferencedDomainName || *cchReferencedDomainName <= requiredDomain) {
		*cchName = requiredAccount + 1;
		*cchReferencedDomainName = requiredDomain + 1;
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	memcpy(Name, kAccountSystem, (requiredAccount + 1) * sizeof(WCHAR));
	memcpy(ReferencedDomainName, kDomainNtAuthority, (requiredDomain + 1) * sizeof(WCHAR));
	*peUse = SidTypeWellKnownGroup;
	*cchName = requiredAccount;
	*cchReferencedDomainName = requiredDomain;
	return TRUE;
}

BOOL WINAPI LookupPrivilegeValueA(LPCSTR lpSystemName, LPCSTR lpName, PLUID lpLuid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LookupPrivilegeValueA(%s, %s, %p)\n", lpSystemName ? lpSystemName : "(null)", lpName ? lpName : "(null)",
			  lpLuid);
	(void)lpSystemName; // only local lookup supported
	if (!lpName || !lpLuid) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	char *normalized = normalizePrivilegeName(lpName);
	*lpLuid = lookupOrGeneratePrivilegeLuid(normalized);
	free(normalized);
	return TRUE;
}

BOOL WINAPI LookupPrivilegeValueW(LPCWSTR lpSystemName, LPCWSTR lpName, PLUID lpLuid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LookupPrivilegeValueW(%p, %p, %p)\n", lpSystemName, lpName, lpLuid);
	(void)lpSystemName; // only local lookup supported
	if (!lpName || !lpLuid) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	char *ansiName = wideStringToString(lpName, -1);
	char *normalized = normalizePrivilegeName(ansiName);
	*lpLuid = lookupOrGeneratePrivilegeLuid(normalized);
	free(normalized);
	free(ansiName);
	return TRUE;
}

BOOL WINAPI GetUserNameA(LPSTR lpBuffer, LPDWORD pcbBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetUserNameA(%p, %p)\n", lpBuffer, pcbBuffer);
	if (!pcbBuffer) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const char *name = "SYSTEM";
	size_t needed = strlen(name) + 1;
	if (!lpBuffer || *pcbBuffer < needed) {
		*pcbBuffer = (DWORD)needed;
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	memcpy(lpBuffer, name, needed);
	*pcbBuffer = (DWORD)needed;
	return TRUE;
}

BOOL WINAPI GetUserNameW(LPWSTR lpBuffer, LPDWORD pcbBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetUserNameW(%p, %p)\n", lpBuffer, pcbBuffer);
	if (!pcbBuffer) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	size_t needed = wstrlen(kAccountSystem) + 1;
	if (!lpBuffer || *pcbBuffer < needed) {
		*pcbBuffer = (DWORD)needed;
		kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	memcpy(lpBuffer, kAccountSystem, needed * sizeof(WCHAR));
	*pcbBuffer = (DWORD)needed;
	return TRUE;
}
