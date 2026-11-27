#include "securitybaseapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "kernel32/internal.h"
#include "types.h"

#include <string.h>

#define ERROR_REVISION_MISMATCH 1306
#define ERROR_INVALID_ACL 1336
#define ERROR_INVALID_SID 1337
#define ERROR_INVALID_SECURITY_DESCR 1338
#define ERROR_ALLOTTED_SPACE_EXCEEDED 1344

enum { kAceAlignment = 4 };

static const BYTE kNtAuthority[6] = {0, 0, 0, 0, 0, 5};

typedef struct SidAndAttributes {
	Sid *SidPtr;
	DWORD Attributes;
} SidAndAttributes;

typedef struct TokenUserData {
	SidAndAttributes User;
} TokenUserData;

typedef struct TokenStatisticsData {
	LUID tokenId;
	LUID authenticationId;
	LARGE_INTEGER expirationTime;
	DWORD tokenType;
	DWORD impersonationLevel;
	DWORD dynamicCharged;
	DWORD dynamicAvailable;
	DWORD groupCount;
	DWORD privilegeCount;
	LUID modifiedId;
} TokenStatisticsData;

typedef struct TokenPrimaryGroupStub {
	Sid *PrimaryGroup;
} TokenPrimaryGroupStub;

static size_t alignToDword(size_t value) { return (value + (kAceAlignment - 1)) & ~(kAceAlignment - 1); }

static size_t sidLength(const Sid *sid) {
	if (!sid) {
		return 0;
	}
	if (sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
		return 0;
	}
	size_t base = sizeof(Sid) - sizeof(DWORD);
	size_t extra = (size_t)sid->SubAuthorityCount * sizeof(DWORD);
	return base + extra;
}

static bool computeAclUsedSize(const ACL *acl, size_t capacity, size_t *used) {
	if (!acl || capacity < sizeof(ACL)) {
		return false;
	}
	size_t offset = sizeof(ACL);
	const BYTE *base = (const BYTE *)acl;
	for (WORD i = 0; i < acl->AceCount; ++i) {
		if (offset + sizeof(ACE_HEADER) > capacity) {
			return false;
		}
		const ACE_HEADER *header = (const ACE_HEADER *)(base + offset);
		if (header->AceSize < sizeof(ACE_HEADER)) {
			return false;
		}
		size_t aceSize = header->AceSize;
		if (offset + aceSize > capacity) {
			return false;
		}
		offset += aceSize;
	}
	*used = offset;
	return true;
}

static bool writeLocalSystemSid(Sid *sid) {
	if (!sid) {
		return false;
	}
	sid->Revision = 1;
	sid->SubAuthorityCount = 1;
	SidIdentifierAuthority authority;
	memset(&authority, 0, sizeof(authority));
	memcpy(authority.Value, kNtAuthority, sizeof(kNtAuthority));
	sid->IdentifierAuthority = authority;
	sid->SubAuthority[0] = SECURITY_LOCAL_SYSTEM_RID;
	return true;
}

BOOL WINAPI InitializeAcl(PACL pAcl, DWORD nAclLength, DWORD dwAclRevision) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InitializeAcl(%p, %u, %u)\n", pAcl, nAclLength, dwAclRevision);
	if (!pAcl) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (nAclLength < sizeof(ACL) || nAclLength > 0xFFFF || (nAclLength & 0x3) != 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	BYTE revision = (BYTE)dwAclRevision;
	switch (revision) {
	case ACL_REVISION1:
	case ACL_REVISION2:
	case ACL_REVISION3:
	case ACL_REVISION4:
		break;
	default:
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	pAcl->AclRevision = revision;
	pAcl->Sbz1 = 0;
	pAcl->AclSize = (WORD)sizeof(ACL);
	pAcl->AceCount = 0;
	pAcl->Sbz2 = (WORD)nAclLength;
	return TRUE;
}

BOOL WINAPI AddAccessAllowedAce(PACL pAcl, DWORD dwAceRevision, DWORD AccessMask, PSID pSid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("AddAccessAllowedAce(%p, %u, 0x%x, %p)\n", pAcl, dwAceRevision, AccessMask, pSid);
	if (!pAcl || !pSid) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	BYTE revision = (BYTE)dwAceRevision;
	switch (revision) {
	case ACL_REVISION1:
	case ACL_REVISION2:
	case ACL_REVISION3:
	case ACL_REVISION4:
		break;
	default:
		kernel32_setLastError(ERROR_REVISION_MISMATCH);
		return FALSE;
	}
	if (pAcl->AclRevision < revision) {
		kernel32_setLastError(ERROR_REVISION_MISMATCH);
		return FALSE;
	}
	if (pAcl->AceCount == 0xFFFF) {
		kernel32_setLastError(ERROR_ALLOTTED_SPACE_EXCEEDED);
		return FALSE;
	}
	size_t capacity = pAcl->Sbz2 ? pAcl->Sbz2 : pAcl->AclSize;
	if (capacity < sizeof(ACL)) {
		kernel32_setLastError(ERROR_INVALID_ACL);
		return FALSE;
	}
	size_t used = 0;
	if (!computeAclUsedSize(pAcl, capacity, &used)) {
		kernel32_setLastError(ERROR_INVALID_ACL);
		return FALSE;
	}
	if (used > pAcl->AclSize) {
		// Clamp to the computed size to avoid propagating inconsistent data.
		pAcl->AclSize = (WORD)used;
	}
	const Sid *sid = (const Sid *)pSid;
	size_t sidLen = sidLength(sid);
	if (sidLen == 0 || sidLen > capacity) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return FALSE;
	}
	size_t aceSize = sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sidLen;
	aceSize = alignToDword(aceSize);
	if (aceSize > 0xFFFF) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return FALSE;
	}
	if (used + aceSize > capacity) {
		kernel32_setLastError(ERROR_ALLOTTED_SPACE_EXCEEDED);
		return FALSE;
	}
	BYTE *dest = (BYTE *)pAcl + used;
	memset(dest, 0, aceSize);
	ACCESS_ALLOWED_ACE *ace = (ACCESS_ALLOWED_ACE *)dest;
	ace->Header.AceType = ACCESS_ALLOWED_ACE_TYPE;
	ace->Header.AceFlags = 0;
	ace->Header.AceSize = (WORD)aceSize;
	ace->Mask = AccessMask;
	memcpy(&ace->SidStart, sid, sidLen);
	pAcl->AceCount = (WORD)(pAcl->AceCount + 1);
	pAcl->AclSize = (WORD)(used + aceSize);
	return TRUE;
}

BOOL WINAPI FindFirstFreeAce(PACL pAcl, GUEST_PTR *pAce) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindFirstFreeAce(%p, %p)\n", pAcl, pAce);
	if (!pAce) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*pAce = GUEST_NULL;
	if (!pAcl) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	size_t capacity = pAcl->Sbz2 ? pAcl->Sbz2 : pAcl->AclSize;
	size_t used = 0;
	if (!computeAclUsedSize(pAcl, capacity, &used)) {
		kernel32_setLastError(ERROR_INVALID_ACL);
		return FALSE;
	}
	*pAce = toGuestPtr((BYTE *)pAcl + used);
	size_t currentSize = pAcl->AclSize;
	pAcl->AclSize = (WORD)(currentSize > used ? currentSize : used);
	return TRUE;
}

BOOL WINAPI GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, LPBOOL lpbDaclPresent, GUEST_PTR *pDacl,
									  LPBOOL lpbDaclDefaulted) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSecurityDescriptorDacl(%p, %p, %p, %p)\n", pSecurityDescriptor, lpbDaclPresent, pDacl,
			  lpbDaclDefaulted);
	if (!pSecurityDescriptor) {
		kernel32_setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	if (!lpbDaclPresent) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (pSecurityDescriptor->Revision != SECURITY_DESCRIPTOR_REVISION) {
		kernel32_setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	BOOL hasDacl = (pSecurityDescriptor->Control & SE_DACL_PRESENT) ? TRUE : FALSE;
	*lpbDaclPresent = hasDacl;
	if (!hasDacl) {
		if (pDacl) {
			*pDacl = GUEST_NULL;
		}
		if (lpbDaclDefaulted) {
			*lpbDaclDefaulted = FALSE;
		}
		return TRUE;
	}
	if (pDacl) {
		*pDacl = pSecurityDescriptor->Dacl;
	}
	if (lpbDaclDefaulted) {
		*lpbDaclDefaulted = (pSecurityDescriptor->Control & SE_DACL_DEFAULTED) ? TRUE : FALSE;
	}
	return TRUE;
}

PSID_IDENTIFIER_AUTHORITY WINAPI GetSidIdentifierAuthority(PSID pSid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSidIdentifierAuthority(%p)\n", pSid);
	if (!pSid) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return NULL;
	}
	Sid *sid = (Sid *)pSid;
	if (sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return NULL;
	}
	return (PSID_IDENTIFIER_AUTHORITY)&sid->IdentifierAuthority;
}

PUCHAR WINAPI GetSidSubAuthorityCount(PSID pSid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSidSubAuthorityCount(%p)\n", pSid);
	if (!pSid) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return NULL;
	}
	Sid *sid = (Sid *)pSid;
	if (sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return NULL;
	}
	return &sid->SubAuthorityCount;
}

PDWORD WINAPI GetSidSubAuthority(PSID pSid, DWORD nSubAuthority) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSidSubAuthority(%p, %u)\n", pSid, nSubAuthority);
	if (!pSid) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return NULL;
	}
	Sid *sid = (Sid *)pSid;
	if (sid->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES || nSubAuthority >= sid->SubAuthorityCount) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return NULL;
	}
	return &sid->SubAuthority[nSubAuthority];
}

BOOL WINAPI ImpersonateLoggedOnUser(HANDLE hToken) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: ImpersonateLoggedOnUser(%p)\n", hToken);
	(void)hToken;
	return TRUE;
}

BOOL WINAPI DuplicateTokenEx(HANDLE hExistingToken, DWORD dwDesiredAccess, void *lpTokenAttributes,
							 DWORD ImpersonationLevel, DWORD TokenType, PHANDLE phNewToken) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DuplicateTokenEx(%p, 0x%x, %p, %u, %u, %p)\n", hExistingToken, dwDesiredAccess, lpTokenAttributes,
			  ImpersonationLevel, TokenType, phNewToken);
	(void)lpTokenAttributes;
	(void)ImpersonationLevel;
	(void)TokenType;
	if (!phNewToken) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	TokenObject *existing = (TokenObject *)Handles_getAs(wibo_handles(), hExistingToken, ObjectType_Token, NULL);
	if (!existing) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	ObjectBase_ref(existing->obj);
	DWORD access = dwDesiredAccess == 0 ? existing->desiredAccess : dwDesiredAccess;
	TokenObject *newToken = TokenObject_alloc(existing->obj, access);
	ObjectBase_unref(&existing->base);
	*phNewToken = Handles_alloc(wibo_handles(), &newToken->base, 0, 0);
	return TRUE;
}

BOOL WINAPI CopySid(DWORD nDestinationSidLength, PSID pDestinationSid, PSID pSourceSid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CopySid(%u, %p, %p)\n", nDestinationSidLength, pDestinationSid, pSourceSid);
	if (!pDestinationSid || !pSourceSid) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const Sid *source = (const Sid *)pSourceSid;
	size_t required = sidLength(source);
	if (required == 0 || required > nDestinationSidLength) {
		kernel32_setLastError(ERROR_ALLOTTED_SPACE_EXCEEDED);
		return FALSE;
	}
	memcpy(pDestinationSid, pSourceSid, required);
	return TRUE;
}

BOOL WINAPI InitializeSid(PSID sid, PSID_IDENTIFIER_AUTHORITY pIdentifierAuthority, BYTE nSubAuthorityCount) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InitializeSid(%p, %p, %u)\n", sid, pIdentifierAuthority, nSubAuthorityCount);
	if (!sid || !pIdentifierAuthority) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (nSubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return FALSE;
	}
	Sid *sidStruct = (Sid *)sid;
	sidStruct->Revision = SID_REVISION;
	sidStruct->SubAuthorityCount = nSubAuthorityCount;
	sidStruct->IdentifierAuthority = *pIdentifierAuthority;
	if (nSubAuthorityCount > 0) {
		memset(sidStruct->SubAuthority, 0, sizeof(DWORD) * nSubAuthorityCount);
	}
	return TRUE;
}

BOOL WINAPI EqualSid(PSID pSid1, PSID pSid2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EqualSid(%p, %p)\n", pSid1, pSid2);
	if (!pSid1 || !pSid2) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return FALSE;
	}
	const Sid *sid1 = (const Sid *)pSid1;
	const Sid *sid2 = (const Sid *)pSid2;
	if (sid1->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES || sid2->SubAuthorityCount > SID_MAX_SUB_AUTHORITIES) {
		kernel32_setLastError(ERROR_INVALID_SID);
		return FALSE;
	}
	bool equal = sid1->Revision == sid2->Revision &&
				 memcmp(&sid1->IdentifierAuthority, &sid2->IdentifierAuthority, sizeof(SidIdentifierAuthority)) == 0 &&
				 sid1->SubAuthorityCount == sid2->SubAuthorityCount;
	if (equal && sid1->SubAuthorityCount > 0) {
		equal = memcmp(sid1->SubAuthority, sid2->SubAuthority, sizeof(DWORD) * sid1->SubAuthorityCount) == 0;
	}
	return equal ? TRUE : FALSE;
}

BOOL WINAPI SetKernelObjectSecurity(HANDLE Handle, SECURITY_INFORMATION SecurityInformation,
									PSECURITY_DESCRIPTOR SecurityDescriptor) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetKernelObjectSecurity(%p, 0x%x, %p)\n", Handle, SecurityInformation, SecurityDescriptor);
	(void)SecurityInformation;
	if (!SecurityDescriptor) {
		kernel32_setLastError(ERROR_INVALID_SECURITY_DESCR);
		return FALSE;
	}
	ObjectBase *obj = Handles_get(wibo_handles(), Handle, NULL);
	if (!obj) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	ObjectBase_unref(obj);
	return TRUE;
}

BOOL WINAPI InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR pSecurityDescriptor, DWORD dwRevision) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InitializeSecurityDescriptor(%p, %u)\n", pSecurityDescriptor, dwRevision);
	if (!pSecurityDescriptor || dwRevision != SECURITY_DESCRIPTOR_REVISION) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	pSecurityDescriptor->Revision = (BYTE)dwRevision;
	pSecurityDescriptor->Sbz1 = 0;
	pSecurityDescriptor->Control = 0;
	pSecurityDescriptor->Owner = GUEST_NULL;
	pSecurityDescriptor->Group = GUEST_NULL;
	pSecurityDescriptor->Sacl = GUEST_NULL;
	pSecurityDescriptor->Dacl = GUEST_NULL;
	return TRUE;
}

BOOL WINAPI SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, BOOL bDaclPresent, PACL pDacl,
									  BOOL bDaclDefaulted) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetSecurityDescriptorDacl(%p, %u, %p, %u)\n", pSecurityDescriptor, bDaclPresent, pDacl, bDaclDefaulted);
	if (!pSecurityDescriptor || pSecurityDescriptor->Revision != SECURITY_DESCRIPTOR_REVISION) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	WORD control = (WORD)(pSecurityDescriptor->Control & ~(SE_DACL_PRESENT | SE_DACL_DEFAULTED));
	if (bDaclPresent) {
		control = (WORD)(control | SE_DACL_PRESENT);
		if (bDaclDefaulted) {
			control = (WORD)(control | SE_DACL_DEFAULTED);
		}
		pSecurityDescriptor->Dacl = toGuestPtr(pDacl);
	} else {
		pSecurityDescriptor->Dacl = GUEST_NULL;
	}
	pSecurityDescriptor->Control = control;
	return TRUE;
}

BOOL WINAPI GetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass,
								LPVOID TokenInformation, DWORD TokenInformationLength, LPDWORD ReturnLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetTokenInformation(%p, %u, %p, %u, %p)\n", TokenHandle, TokenInformationClass, TokenInformation,
			  TokenInformationLength, ReturnLength);
	if (!ReturnLength) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ObjectBase *token = Handles_getAs(wibo_handles(), TokenHandle, ObjectType_Token, NULL);
	if (!token) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	ObjectBase_unref(token);
	*ReturnLength = 0;
	if (TokenInformationClass == TokenUser) {
		size_t sidSize = sizeof(Sid);
		size_t tokenUserSize = sizeof(TokenUserData);
		DWORD required = (DWORD)(tokenUserSize + sidSize);
		*ReturnLength = required;
		if (!TokenInformation || TokenInformationLength < required) {
			kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		TokenUserData *tokenUser = (TokenUserData *)TokenInformation;
		Sid *sid = (Sid *)((BYTE *)TokenInformation + tokenUserSize);
		if (!writeLocalSystemSid(sid)) {
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		tokenUser->User.SidPtr = sid;
		tokenUser->User.Attributes = 0;
		return TRUE;
	}
	if (TokenInformationClass == TokenStatistics) {
		DWORD required = sizeof(TokenStatisticsData);
		*ReturnLength = required;
		if (!TokenInformation || TokenInformationLength < required) {
			kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		TokenStatisticsData *stats = (TokenStatisticsData *)TokenInformation;
		memset(stats, 0, sizeof(*stats));
		stats->tokenType = 1;		   // TokenPrimary
		stats->impersonationLevel = 0; // SecurityAnonymous
		stats->tokenId.LowPart = 1;
		stats->authenticationId.LowPart = 1;
		stats->modifiedId.LowPart = 1;
		return TRUE;
	}
	if (TokenInformationClass == TokenElevation) {
		DWORD required = sizeof(DWORD);
		*ReturnLength = required;
		if (!TokenInformation || TokenInformationLength < required) {
			kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		*(DWORD *)TokenInformation = 0; // not elevated
		return TRUE;
	}
	if (TokenInformationClass == TokenPrimaryGroup) {
		DWORD required = (DWORD)(sizeof(TokenPrimaryGroupStub) + sizeof(Sid));
		*ReturnLength = required;
		if (!TokenInformation || TokenInformationLength < required) {
			kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
			return FALSE;
		}
		TokenPrimaryGroupStub *groupInfo = (TokenPrimaryGroupStub *)TokenInformation;
		Sid *sid = (Sid *)((BYTE *)TokenInformation + sizeof(TokenPrimaryGroupStub));
		if (!writeLocalSystemSid(sid)) {
			kernel32_setLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		groupInfo->PrimaryGroup = sid;
		return TRUE;
	}
	kernel32_setLastError(ERROR_NOT_SUPPORTED);
	return FALSE;
}

BOOL WINAPI AdjustTokenPrivileges(HANDLE TokenHandle, BOOL DisableAllPrivileges, PTOKEN_PRIVILEGES NewState,
								  DWORD BufferLength, PTOKEN_PRIVILEGES PreviousState, LPDWORD ReturnLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("AdjustTokenPrivileges(%p, %u, %p, %u, %p, %p)\n", TokenHandle, DisableAllPrivileges, NewState,
			  BufferLength, PreviousState, ReturnLength);
	(void)TokenHandle;
	(void)DisableAllPrivileges;
	(void)NewState;
	(void)BufferLength;
	(void)PreviousState;
	(void)ReturnLength;
	return TRUE;
}

BOOL WINAPI SetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass,
								LPVOID TokenInformation, DWORD TokenInformationLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: SetTokenInformation(%p, %u, %p, %u)\n", TokenHandle, TokenInformationClass, TokenInformation,
			  TokenInformationLength);
	(void)TokenInformationClass;
	(void)TokenInformation;
	(void)TokenInformationLength;
	ObjectBase *token = Handles_getAs(wibo_handles(), TokenHandle, ObjectType_Token, NULL);
	if (!token) {
		kernel32_setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	ObjectBase_unref(token);
	return TRUE;
}
