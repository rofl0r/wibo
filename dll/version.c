#include "version.h"

#include "bundle.h"
#include "common.h"
#include "context.h"
#include "cutil.h"
#include "errors.h"
#include "files.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "resources.h"
#include "strutil.h"
#include "types.h"

#include <stdio.h>
#include <string.h>

static const uint32_t kRtVersion = 16;

typedef struct {
	uint16_t totalLength;
	uint16_t valueLength;
	uint16_t type;
	const uint8_t *keyPtr;
	size_t keyLen;
	const uint8_t *valuePtr;
	uint32_t valueBytes;
	const uint8_t *childrenPtr;
	uint32_t childrenBytes;
} VersionBlockView;

static uint16_t readU16(const uint8_t *ptr) { return (uint16_t)(ptr[0] | (ptr[1] << 8)); }

static size_t align4(size_t offset) { return (offset + 3u) & ~(size_t)3u; }

// Returns a malloc'd NUL-terminated copy of the low byte of each of the n UTF-16 units at p.
static char *narrowKey(const uint8_t *p, size_t n) {
	char *s = xmalloc(n + 1);
	for (size_t i = 0; i < n; ++i)
		s[i] = (char)(readU16(p + i * sizeof(uint16_t)) & 0xFF);
	s[n] = '\0';
	return s;
}

static bool parseVersionBlock(const uint8_t *block, size_t available, VersionBlockView *out) {
	if (available < sizeof(uint16_t) * 3) {
		DEBUG_LOG("header too small: available=%zu\n", available);
		return false;
	}

	uint16_t totalLength = readU16(block);
	uint16_t valueLength = readU16(block + sizeof(uint16_t));
	uint16_t type = readU16(block + sizeof(uint16_t) * 2);
	if (totalLength == 0 || totalLength > available) {
		DEBUG_LOG("invalid totalLength=%u available=%zu\n", totalLength, available);
		return false;
	}

	const uint8_t *end = block + totalLength;
	const uint8_t *cursor = block + sizeof(uint16_t) * 3;
	const uint8_t *keyPtr = cursor;
	size_t keyLen = 0;
	while (cursor + sizeof(uint16_t) <= end) {
		uint16_t ch = readU16(cursor);
		cursor += sizeof(uint16_t);
		if (!ch)
			break;
		++keyLen;
	}
	if (wibo_debugEnabled) {
		char *keyStr = narrowKey(keyPtr, keyLen);
		DEBUG_LOG("parsed key fragment=%s\n", keyStr);
		free(keyStr);
	}

	cursor = block + sizeof(uint16_t) * 3 + (keyLen + 1) * sizeof(uint16_t);
	if (cursor > end) {
		DEBUG_LOG("key cursor beyond block: cursor=%zu end=%zu\n", (size_t)(cursor - block), (size_t)(end - block));
		return false;
	}

	cursor = block + align4((size_t)(cursor - block));

	uint32_t valueBytes = 0;
	if (valueLength) {
		valueBytes = type == 1 ? (uint32_t)valueLength * sizeof(uint16_t) : (uint32_t)valueLength;
		if (cursor > end || (size_t)(end - cursor) < valueBytes) {
			DEBUG_LOG("value beyond block: bytes=%u remaining=%zu\n", valueBytes,
					  cursor > end ? (size_t)0 : (size_t)(end - cursor));
			return false;
		}
	}

	const uint8_t *children = block + align4((size_t)((cursor + valueBytes) - block));
	if (children > end)
		children = end;

	out->totalLength = totalLength;
	out->valueLength = valueLength;
	out->type = type;
	out->keyPtr = keyPtr;
	out->keyLen = keyLen;
	out->valuePtr = valueLength ? cursor : NULL;
	out->valueBytes = valueBytes;
	out->childrenPtr = children;
	out->childrenBytes = (uint32_t)(end - children);
	return true;
}

static bool queryVersionBlock(const uint8_t *block, size_t available, const StringVec *segments, size_t depth,
							  const uint8_t **outPtr, uint32_t *outLen, uint16_t *outType) {
	VersionBlockView view;
	if (!parseVersionBlock(block, available, &view))
		return false;

	if (depth == segments->len) {
		if (outPtr)
			*outPtr = view.valueBytes ? view.valuePtr : NULL;
		if (outLen)
			*outLen = view.type == 1 ? view.valueLength : view.valueBytes;
		if (outType)
			*outType = view.type;
		return true;
	}

	const char *segment = segments->data[depth];
	size_t segmentLen = strlen(segment);
	char *targetLower = stringToLower(segment, segmentLen);
	const uint8_t *cursor = view.childrenPtr;
	const uint8_t *end = view.childrenPtr + view.childrenBytes;
	bool found = false;

	while (cursor + 6 <= end) {
		const uint8_t *childStart = cursor;
		VersionBlockView child;
		if (!parseVersionBlock(cursor, (size_t)(end - cursor), &child))
			break;
		if (child.totalLength == 0)
			break;

		char *childKey = narrowKey(child.keyPtr, child.keyLen);
		char *childKeyLower = stringToLower(childKey, child.keyLen);
		free(childKey);
		bool match = child.keyLen == segmentLen && memcmp(childKeyLower, targetLower, segmentLen) == 0;
		free(childKeyLower);
		if (match && queryVersionBlock(childStart, child.totalLength, segments, depth + 1, outPtr, outLen, outType)) {
			found = true;
			break;
		}
		cursor = childStart + align4(child.totalLength);
		if (cursor <= childStart || cursor > end)
			break;
	}
	free(targetLower);
	return found;
}

static void splitSubBlock(const char *subBlock, StringVec *segments) {
	segments->len = 0;
	if (!*subBlock || strcmp(subBlock, "\\") == 0)
		return;

	const char *cursor = subBlock;
	if (*cursor == '\\')
		++cursor;

	while (*cursor) {
		const char *next = strchr(cursor, '\\');
		if (!next)
			next = cursor + strlen(cursor);
		vec_push(segments, xstrndup(cursor, (size_t)(next - cursor)));
		cursor = *next ? next + 1 : next;
	}
}

static void freeSegments(StringVec *segments) {
	for (size_t i = 0; i < segments->len; ++i)
		free(segments->data[i]);
	vec_free(segments);
}

static bool loadVersionResource(const char *fileName, Buf *buffer) {
	if (!fileName) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}

	char *hostPath = files_pathFromWindows(fileName);
	FILE *fp = wibo_openImage(hostPath);
	free(hostPath);
	if (!fp) {
		kernel32_setLastError(ERROR_FILE_NOT_FOUND);
		return false;
	}

	Executable *executable = Executable_new();
	if (!Executable_loadPEFile(executable, fp, false)) {
		fclose(fp);
		Executable_free(executable);
		kernel32_setLastError(ERROR_BAD_EXE_FORMAT);
		return false;
	}

	fclose(fp);

	ResourceIdentifier type = ResourceIdentifier_fromID(kRtVersion);
	ResourceIdentifier name = ResourceIdentifier_fromID(1);
	ResourceLocation loc;
	bool found = Executable_findResource(executable, &type, &name, NULL, &loc);
	if (!found) {
		ResourceIdentifier nameString = wibo_resourceIdentifierFromAnsi("VS_VERSION_INFO");
		found = Executable_findResource(executable, &type, &nameString, NULL, &loc);
		ResourceIdentifier_free(&nameString);
	}
	if (found)
		Buf_append(buffer, loc.data, loc.size);

	Executable_free(executable);
	return found;
}

UINT WINAPI GetFileVersionInfoSizeA(LPCSTR lptstrFilename, LPDWORD lpdwHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileVersionInfoSizeA(%s, %p)\n", lptstrFilename, lpdwHandle);
	if (lpdwHandle)
		*lpdwHandle = 0;

	Buf buffer;
	Buf_init(&buffer);
	UINT size = loadVersionResource(lptstrFilename, &buffer) ? (UINT)buffer.len : 0;
	Buf_free(&buffer);
	return size;
}

UINT WINAPI GetFileVersionInfoA(LPCSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
	HOST_CONTEXT_GUARD();
	(void)dwHandle;
	DEBUG_LOG("GetFileVersionInfoA(%s, %u, %p)\n", lptstrFilename, dwLen, lpData);
	if (!lpData || dwLen == 0) {
		kernel32_setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	Buf buffer;
	Buf_init(&buffer);
	UINT result = 0;
	if (loadVersionResource(lptstrFilename, &buffer)) {
		if (buffer.len > dwLen) {
			kernel32_setLastError(ERROR_INSUFFICIENT_BUFFER);
		} else {
			if (buffer.len)
				memcpy(lpData, buffer.data, buffer.len);
			memset((uint8_t *)lpData + buffer.len, 0, dwLen - buffer.len);
			result = 1;
		}
	}
	Buf_free(&buffer);
	return result;
}

static unsigned int VerQueryValueImpl(const void *pBlock, const char *subBlock, GUEST_PTR *lplpBuffer,
									  unsigned int *puLen) {
	if (!pBlock)
		return 0;

	const uint8_t *base = pBlock;
	uint16_t totalLength = readU16(base);
	if (totalLength < 6)
		return 0;

	StringVec segments = {0};
	splitSubBlock(subBlock, &segments);

	const uint8_t *outPtr = NULL;
	uint32_t outLen = 0;
	uint16_t outType = 0;
	bool found = queryVersionBlock(base, totalLength, &segments, 0, &outPtr, &outLen, &outType);
	freeSegments(&segments);
	if (!found)
		return 0;

	if (outType == 1 && outPtr) {
		char *dest = (char *)outPtr;
		char *narrow = wideStringToString((const uint16_t *)outPtr, (int)outLen);
		memcpy(dest, narrow, outLen + 1);
		if (lplpBuffer)
			*lplpBuffer = toGuestPtr(dest);
		if (puLen)
			*puLen = outLen;
		free(narrow);
		return 1;
	}

	if (lplpBuffer)
		*lplpBuffer = toGuestPtr(outPtr);
	if (puLen)
		*puLen = outLen;
	return 1;
}

UINT WINAPI VerQueryValueA(LPCVOID pBlock, LPCSTR lpSubBlock, GUEST_PTR *lplpBuffer, PUINT puLen) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VerQueryValueA(%p, %s, %p, %p)\n", pBlock, lpSubBlock ? lpSubBlock : "(null)", lplpBuffer, puLen);
	if (!lpSubBlock)
		return 0;
	return VerQueryValueImpl(pBlock, lpSubBlock, lplpBuffer, puLen);
}

UINT WINAPI GetFileVersionInfoSizeW(LPCWSTR lptstrFilename, LPDWORD lpdwHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileVersionInfoSizeW -> ");
	char *narrow = wideStringToString(lptstrFilename, -1);
	UINT size = GetFileVersionInfoSizeA(narrow, lpdwHandle);
	free(narrow);
	return size;
}

UINT WINAPI GetFileVersionInfoW(LPCWSTR lptstrFilename, DWORD dwHandle, DWORD dwLen, LPVOID lpData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetFileVersionInfoW -> ");
	char *narrow = wideStringToString(lptstrFilename, -1);
	UINT result = GetFileVersionInfoA(narrow, dwHandle, dwLen, lpData);
	free(narrow);
	return result;
}

UINT WINAPI VerQueryValueW(LPCVOID pBlock, LPCWSTR lpSubBlock, GUEST_PTR *lplpBuffer, PUINT puLen) {
	HOST_CONTEXT_GUARD();
	if (!lpSubBlock)
		return 0;
	char *narrow = wideStringToString(lpSubBlock, -1);
	DEBUG_LOG("VerQueryValueW(%p, %s, %p, %p)\n", pBlock, narrow, lplpBuffer, puLen);
	UINT result = VerQueryValueImpl(pBlock, narrow, lplpBuffer, puLen);
	free(narrow);
	return result;
}

#include "version_trampolines.h"

const ModuleStub lib_version = {
	.names =
		(const char *[]){
			"version",
			NULL,
		},
	.byName = versionThunkByName,
};
