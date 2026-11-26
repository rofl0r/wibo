#include "common.h"
#include "errors.h"
#include "heap.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "unwind.h"
#include "types.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define IMAGE_FILE_DLL 0x2000

typedef struct {
	uint8_t magic[4]; // "PE\0\0"
	uint16_t machine;
	uint16_t numberOfSections;
	uint32_t timeDateStamp;
	uint32_t pointerToSymbolTable;
	uint32_t numberOfSymbols;
	uint16_t sizeOfOptionalHeader;
	uint16_t characteristics;
} PEHeader;

typedef struct {
	uint32_t virtualAddress;
	uint32_t size;
} PEImageDataDirectory;

typedef struct {
	uint16_t magic; // 0x10B for PE32
	uint8_t majorLinkerVersion;
	uint8_t minorLinkerVersion;
	uint32_t sizeOfCode;
	uint32_t sizeOfInitializedData;
	uint32_t sizeOfUninitializedData;
	uint32_t addressOfEntryPoint;
	uint32_t baseOfCode;
	uint32_t baseOfData;
	uint32_t imageBase;
	uint32_t sectionAlignment;
	uint32_t fileAlignment;
	uint16_t majorOperatingSystemVersion;
	uint16_t minorOperatingSystemVersion;
	uint16_t majorImageVersion;
	uint16_t minorImageVersion;
	uint16_t majorSubsystemVersion;
	uint16_t minorSubsystemVersion;
	uint32_t win32VersionValue;
	uint32_t sizeOfImage;
	uint32_t sizeOfHeaders;
	uint32_t checkSum;
	uint16_t subsystem;
	uint16_t dllCharacteristics;
	uint32_t sizeOfStackReserve;
	uint32_t sizeOfStackCommit;
	uint32_t sizeOfHeapReserve;
	uint32_t sizeOfHeapCommit;
	uint32_t loaderFlags;
	uint32_t numberOfRvaAndSizes;
	PEImageDataDirectory exportTable;
	PEImageDataDirectory importTable;	// *
	PEImageDataDirectory resourceTable; // *
	PEImageDataDirectory exceptionTable;
	PEImageDataDirectory certificateTable;
	PEImageDataDirectory baseRelocationTable; // *
	PEImageDataDirectory debug;				  // *
	PEImageDataDirectory architecture;
	PEImageDataDirectory globalPtr;
	PEImageDataDirectory tlsTable;
	PEImageDataDirectory loadConfigTable;
	PEImageDataDirectory boundImport;
	PEImageDataDirectory iat;
	PEImageDataDirectory delayImportDescriptor;
	PEImageDataDirectory clrRuntimeHeader;
	PEImageDataDirectory reserved;
} PE32Header;

// IMAGE_OPTIONAL_HEADER64 layout (PE32+, magic 0x20B)
typedef struct {
	uint16_t magic;
	uint8_t majorLinkerVersion;
	uint8_t minorLinkerVersion;
	uint32_t sizeOfCode;
	uint32_t sizeOfInitializedData;
	uint32_t sizeOfUninitializedData;
	uint32_t addressOfEntryPoint;
	uint32_t baseOfCode;
	uint64_t imageBase;
	uint32_t sectionAlignment;
	uint32_t fileAlignment;
	uint16_t majorOperatingSystemVersion;
	uint16_t minorOperatingSystemVersion;
	uint16_t majorImageVersion;
	uint16_t minorImageVersion;
	uint16_t majorSubsystemVersion;
	uint16_t minorSubsystemVersion;
	uint32_t win32VersionValue;
	uint32_t sizeOfImage;
	uint32_t sizeOfHeaders;
	uint32_t checkSum;
	uint16_t subsystem;
	uint16_t dllCharacteristics;
	uint64_t sizeOfStackReserve;
	uint64_t sizeOfStackCommit;
	uint64_t sizeOfHeapReserve;
	uint64_t sizeOfHeapCommit;
	uint32_t loaderFlags;
	uint32_t numberOfRvaAndSizes;
	PEImageDataDirectory exportTable;
	PEImageDataDirectory importTable;
	PEImageDataDirectory resourceTable;
	PEImageDataDirectory exceptionTable;
	PEImageDataDirectory certificateTable;
	PEImageDataDirectory baseRelocationTable;
	PEImageDataDirectory debug;
	PEImageDataDirectory architecture;
	PEImageDataDirectory globalPtr;
	PEImageDataDirectory tlsTable;
	PEImageDataDirectory loadConfigTable;
	PEImageDataDirectory boundImport;
	PEImageDataDirectory iat;
	PEImageDataDirectory delayImportDescriptor;
	PEImageDataDirectory clrRuntimeHeader;
	PEImageDataDirectory reserved;
} PE32PlusHeader;

// Format-independent subset of the optional header, filled from either PE32 or PE32+.
typedef struct {
	bool is64;
	uint64_t imageBase;
	uint32_t addressOfEntryPoint;
	uint32_t sectionAlignment;
	uint32_t fileAlignment;
	uint32_t sizeOfImage;
	uint32_t sizeOfHeaders;
	PEImageDataDirectory exportTable;
	PEImageDataDirectory importTable;
	PEImageDataDirectory baseRelocationTable;
	PEImageDataDirectory delayImportDescriptor;
	PEImageDataDirectory tlsTable;
	PEImageDataDirectory exceptionTable;
} PEOptionalHeader;

typedef struct {
	char name[8];
	uint32_t virtualSize;
	uint32_t virtualAddress;
	uint32_t sizeOfRawData;
	uint32_t pointerToRawData;
	uint32_t pointerToRelocations;
	uint32_t pointerToLinenumbers;
	uint16_t numberOfRelocations;
	uint16_t numberOfLinenumbers;
	uint32_t characteristics;
} PESectionHeader;

typedef struct {
	uint32_t importLookupTable;
	uint32_t timeDateStamp;
	uint32_t forwarderChain;
	uint32_t name;
	uint32_t importAddressTable;
} PEImportDirectoryEntry;

typedef struct {
	uint16_t hint;
	char name[1]; // variable length
} PEHintNameTableEntry;

typedef struct {
	uint32_t attributes;
	uint32_t name;
	uint32_t moduleHandle;
	uint32_t importAddressTable;
	uint32_t importNameTable;
	uint32_t boundImportAddressTable;
	uint32_t unloadInformationTable;
	uint32_t timeStamp;
} PEDelayImportDescriptor;

typedef struct {
	uint32_t virtualAddress;
	uint32_t sizeOfBlock;
} PEBaseRelocationBlock;

#define IMAGE_REL_BASED_ABSOLUTE 0
#define IMAGE_REL_BASED_HIGHLOW 3
#define IMAGE_REL_BASED_DIR64 10

#define IMAGE_FILE_MACHINE_I386 0x14C
#define IMAGE_FILE_MACHINE_AMD64 0x8664
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC 0x10B
#define IMAGE_NT_OPTIONAL_HDR64_MAGIC 0x20B

#define IMAGE_SCN_MEM_EXECUTE 0x20000000
#define IMAGE_SCN_MEM_READ 0x40000000
#define IMAGE_SCN_MEM_WRITE 0x80000000
#define IMAGE_SCN_MEM_NOT_CACHED 0x04000000

static uintptr_t alignDown(uintptr_t value, size_t alignment) {
	if (alignment == 0) {
		return value;
	}
	return value - (value % alignment);
}

static uintptr_t alignUp(uintptr_t value, size_t alignment) {
	if (alignment == 0) {
		return value;
	}
	const uintptr_t remainder = value % alignment;
	if (remainder == 0) {
		return value;
	}
	if (value > UINTPTR_MAX - (alignment - remainder)) {
		return UINTPTR_MAX;
	}
	return value + (alignment - remainder);
}

static DWORD sectionProtectFromCharacteristics(uint32_t characteristics) {
	const bool executable = (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
	bool readable = (characteristics & IMAGE_SCN_MEM_READ) != 0;
	const bool writable = (characteristics & IMAGE_SCN_MEM_WRITE) != 0;
	if (!readable && !writable && !executable) {
		readable = true;
	}
	DWORD protect = PAGE_NOACCESS;
	if (executable) {
		if (writable) {
			protect = PAGE_EXECUTE_READWRITE;
		} else if (readable) {
			protect = PAGE_EXECUTE_READ;
		} else {
			protect = PAGE_EXECUTE;
		}
	} else {
		if (writable) {
			protect = PAGE_READWRITE;
		} else if (readable) {
			protect = PAGE_READONLY;
		}
	}
	if ((characteristics & IMAGE_SCN_MEM_NOT_CACHED) != 0) {
		protect |= PAGE_NOCACHE;
	}
	return protect;
}

Executable *Executable_new(void) { return xcalloc(1, sizeof(Executable)); }

void Executable_free(Executable *self) {
	if (!self) {
		return;
	}
	if (self->imageBase) {
		wibo_heap_virtualFree(self->imageBase, 0, MEM_RELEASE);
		self->imageBase = NULL;
	}
	wibo_unregisterFunctionTable(self->functionTable);
	self->functionTable = NULL;
	vec_free(&self->sections);
	free(self);
}

// Reads from either a FILE* or an in-memory image; hasSize is false when the file size is unknown.
typedef struct {
	bool isFile;
	FILE *file;
	const uint8_t *bytes;
	size_t byteCount;
	bool hasSize;
	uint64_t size;
} PeInputView;

static bool computeFileSize(FILE *file, uint64_t *outSize) {
	if (!file) {
		return false;
	}
	int fd = fileno(file);
	if (fd < 0) {
		return false;
	}
	struct stat st;
	if (fstat(fd, &st) != 0) {
		return false;
	}
	if (st.st_size < 0) {
		return false;
	}
	*outSize = (uint64_t)st.st_size;
	return true;
}

static PeInputView peInputViewFromFile(FILE *file) {
	PeInputView view;
	memset(&view, 0, sizeof(view));
	view.isFile = true;
	view.file = file;
	view.hasSize = computeFileSize(file, &view.size);
	return view;
}

static PeInputView peInputViewFromBytes(const uint8_t *bytes, size_t byteCount) {
	PeInputView view;
	memset(&view, 0, sizeof(view));
	view.bytes = bytes;
	view.byteCount = byteCount;
	view.hasSize = true;
	view.size = byteCount;
	return view;
}

static bool peViewReadFile(const PeInputView *view, uint64_t offset, void *dest, size_t size) {
	if (!view->file) {
		return false;
	}
	const uint64_t maxOffset = sizeof(off_t) >= 8 ? (uint64_t)INT64_MAX : (uint64_t)INT32_MAX;
	if (offset > maxOffset) {
		return false;
	}
	if (view->hasSize) {
		if (offset > view->size) {
			return false;
		}
		uint64_t remaining = view->size - offset;
		if (remaining < size) {
			return false;
		}
	}
	if (fseeko(view->file, (off_t)offset, SEEK_SET) != 0) {
		return false;
	}
	unsigned char *buffer = dest;
	size_t totalRead = 0;
	while (totalRead < size) {
		size_t chunk = fread(buffer + totalRead, 1, size - totalRead, view->file);
		if (chunk == 0) {
			if (feof(view->file)) {
				break;
			}
			return false;
		}
		totalRead += chunk;
	}
	return totalRead == size;
}

static bool peViewReadBytes(const PeInputView *view, uint64_t offset, void *dest, size_t size) {
	if (offset > view->byteCount) {
		return false;
	}
	size_t start = (size_t)offset;
	if (view->byteCount - start < size) {
		return false;
	}
	memcpy(dest, view->bytes + start, size);
	return true;
}

static bool peViewRead(const PeInputView *view, uint64_t offset, void *dest, size_t size) {
	if (size == 0) {
		return true;
	}
	return view->isFile ? peViewReadFile(view, offset, dest, size) : peViewReadBytes(view, offset, dest, size);
}

static void resetExecutableState(Executable *executable) {
	if (executable->imageBase) {
		wibo_heap_virtualFree(executable->imageBase, 0, MEM_RELEASE);
	}
	executable->imageBase = NULL;
	executable->imageSize = 0;
	executable->entryPoint = NULL;
	executable->rsrcBase = NULL;
	executable->rsrcSize = 0;
	executable->preferredImageBase = 0;
	executable->relocationDelta = 0;
	executable->is64Bit = false;
	executable->exportDirectoryRVA = 0;
	executable->exportDirectorySize = 0;
	executable->relocationDirectoryRVA = 0;
	executable->relocationDirectorySize = 0;
	executable->importDirectoryRVA = 0;
	executable->importDirectorySize = 0;
	executable->delayImportDirectoryRVA = 0;
	executable->delayImportDirectorySize = 0;
	executable->tlsDirectoryRVA = 0;
	executable->tlsDirectorySize = 0;
	executable->exceptionDirectoryRVA = 0;
	executable->exceptionDirectorySize = 0;
	wibo_unregisterFunctionTable(executable->functionTable);
	executable->functionTable = NULL;
	executable->execMapped = false;
	executable->importsResolved = false;
	executable->importsResolving = false;
	executable->sectionsProtected = false;
	vec_clear(&executable->sections);
}

static int compareSectionBase(const void *lhs, const void *rhs) {
	uintptr_t lhsBase = ((const Executable_SectionInfo *)lhs)->base;
	uintptr_t rhsBase = ((const Executable_SectionInfo *)rhs)->base;
	return (lhsBase > rhsBase) - (lhsBase < rhsBase);
}

static bool readOptionalHeader(const PeInputView *source, uint64_t offset, uint16_t sizeOfOptionalHeader,
							   PEOptionalHeader *out) {
	memset(out, 0, sizeof(*out));
	uint16_t magic = 0;
	if (sizeOfOptionalHeader < sizeof(magic) || !peViewRead(source, offset, &magic, sizeof(magic))) {
		DEBUG_LOG("loadPE: failed to read optional header magic\n");
		return false;
	}

	if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
		// IMAGE_OPTIONAL_HEADER32 layout: https://learn.microsoft.com/windows/win32/debug/pe-format
		const size_t minimumSize = offsetof(PE32Header, reserved) + sizeof(PEImageDataDirectory);
		if (sizeOfOptionalHeader < minimumSize) {
			DEBUG_LOG("loadPE: optional header too small (%u bytes)\n", sizeOfOptionalHeader);
			return false;
		}
		PE32Header h;
		memset(&h, 0, sizeof(h));
		size_t bytes = sizeOfOptionalHeader < sizeof(h) ? sizeOfOptionalHeader : sizeof(h);
		if (!peViewRead(source, offset, &h, bytes)) {
			DEBUG_LOG("loadPE: failed to read optional header\n");
			return false;
		}
		out->is64 = false;
		out->imageBase = h.imageBase;
		out->addressOfEntryPoint = h.addressOfEntryPoint;
		out->sectionAlignment = h.sectionAlignment;
		out->fileAlignment = h.fileAlignment;
		out->sizeOfImage = h.sizeOfImage;
		out->sizeOfHeaders = h.sizeOfHeaders;
		out->exportTable = h.exportTable;
		out->importTable = h.importTable;
		out->baseRelocationTable = h.baseRelocationTable;
		out->delayImportDescriptor = h.delayImportDescriptor;
		out->tlsTable = h.tlsTable;
		out->exceptionTable = h.exceptionTable;
		return true;
	}

	if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		if (sizeof(uintptr_t) < sizeof(uint64_t)) {
			DEBUG_LOG("loadPE: PE32+ images require a 64-bit host\n");
			return false;
		}
		const size_t minimumSize = offsetof(PE32PlusHeader, reserved) + sizeof(PEImageDataDirectory);
		if (sizeOfOptionalHeader < minimumSize) {
			DEBUG_LOG("loadPE: PE32+ optional header too small (%u bytes)\n", sizeOfOptionalHeader);
			return false;
		}
		PE32PlusHeader h;
		memset(&h, 0, sizeof(h));
		size_t bytes = sizeOfOptionalHeader < sizeof(h) ? sizeOfOptionalHeader : sizeof(h);
		if (!peViewRead(source, offset, &h, bytes)) {
			DEBUG_LOG("loadPE: failed to read PE32+ optional header\n");
			return false;
		}
		out->is64 = true;
		out->imageBase = h.imageBase;
		out->addressOfEntryPoint = h.addressOfEntryPoint;
		out->sectionAlignment = h.sectionAlignment;
		out->fileAlignment = h.fileAlignment;
		out->sizeOfImage = h.sizeOfImage;
		out->sizeOfHeaders = h.sizeOfHeaders;
		out->exportTable = h.exportTable;
		out->importTable = h.importTable;
		out->baseRelocationTable = h.baseRelocationTable;
		out->delayImportDescriptor = h.delayImportDescriptor;
		out->tlsTable = h.tlsTable;
		out->exceptionTable = h.exceptionTable;
		return true;
	}

	DEBUG_LOG("loadPE: unsupported optional header magic 0x%x\n", magic);
	return false;
}

// IAT and ILT entries are pointer-sized: 4 bytes for PE32, 8 bytes for PE32+.
static uint64_t readThunk(const uint8_t *slot, bool is64) {
	if (is64) {
		uint64_t value;
		memcpy(&value, slot, sizeof(value));
		return value;
	}
	uint32_t value;
	memcpy(&value, slot, sizeof(value));
	return value;
}

static void writeThunk(uint8_t *slot, bool is64, uint64_t value) {
	if (is64) {
		memcpy(slot, &value, sizeof(value));
		return;
	}
	uint32_t narrow = (uint32_t)value;
	memcpy(slot, &narrow, sizeof(narrow));
}

static bool loadPEFromSource(Executable *executable, const PeInputView *source, bool exec) {
	resetExecutableState(executable);
	kernel32_setLastError(ERROR_BAD_EXE_FORMAT);

	uint16_t dosSignature = 0;
	if (!peViewRead(source, 0, &dosSignature, sizeof(dosSignature)) || dosSignature != 0x5A4D) {
		DEBUG_LOG("loadPE: missing MZ header signature\n");
		return false;
	}

	uint32_t offsetToPE = 0;
	if (!peViewRead(source, 0x3C, &offsetToPE, sizeof(offsetToPE))) {
		DEBUG_LOG("loadPE: failed to read e_lfanew\n");
		return false;
	}

	if (source->hasSize) {
		if (offsetToPE > source->size || (source->size - offsetToPE) < sizeof(PEHeader)) {
			DEBUG_LOG("loadPE: PE header offset outside data (offset=%u size=%llu)\n", offsetToPE,
					  (unsigned long long)source->size);
			return false;
		}
	}

	PEHeader header;
	memset(&header, 0, sizeof(header));
	if (!peViewRead(source, offsetToPE, &header, sizeof(header))) {
		DEBUG_LOG("loadPE: unable to read PE header\n");
		return false;
	}
	if (memcmp(header.magic, "PE\0\0", 4) != 0) {
		DEBUG_LOG("loadPE: invalid PE signature\n");
		return false;
	}
	if (header.machine != IMAGE_FILE_MACHINE_I386 && header.machine != IMAGE_FILE_MACHINE_AMD64) {
		DEBUG_LOG("loadPE: unsupported machine 0x%x\n", header.machine);
		return false;
	}
	if (header.numberOfSections == 0 || header.numberOfSections > 1024) {
		DEBUG_LOG("loadPE: unreasonable section count %u\n", header.numberOfSections);
		return false;
	}
	executable->isDll = !!(header.characteristics & IMAGE_FILE_DLL);

	PEOptionalHeader optional;
	if (!readOptionalHeader(source, offsetToPE + sizeof(header), header.sizeOfOptionalHeader, &optional)) {
		return false;
	}
	if ((header.machine == IMAGE_FILE_MACHINE_AMD64) != optional.is64) {
		DEBUG_LOG("loadPE: machine 0x%x does not match optional header width\n", header.machine);
		return false;
	}
	if (optional.sizeOfImage == 0 || optional.sizeOfHeaders == 0 || optional.sizeOfHeaders > optional.sizeOfImage) {
		DEBUG_LOG("loadPE: invalid image/header sizes (image=%u headers=%u)\n", optional.sizeOfImage,
				  optional.sizeOfHeaders);
		return false;
	}
	if (optional.fileAlignment == 0 || optional.sectionAlignment == 0) {
		DEBUG_LOG("loadPE: invalid alignment (file=%u section=%u)\n", optional.fileAlignment,
				  optional.sectionAlignment);
		return false;
	}

	DEBUG_LOG("Sections: %u / Size of optional header: %x\n", header.numberOfSections, header.sizeOfOptionalHeader);
	DEBUG_LOG("Image Base: %llx / Size: %x\n", (unsigned long long)optional.imageBase, optional.sizeOfImage);

	long pageSize = sysconf(_SC_PAGE_SIZE);
	const size_t pageSizeValue = pageSize > 0 ? (size_t)pageSize : (size_t)4096;
	DEBUG_LOG("Page size: %x\n", (unsigned int)pageSizeValue);

	executable->preferredImageBase = (uintptr_t)optional.imageBase;
	executable->exportDirectoryRVA = optional.exportTable.virtualAddress;
	executable->exportDirectorySize = optional.exportTable.size;
	executable->relocationDirectoryRVA = optional.baseRelocationTable.virtualAddress;
	executable->relocationDirectorySize = optional.baseRelocationTable.size;
	executable->importDirectoryRVA = optional.importTable.virtualAddress;
	executable->importDirectorySize = optional.importTable.size;
	executable->delayImportDirectoryRVA = optional.delayImportDescriptor.virtualAddress;
	executable->delayImportDirectorySize = optional.delayImportDescriptor.size;
	executable->tlsDirectoryRVA = optional.tlsTable.virtualAddress;
	executable->tlsDirectorySize = optional.tlsTable.size;
	executable->exceptionDirectoryRVA = optional.exceptionTable.virtualAddress;
	executable->exceptionDirectorySize = optional.exceptionTable.size;
	executable->is64Bit = optional.is64;
	executable->execMapped = exec;
	executable->importsResolved = false;
	executable->importsResolving = false;
	executable->sectionsProtected = false;

	executable->imageSize = optional.sizeOfImage;
	DWORD initialProtect = exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
	void *preferredBase = (void *)(uintptr_t)optional.imageBase;
	void *allocatedBase = preferredBase;
	size_t allocationSize = (size_t)optional.sizeOfImage;
	VmStatus allocStatus = wibo_heap_virtualAlloc(&allocatedBase, &allocationSize, MEM_RESERVE | MEM_COMMIT,
												  initialProtect, MEM_IMAGE);
	if (allocStatus != VmStatus_Success) {
		DEBUG_LOG("loadPE: preferred base allocation failed (status=%u), retrying anywhere\n", (unsigned)allocStatus);
		allocatedBase = NULL;
		allocationSize = (size_t)optional.sizeOfImage;
		allocStatus = wibo_heap_virtualAlloc(&allocatedBase, &allocationSize, MEM_RESERVE | MEM_COMMIT,
											 initialProtect, MEM_IMAGE);
	}
	if (allocStatus != VmStatus_Success) {
		DEBUG_LOG("loadPE: mapping failed (status=%u)\n", (unsigned)allocStatus);
		return false;
	}
	DEBUG_LOG("loadPE: mapping succeeded (base=%p, size=%zu)\n", allocatedBase, allocationSize);

	executable->imageBase = allocatedBase;
	executable->relocationDelta =
		(intptr_t)((uintptr_t)executable->imageBase - (uintptr_t)optional.imageBase);
	memset(executable->imageBase, 0, optional.sizeOfImage);
	vec_clear(&executable->sections);

	uintptr_t imageBaseAddr = (uintptr_t)executable->imageBase;
	uintptr_t headerSpan = alignUp((uintptr_t)optional.sizeOfHeaders, pageSizeValue);
	if (headerSpan != 0) {
		Executable_SectionInfo headerInfo;
		memset(&headerInfo, 0, sizeof(headerInfo));
		headerInfo.base = imageBaseAddr;
		headerInfo.size = (size_t)headerSpan;
		headerInfo.protect = PAGE_READONLY;
		vec_push(&executable->sections, headerInfo);
	}

	const uint64_t sectionHeadersOffset = (uint64_t)offsetToPE + sizeof(header) + header.sizeOfOptionalHeader;
	if (source->hasSize) {
		uint64_t sectionTableBytes = (uint64_t)header.numberOfSections * sizeof(PESectionHeader);
		if (sectionHeadersOffset > source->size || sectionTableBytes > (source->size - sectionHeadersOffset)) {
			DEBUG_LOG("loadPE: section table exceeds available data\n");
			goto fail;
		}
	}
	for (uint16_t i = 0; i < header.numberOfSections; ++i) {
		uint64_t currentOffset = sectionHeadersOffset + (uint64_t)i * sizeof(PESectionHeader);
		PESectionHeader section;
		memset(&section, 0, sizeof(section));
		if (!peViewRead(source, currentOffset, &section, sizeof(section))) {
			DEBUG_LOG("loadPE: failed to read section header %u\n", i);
			goto fail;
		}

		char name[9];
		memcpy(name, section.name, 8);
		name[8] = '\0';
		DEBUG_LOG("Section %u: name=%s addr=%x size=%x (raw=%x) ptr=%x\n", i, name, section.virtualAddress,
				  section.virtualSize, section.sizeOfRawData, section.pointerToRawData);

		const uint64_t sectionEndVirtual = (uint64_t)section.virtualAddress + (uint64_t)section.virtualSize;
		if (section.virtualAddress > optional.sizeOfImage || sectionEndVirtual > optional.sizeOfImage) {
			DEBUG_LOG("loadPE: section %s exceeds image size\n", name);
			goto fail;
		}

		void *sectionBase = (void *)(imageBaseAddr + section.virtualAddress);
		if (section.pointerToRawData != 0 && section.sizeOfRawData != 0) {
			uint64_t sectionDataEnd = (uint64_t)section.pointerToRawData + (uint64_t)section.sizeOfRawData;
			if (sectionDataEnd < (uint64_t)section.pointerToRawData) {
				DEBUG_LOG("loadPE: raw data overflow for section %s\n", name);
				goto fail;
			}
			uint64_t mappedEnd = (uint64_t)section.virtualAddress + (uint64_t)section.sizeOfRawData;
			if (mappedEnd > optional.sizeOfImage) {
				DEBUG_LOG("loadPE: raw section data for %s exceeds image size\n", name);
				goto fail;
			}
			if (!peViewRead(source, section.pointerToRawData, sectionBase, section.sizeOfRawData)) {
				DEBUG_LOG("loadPE: failed to load section %s data\n", name);
				goto fail;
			}
		}

		if (strcmp(name, ".rsrc") == 0) {
			executable->rsrcBase = sectionBase;
			executable->rsrcSize = section.virtualSize > section.sizeOfRawData ? section.virtualSize
																			   : section.sizeOfRawData;
		}

		size_t sectionSpan = section.virtualSize > section.sizeOfRawData ? section.virtualSize : section.sizeOfRawData;
		if (sectionSpan != 0) {
			uintptr_t sectionStart = alignDown(imageBaseAddr + (uintptr_t)section.virtualAddress, pageSizeValue);
			uintptr_t sectionEnd =
				alignUp(imageBaseAddr + (uintptr_t)section.virtualAddress + (uintptr_t)sectionSpan, pageSizeValue);
			if (sectionEnd < sectionStart) {
				DEBUG_LOG("loadPE: invalid span for section %s\n", name);
				goto fail;
			}
			if (sectionEnd > sectionStart) {
				Executable_SectionInfo sectionInfo;
				memset(&sectionInfo, 0, sizeof(sectionInfo));
				sectionInfo.base = sectionStart;
				sectionInfo.size = (size_t)(sectionEnd - sectionStart);
				sectionInfo.protect = sectionProtectFromCharacteristics(section.characteristics);
				sectionInfo.characteristics = section.characteristics;
				vec_push(&executable->sections, sectionInfo);
			}
		}
	}

	if (executable->sections.len > 1) {
		qsort(executable->sections.data, executable->sections.len, sizeof(*executable->sections.data),
			  compareSectionBase);
	}

	if (exec && executable->relocationDelta != 0) {
		if (executable->relocationDirectoryRVA == 0 || executable->relocationDirectorySize == 0) {
			DEBUG_LOG("Relocation required but no relocation directory present\n");
			goto fail;
		}

		uint8_t *relocCursor = Executable_fromRVA(executable, executable->relocationDirectoryRVA);
		uint8_t *relocEnd = relocCursor + executable->relocationDirectorySize;
		while (relocCursor < relocEnd) {
			PEBaseRelocationBlock *block = (PEBaseRelocationBlock *)relocCursor;
			if (block->sizeOfBlock < sizeof(PEBaseRelocationBlock) ||
				block->sizeOfBlock > (uint32_t)(relocEnd - relocCursor)) {
				break;
			}
			if (block->sizeOfBlock == sizeof(PEBaseRelocationBlock)) {
				break;
			}
			size_t entryCount = (block->sizeOfBlock - sizeof(PEBaseRelocationBlock)) / sizeof(uint16_t);
			uint16_t *entries = (uint16_t *)(relocCursor + sizeof(PEBaseRelocationBlock));
			for (size_t i = 0; i < entryCount; ++i) {
				uint16_t entry = entries[i];
				uint16_t type = entry >> 12;
				uint16_t offset = entry & 0x0FFF;
				if (type == IMAGE_REL_BASED_ABSOLUTE)
					continue;
				uintptr_t target = (uintptr_t)executable->imageBase + block->virtualAddress + offset;
				switch (type) {
				case IMAGE_REL_BASED_HIGHLOW: {
					uint32_t *addr = (uint32_t *)target;
					*addr += (uint32_t)executable->relocationDelta;
					break;
				}
				case IMAGE_REL_BASED_DIR64: {
					uint64_t *addr = (uint64_t *)target;
					*addr += (uint64_t)executable->relocationDelta;
					break;
				}
				default:
					DEBUG_LOG("Unhandled relocation type %u at %08x\n", type, block->virtualAddress + offset);
					break;
				}
			}
			relocCursor += block->sizeOfBlock;
		}
	}

	executable->entryPoint =
		optional.addressOfEntryPoint ? Executable_fromRVA(executable, optional.addressOfEntryPoint) : NULL;

	// PE32 exception directories are not .pdata; only x64 images have unwind tables.
	if (executable->is64Bit && executable->exceptionDirectorySize >= sizeof(WiboRuntimeFunction) &&
		executable->exceptionDirectoryRVA < executable->imageSize &&
		executable->exceptionDirectorySize <= executable->imageSize - executable->exceptionDirectoryRVA) {
		size_t count = executable->exceptionDirectorySize / sizeof(WiboRuntimeFunction);
		const WiboRuntimeFunction *entries = Executable_fromRVA(executable, executable->exceptionDirectoryRVA);
		executable->functionTable =
			wibo_registerFunctionTable(executable->imageBase, executable->imageSize, entries, count);
	}

	kernel32_setLastError(ERROR_SUCCESS);
	return true;

fail:
	if (executable->imageBase) {
		wibo_heap_virtualFree(executable->imageBase, 0, MEM_RELEASE);
		executable->imageBase = NULL;
	}
	return false;
}

/**
 * Load a PE file into memory.
 *
 * @param file The file to load.
 * @param exec Whether to make the loaded image executable.
 */
bool Executable_loadPEFile(Executable *self, FILE *file, bool exec) {
	if (!file) {
		kernel32_setLastError(ERROR_BAD_EXE_FORMAT);
		return false;
	}
	PeInputView source = peInputViewFromFile(file);
	return loadPEFromSource(self, &source, exec);
}

bool Executable_loadPEImage(Executable *self, const uint8_t *image, size_t imageSize, bool exec) {
	if (!image || imageSize == 0) {
		kernel32_setLastError(ERROR_BAD_EXE_FORMAT);
		return false;
	}
	PeInputView source = peInputViewFromBytes(image, imageSize);
	return loadPEFromSource(self, &source, exec);
}

static bool finalizeSections(Executable *self) {
	if (!self->execMapped || self->sectionsProtected) {
		return true;
	}
	for (size_t i = 0; i < self->sections.len; ++i) {
		const Executable_SectionInfo *section = &self->sections.data[i];
		if (section->size == 0) {
			continue;
		}
		void *sectionAddress = (void *)section->base;
		VmStatus status = wibo_heap_virtualProtect(sectionAddress, section->size, section->protect, NULL);
		if (status != VmStatus_Success) {
			DEBUG_LOG("resolveImports: failed to set section protection at %p (size=%zu, protect=0x%x) status=%u\n",
					  sectionAddress, section->size, section->protect, (unsigned)status);
			kernel32_setLastError(wibo_heap_win32ErrorFromVmStatus(status));
			return false;
		}
	}
	self->sectionsProtected = true;
	return true;
}

bool Executable_resolveImports(Executable *self) {
	if (self->importsResolved || !self->execMapped) {
		self->importsResolved = true;
		self->importsResolving = false;
		return finalizeSections(self);
	}
	if (self->importsResolving) {
		return true;
	}
	self->importsResolving = true;

	if (!self->importDirectoryRVA) {
		self->importsResolved = true;
		self->importsResolving = false;
		return finalizeSections(self);
	}

	PEImportDirectoryEntry *dir = Executable_fromRVA(self, self->importDirectoryRVA);
	if (!dir) {
		self->importsResolved = true;
		self->importsResolving = false;
		return finalizeSections(self);
	}

	while (dir->name) {
		char *dllName = Executable_fromRVA(self, dir->name);
		DEBUG_LOG("DLL Name: %s\n", dllName);

		// Older PEs (NT 3.5-era) omit the ILT and only ship the IAT, which
		// on disk holds the hint/name RVAs that later get overwritten.
		uint32_t iltRVA = dir->importLookupTable ? dir->importLookupTable : dir->importAddressTable;
		uint8_t *lookupCursor = Executable_fromRVA(self, iltRVA);
		uint8_t *addressCursor = Executable_fromRVA(self, dir->importAddressTable);
		const size_t thunkSize = self->is64Bit ? sizeof(uint64_t) : sizeof(uint32_t);
		const uint64_t ordinalFlag = self->is64Bit ? (UINT64_C(1) << 63) : 0x80000000u;

		ModuleInfo *module = wibo_loadModule(dllName);
		if (!module && kernel32_getLastError() != ERROR_MOD_NOT_FOUND) {
			DEBUG_LOG("Failed to load import module %s\n", dllName);
			// lastError is set by loadModule
			self->importsResolved = false;
			self->importsResolving = false;
			return false;
		}

		while (readThunk(lookupCursor, self->is64Bit) != 0) {
			uint64_t lookup = readThunk(lookupCursor, self->is64Bit);
			if (lookup & ordinalFlag) {
				// Import by ordinal
				uint16_t ordinal = lookup & 0xFFFF;
				DEBUG_LOG("  Ordinal: %d\n", ordinal);
				void *func = module ? wibo_resolveFuncByOrdinal(module, ordinal)
									: wibo_resolveMissingImportByOrdinal(dllName, ordinal);
				DEBUG_LOG("    -> %p\n", func);
				writeThunk(addressCursor, self->is64Bit, (uint64_t)(uintptr_t)func);
			} else {
				// Import by name
				PEHintNameTableEntry *hintName = Executable_fromRVA(self, (uint32_t)lookup);
				DEBUG_LOG("  Name: %s (IAT=%p)\n", hintName->name, addressCursor);
				void *func = module ? wibo_resolveFuncByName(module, hintName->name)
									: wibo_resolveMissingImportByName(dllName, hintName->name);
				DEBUG_LOG("    -> %p\n", func);
				writeThunk(addressCursor, self->is64Bit, (uint64_t)(uintptr_t)func);
			}
			lookupCursor += thunkSize;
			addressCursor += thunkSize;
		}
		++dir;
	}

	// TODO: actual delay loading from __delayLoadHelper2
	// if (delayImportDirectoryRVA) {
	// 	DEBUG_LOG("Processing delay import table at RVA %x\n", delayImportDirectoryRVA);
	// 	PEDelayImportDescriptor *delay = Executable_fromRVA(self, delayImportDirectoryRVA);
	// 	while (delay && delay->name) {
	// 		char *dllName = Executable_fromRVA(self, delay->name);
	// 		DEBUG_LOG("Delay DLL Name: %s\n", dllName);
	// 		uint32_t *lookupTable = Executable_fromRVA(self, delay->importNameTable);
	// 		uint32_t *addressTable = Executable_fromRVA(self, delay->importAddressTable);
	// 		ModuleInfo *module = wibo_loadModule(dllName);
	// 		while (*lookupTable) {
	// 			uint32_t lookup = *lookupTable;
	// 			if (lookup & 0x80000000) {
	// 				uint16_t ordinal = lookup & 0xFFFF;
	// 				DEBUG_LOG("  Ordinal: %d (IAT=%p)\n", ordinal, addressTable);
	// 				void *func = module ? wibo_resolveFuncByOrdinal(module, ordinal)
	// 									: wibo_resolveMissingImportByOrdinal(dllName, ordinal);
	// 				*addressTable = (uint32_t)(uintptr_t)func;
	// 			} else {
	// 				PEHintNameTableEntry *hintName = Executable_fromRVA(self, lookup);
	// 				DEBUG_LOG("  Name: %s\n", hintName->name);
	// 				void *func = module ? wibo_resolveFuncByName(module, hintName->name)
	// 									: wibo_resolveMissingImportByName(dllName, hintName->name);
	// 				*addressTable = (uint32_t)(uintptr_t)func;
	// 			}
	// 			++lookupTable;
	// 			++addressTable;
	// 		}
	// 		if (delay->moduleHandle) {
	// 			HMODULE *moduleSlot = Executable_fromRVA(self, delay->moduleHandle);
	// 			if (moduleSlot) {
	// 				*moduleSlot = module;
	// 			}
	// 		}
	// 		++delay;
	// 	}
	// }

	self->importsResolved = true;
	self->importsResolving = false;
	if (!finalizeSections(self)) {
		self->importsResolved = false;
		return false;
	}
	return true;
}
