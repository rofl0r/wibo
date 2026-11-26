#include "resources.h"

#include "common.h"
#include "cutil.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "modules.h"

#define RESOURCE_NAME_IS_STRING 0x80000000u
#define RESOURCE_DATA_IS_DIRECTORY 0x80000000u

typedef struct {
	uint32_t characteristics;
	uint32_t timeDateStamp;
	uint16_t majorVersion;
	uint16_t minorVersion;
	uint16_t numberOfNamedEntries;
	uint16_t numberOfIdEntries;
} ImageResourceDirectory;

typedef struct {
	uint32_t name;
	uint32_t offsetToData;
} ImageResourceDirectoryEntry;

static const ImageResourceDirectoryEntry *resourceEntries(const ImageResourceDirectory *dir) {
	return (const ImageResourceDirectoryEntry *)(dir + 1);
}

static bool resourceOffsetInRange(uint32_t offset, size_t needed, uint32_t available) {
	if (available == 0)
		return true;
	if (offset > available)
		return false;
	if (available - offset < needed)
		return false;
	return true;
}

static bool resourceNameEquals(const uint8_t *base, uint32_t nameField, const ResourceIdentifier *ident,
							   uint32_t rsrcSize) {
	if (!(nameField & RESOURCE_NAME_IS_STRING))
		return false;
	uint32_t offset = nameField & ~RESOURCE_NAME_IS_STRING;
	if (!resourceOffsetInRange(offset, sizeof(uint16_t), rsrcSize))
		return false;
	const uint16_t *lengthPtr = (const uint16_t *)(base + offset);
	uint16_t length = *lengthPtr;
	size_t bytesNeeded = sizeof(uint16_t) + (size_t)length * sizeof(uint16_t);
	if (!resourceOffsetInRange(offset, bytesNeeded, rsrcSize))
		return false;
	if (length != ident->nameLen)
		return false;
	const uint16_t *str = lengthPtr + 1;
	for (uint16_t i = 0; i < length; ++i) {
		if (str[i] != ident->name[i])
			return false;
	}
	return true;
}

static const ImageResourceDirectoryEntry *findEntry(const uint8_t *base, const ImageResourceDirectory *dir,
													const ResourceIdentifier *ident, uint32_t rsrcSize) {
	const ImageResourceDirectoryEntry *entries = resourceEntries(dir);
	if (ident->isString) {
		for (uint16_t i = 0; i < dir->numberOfNamedEntries; ++i) {
			const ImageResourceDirectoryEntry *entry = &entries[i];
			if (resourceNameEquals(base, entry->name, ident, rsrcSize))
				return entry;
		}
		return NULL;
	}
	for (uint16_t i = 0; i < dir->numberOfIdEntries; ++i) {
		const ImageResourceDirectoryEntry *entry = &entries[dir->numberOfNamedEntries + i];
		if (!(entry->name & RESOURCE_NAME_IS_STRING) && (entry->name & 0xFFFFu) == (ident->id & 0xFFFFu))
			return entry;
	}
	return NULL;
}

static const ImageResourceDirectory *entryAsDirectory(const uint8_t *base, const ImageResourceDirectoryEntry *entry,
													  uint32_t rsrcSize) {
	if (!(entry->offsetToData & RESOURCE_DATA_IS_DIRECTORY))
		return NULL;
	uint32_t offset = entry->offsetToData & ~RESOURCE_DATA_IS_DIRECTORY;
	if (!resourceOffsetInRange(offset, sizeof(ImageResourceDirectory), rsrcSize))
		return NULL;
	return (const ImageResourceDirectory *)(base + offset);
}

static const ImageResourceDataEntry *entryAsData(const uint8_t *base, const ImageResourceDirectoryEntry *entry,
												 uint32_t rsrcSize) {
	if (entry->offsetToData & RESOURCE_DATA_IS_DIRECTORY)
		return NULL;
	uint32_t offset = entry->offsetToData;
	if (!resourceOffsetInRange(offset, sizeof(ImageResourceDataEntry), rsrcSize))
		return NULL;
	return (const ImageResourceDataEntry *)(base + offset);
}

static uint16_t primaryLang(uint16_t lang) { return lang & 0x3FFu; }

static const ImageResourceDirectoryEntry *selectLanguageEntry(const ImageResourceDirectory *dir,
															  const uint16_t *desired, uint16_t *chosenLang) {
	const ImageResourceDirectoryEntry *entries = resourceEntries(dir);
	uint16_t total = dir->numberOfNamedEntries + dir->numberOfIdEntries;
	const ImageResourceDirectoryEntry *primaryMatch = NULL;
	const ImageResourceDirectoryEntry *neutralMatch = NULL;
	const ImageResourceDirectoryEntry *first = NULL;
	for (uint16_t i = 0; i < total; ++i) {
		const ImageResourceDirectoryEntry *entry = &entries[i];
		if (entry->name & RESOURCE_NAME_IS_STRING)
			continue;
		uint16_t lang = (uint16_t)(entry->name & 0xFFFFu);
		if (!first)
			first = entry;
		if (desired && lang == *desired) {
			*chosenLang = lang;
			return entry;
		}
		if (!primaryMatch && desired && primaryLang(lang) == primaryLang(*desired))
			primaryMatch = entry;
		if (!neutralMatch && lang == 0)
			neutralMatch = entry;
	}
	if (primaryMatch) {
		*chosenLang = (uint16_t)(primaryMatch->name & 0xFFFFu);
		return primaryMatch;
	}
	if (neutralMatch) {
		*chosenLang = 0;
		return neutralMatch;
	}
	if (first) {
		*chosenLang = (uint16_t)(first->name & 0xFFFFu);
		return first;
	}
	return NULL;
}

bool Executable_findResource(const Executable *self, const ResourceIdentifier *type, const ResourceIdentifier *name,
							 const uint16_t *language, ResourceLocation *out) {
	const uint8_t *base = self->rsrcBase;
	if (!base) {
		kernel32_setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return false;
	}
	const ImageResourceDirectory *root = (const ImageResourceDirectory *)base;
	const ImageResourceDirectoryEntry *typeEntry = findEntry(base, root, type, self->rsrcSize);
	if (!typeEntry) {
		kernel32_setLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
		return false;
	}
	const ImageResourceDirectory *nameDir = entryAsDirectory(base, typeEntry, self->rsrcSize);
	if (!nameDir) {
		kernel32_setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return false;
	}
	const ImageResourceDirectoryEntry *nameEntry = findEntry(base, nameDir, name, self->rsrcSize);
	if (!nameEntry) {
		kernel32_setLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
		return false;
	}
	const ImageResourceDirectory *langDir = entryAsDirectory(base, nameEntry, self->rsrcSize);
	if (!langDir) {
		kernel32_setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return false;
	}
	uint16_t chosenLang = language ? *language : 0;
	const ImageResourceDirectoryEntry *langEntry = selectLanguageEntry(langDir, language, &chosenLang);
	if (!langEntry) {
		kernel32_setLastError(ERROR_RESOURCE_LANG_NOT_FOUND);
		return false;
	}
	const ImageResourceDataEntry *dataEntry = entryAsData(base, langEntry, self->rsrcSize);
	if (!dataEntry) {
		kernel32_setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return false;
	}
	out->dataEntry = dataEntry;
	out->data = Executable_fromRVA(self, dataEntry->offsetToData);
	out->size = dataEntry->size;
	out->language = chosenLang;
	return true;
}

bool wibo_resourceEntryBelongsToExecutable(const Executable *exe, const ImageResourceDataEntry *entry) {
	if (!entry || !exe->rsrcBase)
		return false;
	const uint8_t *base = exe->rsrcBase;
	const uint8_t *ptr = (const uint8_t *)entry;
	if (exe->rsrcSize == 0)
		return true;
	return ptr >= base && (ptr + sizeof(*entry)) <= (base + exe->rsrcSize);
}

static bool isIntegerIdentifier(const void *ptr) { return ((uintptr_t)ptr >> 16) == 0; }

ResourceIdentifier ResourceIdentifier_fromID(uint32_t value) {
	ResourceIdentifier ident = {0};
	ident.isString = false;
	ident.id = value;
	return ident;
}

ResourceIdentifier ResourceIdentifier_fromString(uint16_t *name, size_t len) {
	ResourceIdentifier ident = {0};
	ident.isString = true;
	ident.name = name;
	ident.nameLen = len;
	return ident;
}

void ResourceIdentifier_free(ResourceIdentifier *ident) {
	free(ident->name);
	*ident = (ResourceIdentifier){0};
}

ResourceIdentifier wibo_resourceIdentifierFromAnsi(const char *id) {
	if (!id) {
		return ResourceIdentifier_fromID(0);
	}
	if (isIntegerIdentifier(id)) {
		return ResourceIdentifier_fromID((uint32_t)(uintptr_t)id);
	}
	size_t len = strlen(id);
	return ResourceIdentifier_fromString(ascii_to_wstr(id, len), len);
}

ResourceIdentifier wibo_resourceIdentifierFromWide(const uint16_t *id) {
	if (!id) {
		return ResourceIdentifier_fromID(0);
	}
	if (isIntegerIdentifier(id)) {
		return ResourceIdentifier_fromID((uint32_t)(uintptr_t)id);
	}
	size_t len = wstr_len(id);
	uint16_t *copy = xmalloc(len * sizeof *copy);
	memcpy(copy, id, len * sizeof *copy);
	return ResourceIdentifier_fromString(copy, len);
}
