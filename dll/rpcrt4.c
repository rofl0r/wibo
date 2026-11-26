#include "rpcrt4.h"

#include "common.h"
#include "context.h"
#include "cutil.h"
#include "heap.h"
#include "modules.h"
#include "types.h"

#define RPC_S_OK 0
#define RPC_S_INVALID_STRING_BINDING 1700
#define RPC_S_INVALID_BINDING 1702
#define RPC_S_SERVER_UNAVAILABLE 1722
#define RPC_S_INVALID_ARG 87
#define RPC_S_OUT_OF_MEMORY 14

typedef VEC(uint16_t) U16Vec;

typedef struct {
	U16Vec objectUuid;
	U16Vec protocolSequence;
	U16Vec networkAddress;
	U16Vec endpoint;
	U16Vec options;
} BindingComponents;

typedef struct {
	BindingComponents components;
	U16Vec bindingString;
	U16Vec serverPrincipalName;
	unsigned long authnLevel;
	unsigned long authnService;
	RPC_AUTH_IDENTITY_HANDLE authIdentity;
	unsigned long authzService;
	bool hasAuthInfo;
	bool hasSecurityQos;
	RPC_SECURITY_QOS securityQos;
	bool serverReachable;
} BindingHandleData;

// Keys are the raw RPC_WSTR / RPC_BINDING_HANDLE pointer bytes; values are owned BindingComponents / BindingHandleData.
static hashmap *g_stringBindings;
static hashmap *g_bindingHandles;

static hashmap *stringBindingMap(void) {
	if (!g_stringBindings) {
		g_stringBindings = hashmap_create();
	}
	return g_stringBindings;
}

static hashmap *bindingHandleMap(void) {
	if (!g_bindingHandles) {
		g_bindingHandles = hashmap_create();
	}
	return g_bindingHandles;
}

static void u16vecSet(U16Vec *dst, const uint16_t *src, size_t len) {
	dst->len = 0;
	vec_reserve(dst, len);
	if (len > 0) {
		memcpy(dst->data, src, len * sizeof(uint16_t));
	}
	dst->len = len;
}

static void u16vecAppend(U16Vec *dst, const U16Vec *src) {
	vec_reserve(dst, src->len);
	if (src->len > 0) {
		memcpy(dst->data + dst->len, src->data, src->len * sizeof(uint16_t));
	}
	dst->len += src->len;
}

static void u16vecAppendAscii(U16Vec *dst, const char *s) {
	size_t n = strlen(s);
	vec_reserve(dst, n);
	for (size_t i = 0; i < n; ++i) {
		dst->data[dst->len++] = (uint8_t)s[i];
	}
}

static void toU16(U16Vec *dst, RPC_WSTR str) {
	if (!str) {
		dst->len = 0;
		return;
	}
	u16vecSet(dst, (const uint16_t *)str, wstr_len((const uint16_t *)str));
}

// Non-ASCII code units are replaced with '?'; this is only used for debug logging.
static char *narrow(const U16Vec *value) {
	char *out = xmalloc(value->len + 1);
	for (size_t i = 0; i < value->len; ++i) {
		uint16_t ch = value->data[i];
		out[i] = ch <= 0x7F ? (char)ch : '?';
	}
	out[value->len] = '\0';
	return out;
}

static void copyComponents(BindingComponents *dst, const BindingComponents *src) {
	u16vecSet(&dst->objectUuid, src->objectUuid.data, src->objectUuid.len);
	u16vecSet(&dst->protocolSequence, src->protocolSequence.data, src->protocolSequence.len);
	u16vecSet(&dst->networkAddress, src->networkAddress.data, src->networkAddress.len);
	u16vecSet(&dst->endpoint, src->endpoint.data, src->endpoint.len);
	u16vecSet(&dst->options, src->options.data, src->options.len);
}

static void freeComponents(BindingComponents *c) {
	vec_free(&c->objectUuid);
	vec_free(&c->protocolSequence);
	vec_free(&c->networkAddress);
	vec_free(&c->endpoint);
	vec_free(&c->options);
}

static void composeString(const BindingComponents *components, U16Vec *result) {
	result->len = 0;
	if (components->objectUuid.len > 0) {
		u16vecAppend(result, &components->objectUuid);
		u16vecAppendAscii(result, "@");
	}
	if (components->protocolSequence.len > 0) {
		u16vecAppend(result, &components->protocolSequence);
	}
	if (components->networkAddress.len > 0) {
		if (components->protocolSequence.len > 0) {
			u16vecAppendAscii(result, ":");
		}
		u16vecAppend(result, &components->networkAddress);
	}
	if (components->endpoint.len > 0) {
		u16vecAppendAscii(result, "[");
		u16vecAppend(result, &components->endpoint);
		u16vecAppendAscii(result, "]");
	}
	if (components->options.len > 0) {
		u16vecAppendAscii(result, "{");
		u16vecAppend(result, &components->options);
		u16vecAppendAscii(result, "}");
	}
}

static BindingHandleData *getBinding(RPC_BINDING_HANDLE handle) {
	void *value = NULL;
	if (!hashmap_find(bindingHandleMap(), &handle, sizeof handle, &value)) {
		return NULL;
	}
	return value;
}

static void freeBinding(BindingHandleData *data) {
	freeComponents(&data->components);
	vec_free(&data->bindingString);
	vec_free(&data->serverPrincipalName);
	wibo_heap_guestFree(data);
}

RPC_STATUS WINAPI RpcStringBindingComposeW(RPC_WSTR objUuid, RPC_WSTR protSeq, RPC_WSTR networkAddr, RPC_WSTR endpoint,
										   RPC_WSTR options, GUEST_PTR *stringBinding) {
	HOST_CONTEXT_GUARD();
	BindingComponents components = {0};
	toU16(&components.objectUuid, objUuid);
	toU16(&components.protocolSequence, protSeq);
	toU16(&components.networkAddress, networkAddr);
	toU16(&components.endpoint, endpoint);
	toU16(&components.options, options);

	U16Vec encoded = {0};
	composeString(&components, &encoded);
	char *encodedNarrow = narrow(&encoded);
	DEBUG_LOG("RpcStringBindingComposeW -> %s\n", encodedNarrow);
	free(encodedNarrow);

	RPC_STATUS status = RPC_S_OK;
	if (stringBinding) {
		size_t length = encoded.len;
		uint16_t *buffer = wibo_heap_guestMalloc((length + 1) * sizeof(uint16_t), false);
		if (!buffer) {
			status = RPC_S_OUT_OF_MEMORY;
			goto done;
		}
		if (length > 0) {
			memcpy(buffer, encoded.data, length * sizeof(uint16_t));
		}
		buffer[length] = 0;
		RPC_WSTR result = (RPC_WSTR)buffer;

		BindingComponents *stored = xmalloc(sizeof *stored);
		*stored = (BindingComponents){0};
		copyComponents(stored, &components);
		void *previous = NULL;
		if (hashmap_remove(stringBindingMap(), &result, sizeof result, &previous)) {
			freeComponents(previous);
			free(previous);
		}
		hashmap_put(stringBindingMap(), &result, sizeof result, stored);
		*stringBinding = toGuestPtr(result);
	}

done:
	vec_free(&encoded);
	freeComponents(&components);
	return status;
}

RPC_STATUS WINAPI RpcBindingFromStringBindingW(RPC_WSTR stringBinding, GUEST_PTR *binding) {
	HOST_CONTEXT_GUARD();
	if (!binding) {
		return RPC_S_INVALID_ARG;
	}
	*binding = GUEST_NULL;
	if (!stringBinding) {
		return RPC_S_INVALID_STRING_BINDING;
	}
	void *storedValue = NULL;
	if (!hashmap_find(stringBindingMap(), &stringBinding, sizeof stringBinding, &storedValue)) {
		return RPC_S_INVALID_STRING_BINDING;
	}
	BindingComponents *storedComponents = storedValue;
	BindingHandleData *handleData = wibo_heap_guestMalloc(sizeof *handleData, true);
	if (!handleData) {
		return RPC_S_OUT_OF_MEMORY;
	}
	copyComponents(&handleData->components, storedComponents);
	composeString(&handleData->components, &handleData->bindingString);
	handleData->serverReachable = false;
	RPC_BINDING_HANDLE handle = handleData;
	hashmap_put(bindingHandleMap(), &handle, sizeof handle, handleData);
	*binding = toGuestPtr(handle);
	DEBUG_LOG("RpcBindingFromStringBindingW(handle=%p)\n", handle);
	return RPC_S_OK;
}

RPC_STATUS WINAPI RpcBindingSetAuthInfoExW(RPC_BINDING_HANDLE binding, RPC_WSTR serverPrincName, ULONG authnLevel,
										   ULONG authnSvc, RPC_AUTH_IDENTITY_HANDLE authIdentity, ULONG authzSvc,
										   RPC_SECURITY_QOS *securityQos) {
	HOST_CONTEXT_GUARD();
	BindingHandleData *data = getBinding(binding);
	if (!data) {
		return RPC_S_INVALID_BINDING;
	}
	toU16(&data->serverPrincipalName, serverPrincName);
	data->authnLevel = authnLevel;
	data->authnService = authnSvc;
	data->authIdentity = authIdentity;
	data->authzService = authzSvc;
	data->hasAuthInfo = true;
	if (securityQos) {
		data->securityQos = *securityQos;
		data->hasSecurityQos = true;
	} else {
		data->hasSecurityQos = false;
	}
	DEBUG_LOG("RpcBindingSetAuthInfoExW(handle=%p, authnSvc=%lu, authnLevel=%lu)\n", binding, authnSvc, authnLevel);
	return RPC_S_OK;
}

RPC_STATUS WINAPI RpcBindingFree(GUEST_PTR *binding) {
	HOST_CONTEXT_GUARD();
	if (!binding) {
		return RPC_S_INVALID_ARG;
	}
	RPC_BINDING_HANDLE handle = fromGuestPtr(*binding);
	if (!handle) {
		return RPC_S_INVALID_BINDING;
	}
	void *value = NULL;
	if (!hashmap_remove(bindingHandleMap(), &handle, sizeof handle, &value)) {
		return RPC_S_INVALID_BINDING;
	}
	freeBinding(value);
	*binding = GUEST_NULL;
	DEBUG_LOG("RpcBindingFree\n");
	return RPC_S_OK;
}

RPC_STATUS WINAPI RpcStringFreeW(GUEST_PTR *string) {
	HOST_CONTEXT_GUARD();
	if (!string) {
		return RPC_S_INVALID_ARG;
	}
	RPC_WSTR value = fromGuestPtr(*string);
	if (!value) {
		return RPC_S_OK;
	}
	void *stored = NULL;
	if (hashmap_remove(stringBindingMap(), &value, sizeof value, &stored)) {
		freeComponents(stored);
		free(stored);
	}
	wibo_heap_guestFree(value);
	*string = GUEST_NULL;
	return RPC_S_OK;
}

CLIENT_CALL_RETURN CDECL_NO_CONV NdrClientCall2(PMIDL_STUB_DESC stubDescriptor, PFORMAT_STRING format, ...) {
	DEBUG_LOG("STUB: NdrClientCall2 stubDescriptor=%p format=%p\n", stubDescriptor, format);
	CLIENT_CALL_RETURN result = {0};
	result.Simple = RPC_S_SERVER_UNAVAILABLE;
	DEBUG_LOG("NdrClientCall2 returning RPC_S_SERVER_UNAVAILABLE\n");
	return result;
}

VOID WINAPI NdrServerCall2(PRPC_MESSAGE message) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: NdrServerCall2 message=%p\n", message);
}

#include "rpcrt4_trampolines.h"

const ModuleStub lib_rpcrt4 = {
	.names = (const char *[]){"rpcrt4", NULL},
	.byName = rpcrt4ThunkByName,
};
