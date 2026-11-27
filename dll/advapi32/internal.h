#pragma once

#include "common.h"
#include "cutil.h"
#include "handles.h"
#include "securitybaseapi.h"

#include <stdlib.h>

#define SECURITY_LOCAL_SYSTEM_RID 18

typedef struct TokenObject {
	ObjectBase base;
	ObjectBase *obj;
	DWORD desiredAccess;
} TokenObject;

static inline void TokenObject_destroy(ObjectBase *base) {
	TokenObject *self = (TokenObject *)base;
	ObjectBase_unref(self->obj);
	free(self);
}

// Takes ownership of the reference to `obj`.
static inline TokenObject *TokenObject_alloc(ObjectBase *obj, DWORD desiredAccess) {
	TokenObject *self = xcalloc(1, sizeof(*self));
	ObjectBase_init(&self->base, ObjectType_Token, TokenObject_destroy);
	self->obj = obj;
	self->desiredAccess = desiredAccess;
	return self;
}

typedef SID_IDENTIFIER_AUTHORITY SidIdentifierAuthority;

typedef struct Sid {
	BYTE Revision;
	BYTE SubAuthorityCount;
	SidIdentifierAuthority IdentifierAuthority;
	DWORD SubAuthority[1];
} Sid;
