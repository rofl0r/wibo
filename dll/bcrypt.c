#include "bcrypt.h"

#include "common.h"
#include "context.h"
#include "cutil.h"
#include "errors.h"
#include "modules.h"

#include <errno.h>
#include <string.h>
#include <sys/random.h>

#define BCRYPT_RNG_USE_ENTROPY_IN_BUFFER 0x00000001
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002

static bool fillWithSystemRandom(PUCHAR buffer, size_t length) {
#ifdef __APPLE__
	arc4random_buf(buffer, length);
#else
	while (length > 0) {
		ssize_t written = getrandom(buffer, length, 0);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (written == 0)
			continue;
		buffer += written;
		length -= (size_t)written;
	}
#endif
	return true;
}

NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("BCryptGenRandom(%p, %p, %lu, %lu)\n", hAlgorithm, pbBuffer, cbBuffer, dwFlags);
	if (pbBuffer == NULL && cbBuffer != 0)
		return STATUS_INVALID_HANDLE;

	if (hAlgorithm != NULL)
		return STATUS_NOT_IMPLEMENTED;

	if ((dwFlags & BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
		return STATUS_INVALID_HANDLE;

	ULONG allowedFlags = BCRYPT_RNG_USE_ENTROPY_IN_BUFFER | BCRYPT_USE_SYSTEM_PREFERRED_RNG;
	if ((dwFlags & ~allowedFlags) != 0)
		return STATUS_INVALID_PARAMETER;

	if (cbBuffer == 0)
		return STATUS_SUCCESS;

	unsigned char *entropy = NULL;
	if ((dwFlags & BCRYPT_RNG_USE_ENTROPY_IN_BUFFER) && pbBuffer != NULL) {
		entropy = xmalloc(cbBuffer);
		memcpy(entropy, pbBuffer, cbBuffer);
	}

	if (!fillWithSystemRandom(pbBuffer, cbBuffer)) {
		free(entropy);
		return STATUS_UNEXPECTED_IO_ERROR;
	}

	if (entropy) {
		for (size_t i = 0; i < cbBuffer; ++i)
			pbBuffer[i] ^= entropy[i];
		free(entropy);
	}
	return STATUS_SUCCESS;
}

BOOL WINAPI ProcessPrng(PBYTE pbData, SIZE_T cbData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ProcessPrng(%p, %lu)\n", pbData, cbData);
	if (pbData == NULL && cbData != 0)
		return FALSE;
	return fillWithSystemRandom(pbData, cbData);
}

#include "bcrypt_trampolines.h"

const ModuleStub lib_bcrypt = {
	.names =
		(const char *[]){
			"bcrypt",
			"bcryptprimitives",
			NULL,
		},
	.byName = bcryptThunkByName,
};
