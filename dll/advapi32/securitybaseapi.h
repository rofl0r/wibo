#pragma once

#include "types.h"

typedef struct ACL {
	BYTE AclRevision;
	BYTE Sbz1;
	WORD AclSize;
	WORD AceCount;
	WORD Sbz2;
} ACL;

typedef struct ACE_HEADER {
	BYTE AceType;
	BYTE AceFlags;
	WORD AceSize;
} ACE_HEADER;

typedef struct ACCESS_ALLOWED_ACE {
	ACE_HEADER Header;
	DWORD Mask;
	DWORD SidStart;
} ACCESS_ALLOWED_ACE;

typedef struct SID_IDENTIFIER_AUTHORITY {
	BYTE Value[6];
} SID_IDENTIFIER_AUTHORITY;

typedef struct SECURITY_DESCRIPTOR {
	BYTE Revision;
	BYTE Sbz1;
	WORD Control;
	GUEST_PTR Owner;
	GUEST_PTR Group;
	GUEST_PTR Sacl;
	GUEST_PTR Dacl;
} SECURITY_DESCRIPTOR;

typedef SECURITY_DESCRIPTOR *PSECURITY_DESCRIPTOR;
typedef ACL *PACL;
typedef SID_IDENTIFIER_AUTHORITY *PSID_IDENTIFIER_AUTHORITY;
typedef DWORD SECURITY_INFORMATION;

#define SECURITY_DESCRIPTOR_REVISION ((DWORD)1)
#define SE_DACL_PRESENT ((WORD)0x0004)
#define SE_DACL_DEFAULTED ((WORD)0x0008)

#define ACL_REVISION1 ((BYTE)1)
#define ACL_REVISION2 ((BYTE)2)
#define ACL_REVISION3 ((BYTE)3)
#define ACL_REVISION4 ((BYTE)4)
#define ACL_REVISION ACL_REVISION2
#define ACL_REVISION_DS ACL_REVISION4
#define ACCESS_ALLOWED_ACE_TYPE ((BYTE)0x00)
#define SID_MAX_SUB_AUTHORITIES ((BYTE)15)
#define SID_REVISION ((BYTE)1)

typedef struct TOKEN_PRIVILEGES TOKEN_PRIVILEGES;
typedef TOKEN_PRIVILEGES *PTOKEN_PRIVILEGES;

typedef enum TOKEN_INFORMATION_CLASS {
	TokenUser = 1,
	TokenGroups,
	TokenPrivileges,
	TokenOwner,
	TokenPrimaryGroup,
	TokenDefaultDacl,
	TokenSource,
	TokenType,
	TokenImpersonationLevel,
	TokenStatistics,
	TokenRestrictedSids,
	TokenSessionId,
	TokenGroupsAndPrivileges,
	TokenSessionReference,
	TokenSandBoxInert,
	TokenAuditPolicy,
	TokenOrigin,
	TokenElevationType,
	TokenLinkedToken,
	TokenElevation = 20,
} TOKEN_INFORMATION_CLASS;

BOOL WINAPI InitializeAcl(PACL pAcl, DWORD nAclLength, DWORD dwAclRevision);
BOOL WINAPI AddAccessAllowedAce(PACL pAcl, DWORD dwAceRevision, DWORD AccessMask, PSID pSid);
BOOL WINAPI FindFirstFreeAce(PACL pAcl, GUEST_PTR *pAce);
PSID_IDENTIFIER_AUTHORITY WINAPI GetSidIdentifierAuthority(PSID pSid);
PUCHAR WINAPI GetSidSubAuthorityCount(PSID pSid);
PDWORD WINAPI GetSidSubAuthority(PSID pSid, DWORD nSubAuthority);
BOOL WINAPI ImpersonateLoggedOnUser(HANDLE hToken);
BOOL WINAPI DuplicateTokenEx(HANDLE hExistingToken, DWORD dwDesiredAccess, void *lpTokenAttributes,
							 DWORD ImpersonationLevel, DWORD TokenType, PHANDLE phNewToken);
BOOL WINAPI CopySid(DWORD nDestinationSidLength, PSID pDestinationSid, PSID pSourceSid);
BOOL WINAPI InitializeSid(PSID sid, PSID_IDENTIFIER_AUTHORITY pIdentifierAuthority, BYTE nSubAuthorityCount);
BOOL WINAPI EqualSid(PSID pSid1, PSID pSid2);
BOOL WINAPI GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, LPBOOL lpbDaclPresent, GUEST_PTR *pDacl,
									  LPBOOL lpbDaclDefaulted);
BOOL WINAPI SetKernelObjectSecurity(HANDLE Handle, SECURITY_INFORMATION SecurityInformation,
									PSECURITY_DESCRIPTOR SecurityDescriptor);
BOOL WINAPI InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR pSecurityDescriptor, DWORD dwRevision);
BOOL WINAPI SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, BOOL bDaclPresent, PACL pDacl,
									  BOOL bDaclDefaulted);
BOOL WINAPI GetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass,
								LPVOID TokenInformation, DWORD TokenInformationLength, LPDWORD ReturnLength);
BOOL WINAPI AdjustTokenPrivileges(HANDLE TokenHandle, BOOL DisableAllPrivileges, PTOKEN_PRIVILEGES NewState,
								  DWORD BufferLength, PTOKEN_PRIVILEGES PreviousState, LPDWORD ReturnLength);
BOOL WINAPI SetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass,
								LPVOID TokenInformation, DWORD TokenInformationLength);
