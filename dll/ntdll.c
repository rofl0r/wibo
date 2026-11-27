#include "ntdll.h"

#include "common.h"
#include "context.h"
#include "context64.h"
#include "errors.h"
#include "files.h"
#include "handles.h"
#include "heap.h"
#include "kernel32/fileapi.h"
#include "kernel32/internal.h"
#include "kernel32/minwinbase.h"
#include "kernel32/processthreadsapi.h"
#include "modules.h"
#include "processes.h"
#include "strutil.h"
#include "types.h"
#include "unwind.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
	NTSTATUS ExitStatus;
	GUEST_PTR PebBaseAddress;
	ULONG_PTR AffinityMask;
	LONG BasePriority;
	ULONG_PTR UniqueProcessId;
	ULONG_PTR InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION;

typedef struct {
	pid_t pid;
	DWORD exitCode;
	PEB *peb;
	bool isCurrentProcess;
} ProcessHandleDetails;

typedef struct {
	RTL_OSVERSIONINFOW base;
	WORD wServicePackMajor;
	WORD wServicePackMinor;
	WORD wSuiteMask;
	BYTE wProductType;
	BYTE wReserved;
} RtlOsVersionInfoEx;

typedef struct {
	bool ok;
	int err;
} StatFetchResult;

static const LONG kDefaultBasePriority = 8;

static const ULONG kOsMajorVersion = 6;
static const ULONG kOsMinorVersion = 2;
static const ULONG kOsBuildNumber = 0;
static const ULONG kOsPlatformId = 2;          // VER_PLATFORM_WIN32_NT
static const BYTE kProductTypeWorkstation = 1; // VER_NT_WORKSTATION

static const ULONGLONG kHundredNanosecondsPerSecond = 10000000ULL;
static const ULONGLONG kUnixEpochAsFileTime = 116444736000000000ULL;

static const LARGE_INTEGER kFileWriteToEndOfFile = {.QuadPart = -1};
static const LARGE_INTEGER kFileUseFilePointerPosition = {.QuadPart = -2};

#if defined(__APPLE__)
static struct timespec accessTimespec(const struct stat *st) { return st->st_atimespec; }
static struct timespec modifyTimespec(const struct stat *st) { return st->st_mtimespec; }
static struct timespec changeTimespec(const struct stat *st) { return st->st_ctimespec; }
#elif defined(__linux__)
static struct timespec accessTimespec(const struct stat *st) { return st->st_atim; }
static struct timespec modifyTimespec(const struct stat *st) { return st->st_mtim; }
static struct timespec changeTimespec(const struct stat *st) { return st->st_ctim; }
#else
static struct timespec accessTimespec(const struct stat *st) {
	return (struct timespec){.tv_sec = st->st_atime, .tv_nsec = 0};
}
static struct timespec modifyTimespec(const struct stat *st) {
	return (struct timespec){.tv_sec = st->st_mtime, .tv_nsec = 0};
}
static struct timespec changeTimespec(const struct stat *st) {
	return (struct timespec){.tv_sec = st->st_ctime, .tv_nsec = 0};
}
#endif

static LONGLONG timespecToFileTime(struct timespec ts) {
#if defined(__SIZEOF_INT128__)
	__int128 ticks = (__int128)ts.tv_sec * (__int128)kHundredNanosecondsPerSecond;
	ticks += (__int128)(ts.tv_nsec / 100);
	ticks += (__int128)kUnixEpochAsFileTime;
	if (ticks < 0) {
		return 0;
	}
	if (ticks > (__int128)INT64_MAX) {
		return INT64_MAX;
	}
	return (LONGLONG)ticks;
#else
	long double ticks = (long double)ts.tv_sec * (long double)kHundredNanosecondsPerSecond;
	ticks += (long double)ts.tv_nsec / 100.0L;
	ticks += (long double)kUnixEpochAsFileTime;
	if (ticks < 0.0L) {
		return 0;
	}
	if (ticks > (long double)LLONG_MAX) {
		return LLONG_MAX;
	}
	return (LONGLONG)ticks;
#endif
}

static DWORD buildFileAttributes(const struct stat *st) {
	DWORD attributes = 0;
	mode_t mode = st->st_mode;
	if (S_ISDIR(mode)) {
		attributes |= FILE_ATTRIBUTE_DIRECTORY;
	}
	if (S_ISREG(mode)) {
		attributes |= FILE_ATTRIBUTE_ARCHIVE;
	}
	if ((mode & S_IWUSR) == 0) {
		attributes |= FILE_ATTRIBUTE_READONLY;
	}
	if (attributes == 0) {
		attributes = FILE_ATTRIBUTE_NORMAL;
	}
	return attributes;
}

static StatFetchResult fetchStat(FsObject *fs, struct stat *st) {
	StatFetchResult res = {.ok = false, .err = 0};
	if (!fs) {
		return res;
	}
	if (FsObject_valid(fs)) {
		if (fstat(fs->fd, st) == 0) {
			res.ok = true;
			return res;
		}
		if (errno != EBADF) {
			res.err = errno;
			return res;
		}
	}
	if (fs->canonicalPath && fs->canonicalPath[0]) {
		if (stat(fs->canonicalPath, st) == 0) {
			res.ok = true;
		} else {
			res.err = errno;
		}
	}
	return res;
}

static bool resolveProcessDetails(HANDLE processHandle, ProcessHandleDetails *details) {
	if (kernel32_isPseudoCurrentProcessHandle(processHandle)) {
		details->pid = getpid();
		details->exitCode = STILL_ACTIVE;
		details->peb = wibo_processPeb;
		details->isCurrentProcess = true;
		return true;
	}

	ObjectBase *ref = Handles_getAs(wibo_handles(), processHandle, ObjectType_Process, NULL);
	if (!ref) {
		return false;
	}
	ProcessObject *po = (ProcessObject *)ref;
	details->pid = po->pid;
	details->exitCode = po->exitCode;
	details->isCurrentProcess = po->pid == getpid();
	details->peb = details->isCurrentProcess ? wibo_processPeb : NULL;
	ObjectBase_unref(ref);
	return true;
}

// Returns a malloc'd Windows path, or an empty string when unavailable.
static char *toWindowsPathOrEmpty(const char *hostPath) {
	char *canonical = files_canonicalPath(hostPath);
	char *windowsPath = canonical ? files_pathToWindows(canonical) : NULL;
	free(canonical);
	return windowsPath ? windowsPath : xstrdup("");
}

// Returns a malloc'd Windows path, or an empty string when unavailable.
static char *windowsImagePathFor(const ProcessHandleDetails *details) {
	if (details->isCurrentProcess && wibo_guestExecutablePath && wibo_guestExecutablePath[0]) {
		return toWindowsPathOrEmpty(wibo_guestExecutablePath);
	}

	char linkPath[64];
	snprintf(linkPath, sizeof(linkPath), "/proc/%ld/exe", (long)details->pid);
	char target[PATH_MAX];
	ssize_t len = readlink(linkPath, target, sizeof(target) - 1);
	if (len < 0) {
		return xstrdup("");
	}
	target[len] = '\0';
	return toWindowsPathOrEmpty(target);
}

// Strips any drive letter and ensures the result starts with a backslash. Returns a malloc'd string.
static char *volumeRelativePath(const char *windowsPath) {
	if (!windowsPath || !windowsPath[0]) {
		return xstrdup("");
	}
	const char *rest = windowsPath;
	if (windowsPath[1] == ':') {
		rest += 2;
	}
	size_t restLen = strlen(rest);
	char *result = xmalloc(restLen + 2);
	if (rest[0] != '\\') {
		result[0] = '\\';
		memcpy(result + 1, rest, restLen + 1);
	} else {
		memcpy(result, rest, restLen + 1);
	}
	return result;
}

PVOID CDECL ntdll_memset(PVOID dest, int ch, SIZE_T count) {
	VERBOSE_LOG("ntdll_memset(%p, %i, %zu)\n", dest, ch, count);
	return memset(dest, ch, count);
}

NTSTATUS WINAPI NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
						   PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
						   PULONG Key) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtReadFile(%p, %p, %p, %p, %p, %p, %u, %p, %p) ", FileHandle, Event, ApcRoutine, ApcContext,
			  IoStatusBlock, Buffer, Length, ByteOffset, Key);
	(void)ApcRoutine;
	(void)ApcContext;
	(void)Key;

	if (!IoStatusBlock) {
		return STATUS_INVALID_PARAMETER;
	}
	IoStatusBlock->Information = 0;

	NTSTATUS status = STATUS_SUCCESS;
	ObjectBase *fileRef = Handles_getWithFlag(wibo_handles(), FileHandle, Of_File, NULL);
	ObjectBase *eventRef = NULL;
	FileObject *file = (FileObject *)fileRef;
	if (!file || !FsObject_valid(&file->base)) {
		IoStatusBlock->Status = STATUS_INVALID_HANDLE;
		IoStatusBlock->Information = 0;
		status = STATUS_INVALID_HANDLE;
		goto done;
	}

	bool useOverlapped = file->overlapped;
	bool useCurrentFilePosition = (ByteOffset == NULL);
	if (!useCurrentFilePosition && ByteOffset->QuadPart == kFileUseFilePointerPosition.QuadPart) {
		useCurrentFilePosition = true;
	}

	off_t offsetValue = 0;
	const off_t *offset = NULL;
	if (!useCurrentFilePosition) {
		offsetValue = (off_t)ByteOffset->QuadPart;
		offset = &offsetValue;
	}

	if (useOverlapped && useCurrentFilePosition) {
		IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
		IoStatusBlock->Information = 0;
		status = STATUS_INVALID_PARAMETER;
		goto done;
	}

	if (Event) {
		eventRef = Handles_getAs(wibo_handles(), Event, ObjectType_Event, NULL);
		if (!eventRef) {
			IoStatusBlock->Status = STATUS_INVALID_HANDLE;
			IoStatusBlock->Information = 0;
			status = STATUS_INVALID_HANDLE;
			goto done;
		}
		EventObject_reset((EventObject *)eventRef);
	}

	bool updateFilePointer = !useOverlapped;
	IOResult io = files_read(file, Buffer, Length, offset, updateFilePointer);
	if (io.unixError != 0) {
		status = wibo_statusFromErrno(io.unixError);
	} else if (io.reachedEnd && io.bytesTransferred == 0) {
		status = file->isPipe ? STATUS_PIPE_BROKEN : STATUS_END_OF_FILE;
	}

	IoStatusBlock->Status = status;
	IoStatusBlock->Information = (ULONG_PTR)io.bytesTransferred;

	if (eventRef && (status == STATUS_SUCCESS || status == STATUS_END_OF_FILE)) {
		EventObject_set((EventObject *)eventRef);
	}

done:
	if (eventRef) {
		ObjectBase_unref(eventRef);
	}
	if (fileRef) {
		ObjectBase_unref(fileRef);
	}
	DEBUG_LOG("-> 0x%x\n", status);
	return status;
}

NTSTATUS WINAPI NtWriteFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
							PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset,
							PULONG Key) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtWriteFile(%p, %p, %p, %p, %p, %p, %u, %p, %p) ", FileHandle, Event, ApcRoutine, ApcContext,
			  IoStatusBlock, Buffer, Length, ByteOffset, Key);
	(void)ApcRoutine;
	(void)ApcContext;
	(void)Key;

	if (!IoStatusBlock) {
		return STATUS_INVALID_PARAMETER;
	}
	IoStatusBlock->Information = 0;

	NTSTATUS status = STATUS_SUCCESS;
	ObjectBase *fileRef = Handles_getWithFlag(wibo_handles(), FileHandle, Of_File, NULL);
	ObjectBase *eventRef = NULL;
	FileObject *file = (FileObject *)fileRef;
	if (!file || !FsObject_valid(&file->base)) {
		IoStatusBlock->Status = STATUS_INVALID_HANDLE;
		status = STATUS_INVALID_HANDLE;
		goto done;
	}

	bool useOverlapped = file->overlapped;
	bool useCurrentFilePosition = (ByteOffset == NULL);
	bool writeToEndOfFile = false;
	if (ByteOffset) {
		if (ByteOffset->QuadPart == kFileUseFilePointerPosition.QuadPart) {
			useCurrentFilePosition = true;
		} else if (ByteOffset->QuadPart == kFileWriteToEndOfFile.QuadPart) {
			writeToEndOfFile = true;
		}
	}

	off_t offsetValue = 0;
	bool hasOffset = false;
	if (!useCurrentFilePosition && !writeToEndOfFile) {
		offsetValue = (off_t)ByteOffset->QuadPart;
		hasOffset = true;
	}

	if (useOverlapped && useCurrentFilePosition) {
		IoStatusBlock->Status = STATUS_INVALID_PARAMETER;
		status = STATUS_INVALID_PARAMETER;
		goto done;
	}

	if (Event) {
		eventRef = Handles_getAs(wibo_handles(), Event, ObjectType_Event, NULL);
		if (!eventRef) {
			IoStatusBlock->Status = STATUS_INVALID_HANDLE;
			status = STATUS_INVALID_HANDLE;
			goto done;
		}
		EventObject_reset((EventObject *)eventRef);
	}

	bool updateFilePointer = file->isPipe ? true : !useOverlapped;

	if (writeToEndOfFile && !file->isPipe) {
		struct stat st = {0};
		if (fstat(file->base.fd, &st) != 0) {
			int err = errno ? errno : EIO;
			status = wibo_statusFromErrno(err);
			IoStatusBlock->Status = status;
			goto done;
		}
		offsetValue = (off_t)st.st_size;
		hasOffset = true;
	}

	const off_t *offset = hasOffset ? &offsetValue : NULL;
	IOResult io = files_write(file, Buffer, (size_t)Length, offset, updateFilePointer);
	if (io.unixError != 0) {
		status = wibo_statusFromErrno(io.unixError);
	}

	IoStatusBlock->Status = status;
	IoStatusBlock->Information = (ULONG_PTR)io.bytesTransferred;

	if (eventRef && status == STATUS_SUCCESS) {
		EventObject_set((EventObject *)eventRef);
	}

done:
	if (eventRef) {
		ObjectBase_unref(eventRef);
	}
	if (fileRef) {
		ObjectBase_unref(fileRef);
	}
	DEBUG_LOG("-> 0x%x\n", status);
	return status;
}

NTSTATUS WINAPI NtAllocateVirtualMemory(HANDLE ProcessHandle, GUEST_PTR *BaseAddress, ULONG_PTR ZeroBits,
										PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtAllocateVirtualMemory(%p, %p, %lu, %p, %lu, %lu) ", ProcessHandle, BaseAddress, ZeroBits, RegionSize,
			  AllocationType, Protect);
	if (ProcessHandle != (HANDLE)-1) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_HANDLE);
		return STATUS_INVALID_HANDLE;
	}
	if (ZeroBits != 0 || BaseAddress == NULL || RegionSize == NULL) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_PARAMETER);
		return STATUS_INVALID_PARAMETER;
	}

	void *baseAddress = fromGuestPtr(*BaseAddress);
	size_t regionSize = (size_t)*RegionSize;
	VmStatus vmStatus =
		wibo_heap_virtualAlloc(&baseAddress, &regionSize, (DWORD)AllocationType, (DWORD)Protect, MEM_PRIVATE);
	if (vmStatus != VmStatus_Success) {
		NTSTATUS status = wibo_heap_ntStatusFromVmStatus(vmStatus);
		DEBUG_LOG("-> 0x%x\n", status);
		return status;
	}

	*BaseAddress = toGuestPtr(baseAddress);
	*RegionSize = (SIZE_T)regionSize;

	DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
	return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtProtectVirtualMemory(HANDLE ProcessHandle, GUEST_PTR *BaseAddress, PSIZE_T NumberOfBytesToProtect,
									   ULONG NewAccessProtection, PULONG OldAccessProtection) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtProtectVirtualMemory(%p, %p, %p, %lu, %p) ", ProcessHandle, BaseAddress, NumberOfBytesToProtect,
			  NewAccessProtection, OldAccessProtection);
	if (ProcessHandle != (HANDLE)-1) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_HANDLE);
		return STATUS_INVALID_HANDLE;
	}
	if (BaseAddress == NULL || NumberOfBytesToProtect == NULL) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_PARAMETER);
		return STATUS_INVALID_PARAMETER;
	}

	void *base = fromGuestPtr(*BaseAddress);
	size_t length = (size_t)*NumberOfBytesToProtect;
	VmStatus vmStatus = wibo_heap_virtualProtect(base, length, (DWORD)NewAccessProtection, OldAccessProtection);
	if (vmStatus != VmStatus_Success) {
		NTSTATUS status = wibo_heap_ntStatusFromVmStatus(vmStatus);
		DEBUG_LOG("-> 0x%x\n", status);
		return status;
	}

	DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
	return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
									   ULONG Length, FILE_INFORMATION_CLASS FileInformationClass) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtQueryInformationFile(%p, %p, %p, %u, %u) ", FileHandle, IoStatusBlock, FileInformation, Length,
			  (unsigned)FileInformationClass);

	if (!IoStatusBlock) {
		DEBUG_LOG("-> 0x%x\n", STATUS_ACCESS_VIOLATION);
		return STATUS_ACCESS_VIOLATION;
	}

	IoStatusBlock->Information = 0;

	if (Length != 0 && !FileInformation) {
		IoStatusBlock->Status = STATUS_ACCESS_VIOLATION;
		DEBUG_LOG("-> 0x%x\n", STATUS_ACCESS_VIOLATION);
		return STATUS_ACCESS_VIOLATION;
	}

	if ((int32_t)(uintptr_t)FileHandle < 0) {
		IoStatusBlock->Status = STATUS_OBJECT_TYPE_MISMATCH;
		DEBUG_LOG("-> 0x%x\n", STATUS_OBJECT_TYPE_MISMATCH);
		return STATUS_OBJECT_TYPE_MISMATCH;
	}

	ObjectBase *objRef = Handles_getWithFlag(wibo_handles(), FileHandle, Of_FsObject, NULL);
	FsObject *obj = (FsObject *)objRef;
	if (!obj || !FsObject_valid(obj)) {
		IoStatusBlock->Status = STATUS_INVALID_HANDLE;
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_HANDLE);
		if (objRef) {
			ObjectBase_unref(objRef);
		}
		return STATUS_INVALID_HANDLE;
	}

	pthread_mutex_lock(&obj->m);
	NTSTATUS status = STATUS_SUCCESS;

	switch (FileInformationClass) {
	case FileBasicInformation: {
		if (Length < sizeof(FILE_BASIC_INFORMATION)) {
			status = STATUS_INFO_LENGTH_MISMATCH;
			break;
		}
		struct stat st = {0};
		StatFetchResult statRes = fetchStat(obj, &st);
		if (!statRes.ok) {
			status = wibo_statusFromErrno(statRes.err != 0 ? statRes.err : EINVAL);
			break;
		}
		PFILE_BASIC_INFORMATION info = (PFILE_BASIC_INFORMATION)FileInformation;
		info->CreationTime.QuadPart = timespecToFileTime(changeTimespec(&st));
		info->LastAccessTime.QuadPart = timespecToFileTime(accessTimespec(&st));
		info->LastWriteTime.QuadPart = timespecToFileTime(modifyTimespec(&st));
		info->ChangeTime.QuadPart = timespecToFileTime(changeTimespec(&st));
		info->FileAttributes = buildFileAttributes(&st);
		IoStatusBlock->Information = sizeof(FILE_BASIC_INFORMATION);
		break;
	}
	case FileStandardInformation: {
		if (Length < sizeof(FILE_STANDARD_INFORMATION)) {
			status = STATUS_INFO_LENGTH_MISMATCH;
			break;
		}
		struct stat st = {0};
		StatFetchResult statRes = fetchStat(obj, &st);
		if (!statRes.ok) {
			status = wibo_statusFromErrno(statRes.err != 0 ? statRes.err : EINVAL);
			break;
		}
		PFILE_STANDARD_INFORMATION info = (PFILE_STANDARD_INFORMATION)FileInformation;
		unsigned long long allocation = (unsigned long long)st.st_blocks * 512ULL;
		info->AllocationSize.QuadPart = (LONGLONG)allocation;
		info->EndOfFile.QuadPart = (LONGLONG)st.st_size;
		info->NumberOfLinks = (ULONG)st.st_nlink;
		info->DeletePending = obj->deletePending ? TRUE : FALSE;
		info->Directory = S_ISDIR(st.st_mode) ? TRUE : FALSE;
		info->Reserved = 0;
		IoStatusBlock->Information = sizeof(FILE_STANDARD_INFORMATION);
		break;
	}
	case FilePositionInformation: {
		FileObject *file = (FileObject *)Handles_getWithFlag(wibo_handles(), FileHandle, Of_File, NULL);
		if (!file) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}
		if (Length < sizeof(FILE_POSITION_INFORMATION)) {
			status = STATUS_INFO_LENGTH_MISMATCH;
		} else {
			PFILE_POSITION_INFORMATION info = (PFILE_POSITION_INFORMATION)FileInformation;
			info->CurrentByteOffset.QuadPart = (LONGLONG)file->filePos;
			IoStatusBlock->Information = sizeof(FILE_POSITION_INFORMATION);
		}
		ObjectBase_unref(&file->base.base);
		break;
	}
	case FileNameInformation: {
		if (Length < sizeof(ULONG)) {
			status = STATUS_INFO_LENGTH_MISMATCH;
			break;
		}
		char *windowsPath = NULL;
		if (obj->canonicalPath) {
			windowsPath = files_pathToWindows(obj->canonicalPath);
		}
		char *volumeRelative = volumeRelativePath(windowsPath);
		free(windowsPath);
		uint16_t *wide = stringToWideString(volumeRelative, SIZE_MAX);
		free(volumeRelative);
		size_t charCount = wstrlen(wide);
		size_t bytesRequired = charCount * sizeof(uint16_t);
		PFILE_NAME_INFORMATION info = (PFILE_NAME_INFORMATION)FileInformation;
		if (Length < sizeof(ULONG) + bytesRequired) {
			info->FileNameLength = (ULONG)bytesRequired;
			status = STATUS_INFO_LENGTH_MISMATCH;
		} else {
			info->FileNameLength = (ULONG)bytesRequired;
			if (bytesRequired > 0) {
				memcpy(info->FileName, wide, bytesRequired);
			}
			IoStatusBlock->Information = (ULONG)(sizeof(ULONG) + bytesRequired);
		}
		free(wide);
		break;
	}
	default:
		DEBUG_LOG("FIXME: NtQueryInformationFile: Unsupported info class");
		status = STATUS_INVALID_INFO_CLASS;
		break;
	}

	pthread_mutex_unlock(&obj->m);
	ObjectBase_unref(objRef);

	IoStatusBlock->Status = status;
	DEBUG_LOG("-> 0x%x\n", status);
	return status;
}

NTSTATUS WINAPI NtQuerySystemTime(PLARGE_INTEGER SystemTime) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtQuerySystemTime(%p) ", SystemTime);
	if (!SystemTime) {
		DEBUG_LOG("-> 0x%x\n", STATUS_ACCESS_VIOLATION);
		return STATUS_ACCESS_VIOLATION;
	}

	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	ULONGLONG sinceUnix = (ULONGLONG)now.tv_sec * kHundredNanosecondsPerSecond + (ULONGLONG)(now.tv_nsec / 100);
	ULONGLONG fileTime = kUnixEpochAsFileTime + sinceUnix;
	SystemTime->QuadPart = (LONGLONG)fileTime;

	DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
	return STATUS_SUCCESS;
}

BOOLEAN WINAPI RtlTimeToSecondsSince1970(PLARGE_INTEGER Time, PULONG ElapsedSeconds) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlTimeToSecondsSince1970(%p, %p) ", Time, ElapsedSeconds);
	if (!Time || !ElapsedSeconds) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	LONGLONG fileTimeSigned = Time->QuadPart;
	if (fileTimeSigned < 0) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	ULONGLONG fileTime = (ULONGLONG)fileTimeSigned;
	if (fileTime < kUnixEpochAsFileTime) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	ULONGLONG delta = fileTime - kUnixEpochAsFileTime;
	ULONGLONG seconds = delta / kHundredNanosecondsPerSecond;
	if (seconds > 0xFFFFFFFFULL) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	*ElapsedSeconds = (ULONG)seconds;
	DEBUG_LOG("-> %u\n", TRUE);
	return TRUE;
}

VOID WINAPI RtlInitializeBitMap(PRTL_BITMAP BitMapHeader, PULONG BitMapBuffer, ULONG SizeOfBitMap) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlInitializeBitMap(%p, %p, %u)\n", BitMapHeader, BitMapBuffer, SizeOfBitMap);
	if (!BitMapHeader) {
		return;
	}

	BitMapHeader->SizeOfBitMap = SizeOfBitMap;
	BitMapHeader->Buffer = toGuestPtr(BitMapBuffer);
}

VOID WINAPI RtlSetBits(PRTL_BITMAP BitMapHeader, ULONG StartingIndex, ULONG NumberToSet) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlSetBits(%p, %u, %u)\n", BitMapHeader, StartingIndex, NumberToSet);
	if (!BitMapHeader || !BitMapHeader->Buffer || NumberToSet == 0) {
		return;
	}

	ULONG size = BitMapHeader->SizeOfBitMap;
	if (StartingIndex >= size) {
		return;
	}

	ULONG available = size - StartingIndex;
	if (NumberToSet > available) {
		NumberToSet = available;
	}

	ULONG *words = fromGuestPtr(BitMapHeader->Buffer);
	for (ULONG i = 0; i < NumberToSet; ++i) {
		ULONG bitIndex = StartingIndex + i;
		ULONG wordIndex = bitIndex / 32;
		ULONG offset = bitIndex % 32;
		words[wordIndex] |= (1u << offset);
	}
}

BOOLEAN WINAPI RtlAreBitsSet(PRTL_BITMAP BitMapHeader, ULONG StartingIndex, ULONG Length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlAreBitsSet(%p, %u, %u) ", BitMapHeader, StartingIndex, Length);
	if (!BitMapHeader || !BitMapHeader->Buffer || Length == 0) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	ULONG size = BitMapHeader->SizeOfBitMap;
	if (StartingIndex >= size || Length > size - StartingIndex) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	ULONG *words = fromGuestPtr(BitMapHeader->Buffer);
	for (ULONG i = 0; i < Length; ++i) {
		ULONG bitIndex = StartingIndex + i;
		ULONG wordIndex = bitIndex / 32;
		ULONG offset = bitIndex % 32;
		if ((words[wordIndex] & (1u << offset)) == 0) {
			DEBUG_LOG("-> %u\n", FALSE);
			return FALSE;
		}
	}

	DEBUG_LOG("-> %u\n", TRUE);
	return TRUE;
}

BOOLEAN WINAPI RtlAreBitsClear(PRTL_BITMAP BitMapHeader, ULONG StartingIndex, ULONG Length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlAreBitsClear(%p, %u, %u) ", BitMapHeader, StartingIndex, Length);
	if (!BitMapHeader || !BitMapHeader->Buffer || Length == 0) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	ULONG size = BitMapHeader->SizeOfBitMap;
	if (StartingIndex >= size || Length > size - StartingIndex) {
		DEBUG_LOG("-> %u\n", FALSE);
		return FALSE;
	}

	ULONG *words = fromGuestPtr(BitMapHeader->Buffer);
	for (ULONG i = 0; i < Length; ++i) {
		ULONG bitIndex = StartingIndex + i;
		ULONG wordIndex = bitIndex / 32;
		ULONG offset = bitIndex % 32;
		if ((words[wordIndex] & (1u << offset)) != 0) {
			DEBUG_LOG("-> %u\n", FALSE);
			return FALSE;
		}
	}

	DEBUG_LOG("-> %u\n", TRUE);
	return TRUE;
}

GUEST_PTR WINAPI RtlLookupFunctionEntry(ULONGLONG ControlPc, GUEST_PTR ImageBase, GUEST_PTR HistoryTable) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlLookupFunctionEntry(0x%llx, %p, %p) ", (unsigned long long)ControlPc, (void *)(uintptr_t)ImageBase,
			  (void *)(uintptr_t)HistoryTable);
#ifdef WIBO_GUEST64
	static __thread WiboRuntimeFunction s_lastEntry;
	uint64_t moduleBase = 0;
	if (!wibo_lookupRuntimeFunction(ControlPc, &moduleBase, &s_lastEntry)) {
		DEBUG_LOG("-> NULL\n");
		return GUEST_NULL;
	}
	if (ImageBase) {
		*(uint64_t *)(uintptr_t)ImageBase = moduleBase;
	}
	DEBUG_LOG("-> %p\n", &s_lastEntry);
	return (GUEST_PTR)(uintptr_t)&s_lastEntry;
#else
	DEBUG_LOG("-> NULL\n");
	return GUEST_NULL;
#endif
}

GUEST_PTR WINAPI RtlVirtualUnwind(DWORD HandlerType, ULONGLONG ImageBase, ULONGLONG ControlPc, GUEST_PTR FunctionEntry,
								  GUEST_PTR ContextRecord, GUEST_PTR HandlerData, GUEST_PTR EstablisherFrame,
								  GUEST_PTR ContextPointers) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlVirtualUnwind(%u, 0x%llx, 0x%llx, %p, %p) ", HandlerType, (unsigned long long)ImageBase,
			  (unsigned long long)ControlPc, (void *)(uintptr_t)FunctionEntry, (void *)(uintptr_t)ContextRecord);
#ifdef WIBO_GUEST64
	// Context pointers (non-volatile register locations) are not reported; callers get the unwound values.
	(void)ContextPointers;
	if (!ContextRecord) {
		return GUEST_NULL;
	}
	WiboWin64Context *context = (WiboWin64Context *)(uintptr_t)ContextRecord;
	WiboContext64 unwind;
	wibo_contextToUnwind(context, &unwind);
	const WiboRuntimeFunction *function = (const WiboRuntimeFunction *)(uintptr_t)FunctionEntry;
	WiboFrameInfo info;
	if (!wibo_unwindWithFunction(&unwind, ControlPc, ImageBase, function, &info)) {
		DEBUG_LOG("-> NULL (unsupported unwind code)\n");
		return GUEST_NULL;
	}
	wibo_contextFromUnwind(&unwind, context);
	if (EstablisherFrame) {
		*(uint64_t *)(uintptr_t)EstablisherFrame = info.establisherFrame;
	}
	if (!(info.handlerFlags & HandlerType) || !info.languageHandler) {
		DEBUG_LOG("-> NULL\n");
		return GUEST_NULL;
	}
	if (HandlerData) {
		*(uint64_t *)(uintptr_t)HandlerData = info.handlerData;
	}
	DEBUG_LOG("-> %p\n", (void *)(uintptr_t)info.languageHandler);
	return (GUEST_PTR)info.languageHandler;
#else
	(void)HandlerData;
	(void)EstablisherFrame;
	(void)ContextPointers;
	DEBUG_LOG("-> NULL\n");
	return GUEST_NULL;
#endif
}

NTSTATUS WINAPI RtlGetVersion(PRTL_OSVERSIONINFOW lpVersionInformation) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlGetVersion(%p) ", lpVersionInformation);
	if (!lpVersionInformation) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_PARAMETER);
		return STATUS_INVALID_PARAMETER;
	}

	ULONG size = lpVersionInformation->dwOSVersionInfoSize;
	if (size < sizeof(RTL_OSVERSIONINFOW)) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_PARAMETER);
		return STATUS_INVALID_PARAMETER;
	}

	memset(lpVersionInformation, 0, (size_t)size);
	lpVersionInformation->dwOSVersionInfoSize = size;
	lpVersionInformation->dwMajorVersion = kOsMajorVersion;
	lpVersionInformation->dwMinorVersion = kOsMinorVersion;
	lpVersionInformation->dwBuildNumber = kOsBuildNumber;
	lpVersionInformation->dwPlatformId = kOsPlatformId;

	if (size >= sizeof(RtlOsVersionInfoEx)) {
		RtlOsVersionInfoEx *extended = (RtlOsVersionInfoEx *)lpVersionInformation;
		extended->wProductType = kProductTypeWorkstation;
	}

	DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
	return STATUS_SUCCESS;
}

NTSTATUS WINAPI NtQueryInformationProcess(HANDLE ProcessHandle, PROCESSINFOCLASS ProcessInformationClass,
										  PVOID ProcessInformation, ULONG ProcessInformationLength,
										  PULONG ReturnLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtQueryInformationProcess(%p, %u, %p, %u, %p) ", ProcessHandle, (unsigned)ProcessInformationClass,
			  ProcessInformation, ProcessInformationLength, ReturnLength);
	if (!ProcessInformation) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_PARAMETER);
		return STATUS_INVALID_PARAMETER;
	}

	ProcessHandleDetails details = {.pid = -1, .exitCode = STILL_ACTIVE, .peb = NULL, .isCurrentProcess = false};
	if (!resolveProcessDetails(ProcessHandle, &details)) {
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_HANDLE);
		return STATUS_INVALID_HANDLE;
	}

	switch (ProcessInformationClass) {
	case ProcessBasicInformation: {
		size_t required = sizeof(PROCESS_BASIC_INFORMATION);
		if (ReturnLength) {
			*ReturnLength = (ULONG)required;
		}
		if (ProcessInformationLength < required) {
			DEBUG_LOG("-> 0x%x\n", STATUS_INFO_LENGTH_MISMATCH);
			return STATUS_INFO_LENGTH_MISMATCH;
		}

		PROCESS_BASIC_INFORMATION *info = (PROCESS_BASIC_INFORMATION *)ProcessInformation;
		memset(info, 0, sizeof(*info));
		info->ExitStatus = (NTSTATUS)details.exitCode;
		info->PebBaseAddress = toGuestPtr(details.peb);
		DWORD_PTR processMask = 0;
		DWORD_PTR systemMask = 0;
		if (GetProcessAffinityMask(ProcessHandle, &processMask, &systemMask)) {
			info->AffinityMask = (ULONG_PTR)(processMask == 0 ? 1 : processMask);
		} else {
			info->AffinityMask = 1;
		}
		info->BasePriority = kDefaultBasePriority;
		info->UniqueProcessId = (ULONG_PTR)details.pid;
		if (details.isCurrentProcess) {
			info->InheritedFromUniqueProcessId = (ULONG_PTR)getppid();
		} else {
			info->InheritedFromUniqueProcessId = (ULONG_PTR)getpid();
		}
		DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
		return STATUS_SUCCESS;
	}
	case ProcessWow64Information: {
		size_t required = sizeof(ULONG_PTR);
		if (ReturnLength) {
			*ReturnLength = (ULONG)required;
		}
		if (ProcessInformationLength < required) {
			DEBUG_LOG("-> 0x%x\n", STATUS_INFO_LENGTH_MISMATCH);
			return STATUS_INFO_LENGTH_MISMATCH;
		}
		ULONG_PTR *value = (ULONG_PTR *)ProcessInformation;
		*value = 0;
		DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
		return STATUS_SUCCESS;
	}
	case ProcessImageFileName: {
		size_t minimum = sizeof(UNICODE_STRING);
		if (ProcessInformationLength < minimum) {
			if (ReturnLength) {
				*ReturnLength = (ULONG)minimum;
			}
			DEBUG_LOG("-> 0x%x\n", STATUS_INFO_LENGTH_MISMATCH);
			return STATUS_INFO_LENGTH_MISMATCH;
		}

		char *imagePath = windowsImagePathFor(&details);
		DEBUG_LOG("  NtQueryInformationProcess image path: %s\n", imagePath);
		uint16_t *widePath = stringToWideString(imagePath, SIZE_MAX);
		free(imagePath);
		size_t units = wstrlen(widePath) + 1;
		size_t stringBytes = units * sizeof(uint16_t);
		size_t required = sizeof(UNICODE_STRING) + stringBytes;
		if (ReturnLength) {
			*ReturnLength = (ULONG)required;
		}
		if (ProcessInformationLength < required) {
			free(widePath);
			DEBUG_LOG("-> 0x%x\n", STATUS_INFO_LENGTH_MISMATCH);
			return STATUS_INFO_LENGTH_MISMATCH;
		}

		UNICODE_STRING *unicode = (UNICODE_STRING *)ProcessInformation;
		uint16_t *buffer = (uint16_t *)((uint8_t *)ProcessInformation + sizeof(UNICODE_STRING));
		memcpy(buffer, widePath, stringBytes);
		unicode->Length = (unsigned short)((units - 1) * sizeof(uint16_t));
		unicode->MaximumLength = (unsigned short)stringBytes;
		unicode->Buffer = toGuestPtr(buffer);
		free(widePath);
		DEBUG_LOG("-> 0x%x\n", STATUS_SUCCESS);
		return STATUS_SUCCESS;
	}
	default:
		DEBUG_LOG("-> 0x%x\n", STATUS_INVALID_INFO_CLASS);
		return STATUS_INVALID_INFO_CLASS;
	}
}

NTSTATUS WINAPI LdrAddRefDll(ULONG Flags, HMODULE Module) {
	DEBUG_LOG("STUB: LdrAddRefDll(%x, %p)\n", Flags, Module);
	(void)Flags;
	(void)Module;
	return STATUS_SUCCESS;
}

#include "ntdll_trampolines.h"

const ModuleStub lib_ntdll = {
	.names =
		(const char *[]){
			"ntdll",
			NULL,
		},
	.byName = ntdllThunkByName,
};
