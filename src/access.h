#pragma once

#include "common.h"

#include <stdbool.h>
#include <stdint.h>

// Maps the Win32 generic access bits (GENERIC_* constants) to the
// object-specific rights supplied in the mapping table.
typedef struct {
	uint32_t genericRead;
	uint32_t genericWrite;
	uint32_t genericExecute;
	uint32_t genericAll;
} GenericMapping;

typedef struct {
	uint32_t requestedMask; // mask after generic expansion + implicit bits
	uint32_t grantedMask;	// requested & supported
	uint32_t deniedMask;	// requested & ~supported
} NormalizedAccess;

uint32_t wibo_access_mapGenericMask(uint32_t desiredMask, const GenericMapping *mapping);
NormalizedAccess wibo_access_normalizeDesiredAccess(uint32_t desiredMask, const GenericMapping *mapping,
													uint32_t supportedMask, uint32_t alwaysGrantMask,
													uint32_t defaultMask);

static inline bool wibo_access_containsAny(uint32_t mask, uint32_t rights) { return (mask & rights) != 0; }
static inline bool wibo_access_containsAll(uint32_t mask, uint32_t rights) { return (mask & rights) == rights; }

extern const GenericMapping wibo_access_kFileGenericMapping;
extern const GenericMapping wibo_access_kDirectoryGenericMapping;

#define kFileSpecificRightsMask                                                                                        \
	(FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_EXECUTE | FILE_READ_EA | FILE_WRITE_EA |               \
	 FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES)
#define kDirectorySpecificRightsMask                                                                                   \
	(FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_TRAVERSE | FILE_DELETE_CHILD | FILE_READ_EA |   \
	 FILE_WRITE_EA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES)
