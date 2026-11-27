#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Executable Executable;

// The name buffer is owned by the identifier when isString is set; release it with ResourceIdentifier_free.
typedef struct {
	bool isString;
	uint32_t id;
	uint16_t *name;
	size_t nameLen;
} ResourceIdentifier;

typedef struct {
	const void *dataEntry;
	const void *data;
	uint32_t size;
	uint16_t language;
} ResourceLocation;

typedef struct {
	uint32_t offsetToData;
	uint32_t size;
	uint32_t codePage;
	uint32_t reserved;
} ImageResourceDataEntry;

ResourceIdentifier ResourceIdentifier_fromID(uint32_t value);
// Takes ownership of name (malloc'd, len code units, not necessarily NUL-terminated).
ResourceIdentifier ResourceIdentifier_fromString(uint16_t *name, size_t len);
void ResourceIdentifier_free(ResourceIdentifier *ident);

bool wibo_resourceEntryBelongsToExecutable(const Executable *exe, const ImageResourceDataEntry *entry);
// Returned identifiers own their name buffer; release with ResourceIdentifier_free.
ResourceIdentifier wibo_resourceIdentifierFromAnsi(const char *id);
ResourceIdentifier wibo_resourceIdentifierFromWide(const uint16_t *id);
