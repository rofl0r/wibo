// C port naming: namespace functions and variables become full-path ns_name (wibo_debugEnabled, wibo_tls_reserveSlot,
// files_read); namespace types and constants keep bare names; nested types become Outer_Inner; methods become
// ObjType_method(ObjType *self, ...); factories become ObjType_fromX; std::optional becomes a NULL-able pointer;
// std::span becomes (ptr, len); owned pointers are freed explicitly; C++ guest_ptr<T> becomes GUEST_PTR, converted with
// toGuestPtr()/fromGuestPtr() (void * in and out).
#pragma once

#include "macros.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Annotation macros for code generation
#ifdef WIBO_CODEGEN
#define WIBO_ANNOTATE(x) __attribute__((annotate(x)))
#else
#define WIBO_ANNOTATE(x)
#endif

// SAL-style directionality
#define _In_ WIBO_ANNOTATE("SAL:in")
#define _Out_ WIBO_ANNOTATE("SAL:out")
#define _Inout_ WIBO_ANNOTATE("SAL:inout")
#define _In_opt_ WIBO_ANNOTATE("SAL:in_opt")
#define _Out_opt_ WIBO_ANNOTATE("SAL:out_opt")

// Byte-counted buffers
#define _In_reads_bytes_(n) WIBO_ANNOTATE("SAL:in_bcount(" #n ")")
#define _Out_writes_bytes_(n) WIBO_ANNOTATE("SAL:out_bcount(" #n ")")

// Codegen annotation for calling convention
#define _CC_CDECL WIBO_ANNOTATE("CC:cdecl")
#define _CC_STDCALL WIBO_ANNOTATE("CC:stdcall")

// Instructs codegen to convert between calling conventions
#ifdef __x86_64__
#define WINAPI _CC_STDCALL
#define CDECL _CC_CDECL
#define CDECL_NO_CONV _CC_CDECL __attribute__((force_align_arg_pointer))
#define CDECL_VARARG _CC_CDECL
#else
#define WINAPI _CC_STDCALL __attribute__((fastcall))
#define CDECL _CC_CDECL __attribute__((fastcall))
#define CDECL_NO_CONV _CC_CDECL __attribute__((cdecl, force_align_arg_pointer))
// GCC ignores fastcall on variadic functions, so declare them cdecl.
#define CDECL_VARARG _CC_CDECL __attribute__((cdecl))
#endif

// Used for host-to-guest calls
#define GUEST_STDCALL __attribute__((stdcall))

// The Win64 runtime (WIBO_GUEST64) uses native pointers, so guest and host addresses are identical.
#ifdef WIBO_GUEST64
typedef uint64_t GUEST_PTR;
#else
typedef unsigned int GUEST_PTR;
#endif
#define GUEST_NULL ((GUEST_PTR)0)

// PE images whose bitness differs from the runtime's cannot be run by this build.
#ifdef WIBO_GUEST64
#define GUEST_IS_64BIT true
#else
#define GUEST_IS_64BIT false
#endif

#if defined(__x86_64__) && !defined(WIBO_GUEST64)
static inline GUEST_PTR toGuestPtr(const void *addr) {
	uint64_t addr64 = (uint64_t)(uintptr_t)addr;
	if (addr64 > 0xFFFFFFFF)
		__builtin_trap();
	return (GUEST_PTR)addr64;
}
#else
static inline GUEST_PTR toGuestPtr(const void *addr) {
	return (GUEST_PTR)(uintptr_t)addr;
}
#endif
static inline void *fromGuestPtr(GUEST_PTR addr) {
	return (void *)(uintptr_t)addr;
}

typedef void VOID;
#ifdef WIBO_GUEST64
typedef intptr_t HANDLE;
#else
typedef int HANDLE;
#endif
typedef HANDLE HMODULE;
typedef GUEST_PTR HGLOBAL;
typedef GUEST_PTR HLOCAL;
typedef GUEST_PTR HRSRC;
typedef HANDLE HINSTANCE;
typedef HANDLE *LPHANDLE;
typedef HANDLE *PHANDLE;
typedef HANDLE HKL;
typedef VOID *PVOID;
typedef VOID *LPVOID;
typedef const VOID *LPCVOID;
typedef VOID *FARPROC;
typedef unsigned short WORD;
typedef WORD *LPWORD;
typedef WORD LANGID;
typedef WORD ATOM;
typedef unsigned int DWORD;
typedef DWORD *PDWORD;
typedef DWORD *LPDWORD;
typedef int LONG;
typedef LONG *PLONG;
typedef unsigned int ULONG;
typedef ULONG *PULONG;
typedef long long LONGLONG __attribute__((aligned(8)));
typedef unsigned long long ULONGLONG __attribute__((aligned(8)));
#ifdef WIBO_GUEST64
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t UINT_PTR;
#else
typedef int LONG_PTR;
typedef unsigned int ULONG_PTR;
typedef unsigned int UINT_PTR;
#endif
typedef ULONG_PTR DWORD_PTR;
typedef DWORD_PTR *PDWORD_PTR;
typedef short SHORT;
typedef unsigned short USHORT;
typedef char CHAR;
typedef CHAR *LPSTR;
typedef const char *LPCSTR;
typedef const char *LPCCH;
typedef unsigned short WCHAR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef const WCHAR *LPCWCH;
typedef CHAR *LPCH;
typedef WCHAR *LPWCH;
typedef int BOOL;
typedef BOOL *PBOOL;
typedef BOOL *LPBOOL;
typedef unsigned char UCHAR;
typedef UCHAR *PUCHAR;
typedef ULONG_PTR SIZE_T;
typedef SIZE_T *PSIZE_T;
typedef unsigned char BYTE;
typedef unsigned char BOOLEAN;
typedef unsigned int UINT;
typedef UINT *PUINT;
typedef HANDLE HKEY;
typedef HKEY *PHKEY;
typedef VOID *PSID;
typedef DWORD REGSAM;
typedef LONG LSTATUS;
typedef DWORD LCID;
typedef DWORD LCTYPE;
typedef HANDLE HWINSTA;
typedef HANDLE HWND;
typedef BYTE *PBYTE;
typedef BYTE *LPBYTE;
typedef WCHAR *PWSTR;

#define NO_HANDLE ((HANDLE)0)

typedef LONG NTSTATUS;
typedef LONG HRESULT;

typedef union _LARGE_INTEGER {
	struct {
		DWORD LowPart;
		LONG HighPart;
	} DUMMYSTRUCTNAME;
	struct {
		DWORD LowPart;
		LONG HighPart;
	} u;
	LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
	struct {
		DWORD LowPart;
		DWORD HighPart;
	} DUMMYSTRUCTNAME;
	struct {
		DWORD LowPart;
		DWORD HighPart;
	} u;
	ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef struct _RTL_BITMAP {
	ULONG SizeOfBitMap;
	GUEST_PTR Buffer;
} RTL_BITMAP, *PRTL_BITMAP;

typedef enum FILE_INFORMATION_CLASS {
	FileDirectoryInformation = 1,
	FileFullDirectoryInformation = 2,
	FileBothDirectoryInformation = 3,
	FileBasicInformation = 4,
	FileStandardInformation = 5,
	FileInternalInformation = 6,
	FileEaInformation = 7,
	FileAccessInformation = 8,
	FileNameInformation = 9,
	FileRenameInformation = 10,
	FileLinkInformation = 11,
	FileNamesInformation = 12,
	FileDispositionInformation = 13,
	FilePositionInformation = 14,
	FileFullEaInformation = 15,
	FileModeInformation = 16,
	FileAlignmentInformation = 17,
	FileAllInformation = 18,
	FileAllocationInformation = 19,
	FileEndOfFileInformation = 20,
} FILE_INFORMATION_CLASS;

typedef struct _FILE_BASIC_INFORMATION {
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	ULONG FileAttributes;
} FILE_BASIC_INFORMATION, *PFILE_BASIC_INFORMATION;

typedef struct _FILE_STANDARD_INFORMATION {
	LARGE_INTEGER AllocationSize;
	LARGE_INTEGER EndOfFile;
	ULONG NumberOfLinks;
	BOOLEAN DeletePending;
	BOOLEAN Directory;
	USHORT Reserved;
} FILE_STANDARD_INFORMATION, *PFILE_STANDARD_INFORMATION;

typedef struct _FILE_POSITION_INFORMATION {
	LARGE_INTEGER CurrentByteOffset;
} FILE_POSITION_INFORMATION, *PFILE_POSITION_INFORMATION;

typedef struct _FILE_NAME_INFORMATION {
	ULONG FileNameLength;
	WCHAR FileName[1];
} FILE_NAME_INFORMATION, *PFILE_NAME_INFORMATION;

typedef struct GUID {
	DWORD Data1;
	WORD Data2;
	WORD Data3;
	BYTE Data4[8];
} GUID;
typedef struct LUID {
	DWORD LowPart;
	LONG HighPart;
} LUID;
typedef LUID *PLUID;
typedef LUID *LPLUID;

#define TRUE ((BOOL)1)
#define FALSE ((BOOL)0)

#define STILL_ACTIVE ((DWORD)259)

#define FILE_FLAG_BACKUP_SEMANTICS ((DWORD)0x02000000)
#define FILE_FLAG_DELETE_ON_CLOSE ((DWORD)0x04000000)
#define FILE_FLAG_FIRST_PIPE_INSTANCE ((DWORD)0x00080000)
#define FILE_FLAG_NO_BUFFERING ((DWORD)0x20000000)
#define FILE_FLAG_OVERLAPPED ((DWORD)0x40000000)
#define FILE_FLAG_WRITE_THROUGH ((DWORD)0x80000000)

#define STD_INPUT_HANDLE ((DWORD)(-10))
#define STD_OUTPUT_HANDLE ((DWORD)(-11))
#define STD_ERROR_HANDLE ((DWORD)(-12))

#define FILE_READ_DATA ((DWORD)0x00000001)
#define FILE_LIST_DIRECTORY ((DWORD)0x00000001)
#define FILE_WRITE_DATA ((DWORD)0x00000002)
#define FILE_ADD_FILE ((DWORD)0x00000002)
#define FILE_APPEND_DATA ((DWORD)0x00000004)
#define FILE_ADD_SUBDIRECTORY ((DWORD)0x00000004)
#define FILE_CREATE_PIPE_INSTANCE ((DWORD)0x00000004)
#define FILE_READ_EA ((DWORD)0x00000008)
#define FILE_WRITE_EA ((DWORD)0x00000010)
#define FILE_EXECUTE ((DWORD)0x00000020)
#define FILE_TRAVERSE ((DWORD)0x00000020)
#define FILE_DELETE_CHILD ((DWORD)0x00000040)
#define FILE_READ_ATTRIBUTES ((DWORD)0x00000080)
#define FILE_WRITE_ATTRIBUTES ((DWORD)0x00000100)

#define SYNCHRONIZE ((DWORD)0x00100000)
#define DELETE ((DWORD)0x00010000)
#define WRITE_DAC ((DWORD)0x00040000)
#define WRITE_OWNER ((DWORD)0x00080000)
#define ACCESS_SYSTEM_SECURITY ((DWORD)0x01000000)

#define STANDARD_RIGHTS_READ ((DWORD)0x00020000)
#define STANDARD_RIGHTS_WRITE ((DWORD)0x00020000)
#define STANDARD_RIGHTS_EXECUTE ((DWORD)0x00020000)
#define STANDARD_RIGHTS_REQUIRED ((DWORD)0x000f0000)
#define STANDARD_RIGHTS_ALL ((DWORD)0x001f0000)

#define FILE_GENERIC_READ                                                                                              \
	(STANDARD_RIGHTS_READ | FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA | SYNCHRONIZE)
#define FILE_GENERIC_WRITE                                                                                             \
	(STANDARD_RIGHTS_WRITE | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | FILE_APPEND_DATA | SYNCHRONIZE)
#define FILE_GENERIC_EXECUTE (STANDARD_RIGHTS_EXECUTE | FILE_READ_ATTRIBUTES | FILE_EXECUTE | SYNCHRONIZE)
#define FILE_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | SYNCHRONIZE | 0x1FF)

#define EVENT_ALL_ACCESS ((DWORD)0x1F0003)
#define MUTEX_ALL_ACCESS ((DWORD)0x1F0001)
#define SEMAPHORE_ALL_ACCESS ((DWORD)0x1F0003)

#define GENERIC_READ ((DWORD)0x80000000)
#define GENERIC_WRITE ((DWORD)0x40000000)
#define GENERIC_EXECUTE ((DWORD)0x20000000)
#define GENERIC_ALL ((DWORD)0x10000000)

// Page protection constants
#define PAGE_NOACCESS ((DWORD)0x01)
#define PAGE_READONLY ((DWORD)0x02)
#define PAGE_READWRITE ((DWORD)0x04)
#define PAGE_WRITECOPY ((DWORD)0x08)
#define PAGE_EXECUTE ((DWORD)0x10)
#define PAGE_EXECUTE_READ ((DWORD)0x20)
#define PAGE_EXECUTE_READWRITE ((DWORD)0x40)
#define PAGE_EXECUTE_WRITECOPY ((DWORD)0x80)
#define PAGE_GUARD ((DWORD)0x100)
#define PAGE_NOCACHE ((DWORD)0x200)
#define PAGE_WRITECOMBINE ((DWORD)0x400)

// Allocation type and memory state constants
#define MEM_COMMIT ((DWORD)0x00001000)
#define MEM_RESERVE ((DWORD)0x00002000)
#define MEM_DECOMMIT ((DWORD)0x00004000)
#define MEM_RELEASE ((DWORD)0x00008000)
#define MEM_FREE ((DWORD)0x00010000)
#define MEM_PRIVATE ((DWORD)0x00020000)
#define MEM_MAPPED ((DWORD)0x00040000)
#define MEM_RESET ((DWORD)0x00080000)
#define MEM_TOP_DOWN ((DWORD)0x00100000)
#define MEM_WRITE_WATCH ((DWORD)0x00200000)
#define MEM_PHYSICAL ((DWORD)0x00400000)
#define MEM_RESET_UNDO ((DWORD)0x01000000)
#define MEM_LARGE_PAGES ((DWORD)0x20000000)
#define MEM_COALESCE_PLACEHOLDERS ((DWORD)0x00000001)
#define MEM_PRESERVE_PLACEHOLDER ((DWORD)0x00000002)
#define MEM_IMAGE ((DWORD)0x01000000)

// File mapping access flags
#define FILE_MAP_COPY ((DWORD)0x00000001)
#define FILE_MAP_WRITE ((DWORD)0x00000002)
#define FILE_MAP_READ ((DWORD)0x00000004)
#define FILE_MAP_EXECUTE ((DWORD)0x00000020)
#define FILE_MAP_ALL_ACCESS ((DWORD)0x000f001f)

// File share modes
#define FILE_SHARE_READ ((DWORD)0x00000001)
#define FILE_SHARE_WRITE ((DWORD)0x00000002)
#define FILE_SHARE_DELETE ((DWORD)0x00000004)

#define PIPE_ACCESS_INBOUND ((DWORD)0x00000001)
#define PIPE_ACCESS_OUTBOUND ((DWORD)0x00000002)
#define PIPE_ACCESS_DUPLEX ((DWORD)0x00000003)

#define PIPE_TYPE_BYTE ((DWORD)0x00000000)
#define PIPE_TYPE_MESSAGE ((DWORD)0x00000004)
#define PIPE_READMODE_BYTE ((DWORD)0x00000000)
#define PIPE_READMODE_MESSAGE ((DWORD)0x00000002)
#define PIPE_WAIT ((DWORD)0x00000000)
#define PIPE_NOWAIT ((DWORD)0x00000001)
#define PIPE_ACCEPT_REMOTE_CLIENTS ((DWORD)0x00000000)
#define PIPE_REJECT_REMOTE_CLIENTS ((DWORD)0x00000008)
#define PIPE_UNLIMITED_INSTANCES ((DWORD)255)

#define kTlsSlotCount ((SIZE_T)64)

typedef struct _UNICODE_STRING {
	USHORT Length;
	USHORT MaximumLength;
	GUEST_PTR Buffer;
} UNICODE_STRING;
typedef GUEST_PTR PUNICODE_STRING;

typedef struct _RTL_USER_PROCESS_PARAMETERS {
	BYTE Reserved1[16];
	GUEST_PTR Reserved2[10];
	UNICODE_STRING ImagePathName;
	UNICODE_STRING CommandLine;
} RTL_USER_PROCESS_PARAMETERS, *PRTL_USER_PROCESS_PARAMETERS;

typedef struct _LIST_ENTRY {
	GUEST_PTR Flink;
	GUEST_PTR Blink;
} LIST_ENTRY;
typedef GUEST_PTR PLIST_ENTRY;
typedef GUEST_PTR PRLIST_ENTRY;

typedef struct _PEB_LDR_DATA {
	BYTE Reserved1[8];
	PVOID Reserved2[3];
	LIST_ENTRY InMemoryOrderModuleList;
} PEB_LDR_DATA, *PPEB_LDR_DATA;

typedef void(_CC_STDCALL *PS_POST_PROCESS_INIT_ROUTINE)();
typedef PS_POST_PROCESS_INIT_ROUTINE *PPS_POST_PROCESS_INIT_ROUTINE;

typedef struct _PEB {
	BYTE Reserved1[2];
	BYTE BeingDebugged;
	BYTE Reserved2[1];
	GUEST_PTR Reserved3[2];
	GUEST_PTR Ldr;
	GUEST_PTR ProcessParameters;
	GUEST_PTR Reserved4[3];
	GUEST_PTR AtlThunkSListPtr;
	GUEST_PTR Reserved5;
	ULONG Reserved6;
	GUEST_PTR Reserved7;
	ULONG Reserved8;
	ULONG AtlThunkSListPtr32;
	GUEST_PTR Reserved9[45];
	BYTE Reserved10[96];
	GUEST_PTR PostProcessInitRoutine;
	BYTE Reserved11[128];
	GUEST_PTR Reserved12[1];
	ULONG SessionId;
} PEB;
typedef GUEST_PTR PPEB;

typedef struct CLIENT_ID {
	HANDLE UniqueProcess;
	HANDLE UniqueThread;
} CLIENT_ID;

struct _ACTIVATION_CONTEXT;

typedef struct _RTL_ACTIVATION_CONTEXT_STACK_FRAME {
	GUEST_PTR Previous;
	struct _ACTIVATION_CONTEXT *ActivationContext;
	ULONG Flags;
} RTL_ACTIVATION_CONTEXT_STACK_FRAME, *PRTL_ACTIVATION_CONTEXT_STACK_FRAME;

typedef struct _ACTIVATION_CONTEXT_STACK {
	PRTL_ACTIVATION_CONTEXT_STACK_FRAME ActiveFrame;
	LIST_ENTRY FrameListCache;
	ULONG Flags;
	ULONG NextCookieSequenceNumber;
	ULONG StackId;
} ACTIVATION_CONTEXT_STACK, *PACTIVATION_CONTEXT_STACK;

#define GDI_BATCH_BUFFER_SIZE 0x136

typedef struct _GDI_TEB_BATCH {
	ULONG Offset;
	HANDLE HDC;
	ULONG Buffer[GDI_BATCH_BUFFER_SIZE];
} GDI_TEB_BATCH, *PGDI_TEB_BATCH;

typedef struct _NT_TIB {
	GUEST_PTR ExceptionList;
	GUEST_PTR StackBase;
	GUEST_PTR StackLimit;
	GUEST_PTR SubSystemTib;
	union {
		GUEST_PTR FiberData;
		DWORD Version;
	} DUMMYUNIONNAME;
	GUEST_PTR ArbitraryUserPointer;
	GUEST_PTR Self;
} NT_TIB, *PNT_TIB;

typedef struct _TEB {
	NT_TIB Tib;
	GUEST_PTR EnvironmentPointer;
	CLIENT_ID ClientId;
	GUEST_PTR ActiveRpcHandle;
	GUEST_PTR ThreadLocalStoragePointer;
	PPEB Peb;
	ULONG LastErrorValue;
	ULONG CountOfOwnedCriticalSections;
	GUEST_PTR CsrClientThread;
	GUEST_PTR Win32ThreadInfo;
	ULONG Win32ClientInfo[31]; /* used for user32 private data in Wine */
	GUEST_PTR WOW32Reserved;
	ULONG CurrentLocale;
	ULONG FpSoftwareStatusRegister;
	GUEST_PTR SystemReserved1[54]; /* used for kernel32 private data in Wine */
	GUEST_PTR Spare1;
	LONG ExceptionCode;
	GUEST_PTR ActivationContextStackPointer;
	BYTE SpareBytes1[36];
	GUEST_PTR SystemReserved2[10]; /* used for ntdll private data in Wine */
	GDI_TEB_BATCH GdiTebBatch;
	ULONG gdiRgn;
	ULONG gdiPen;
	ULONG gdiBrush;
	CLIENT_ID RealClientId;
	GUEST_PTR GdiCachedProcessHandle;
	ULONG GdiClientPID;
	ULONG GdiClientTID;
	GUEST_PTR GdiThreadLocaleInfo;
	GUEST_PTR UserReserved[5];
	GUEST_PTR glDispatchTable[280];
	ULONG glReserved1[26];
	GUEST_PTR glReserved2;
	GUEST_PTR glSectionInfo;
	GUEST_PTR glSection;
	GUEST_PTR glTable;
	GUEST_PTR glCurrentRC;
	GUEST_PTR glContext;
	ULONG LastStatusValue;
	UNICODE_STRING StaticUnicodeString;
	WCHAR StaticUnicodeBuffer[261];
#ifdef WIBO_GUEST64
	BYTE X64TebTailPadding[0x60]; // places DeallocationStack and TlsSlots at the native x64 offsets
#endif
	GUEST_PTR DeallocationStack;
	GUEST_PTR TlsSlots[64];
	LIST_ENTRY TlsLinks;
	GUEST_PTR Vdm;
	GUEST_PTR ReservedForNtRpc;
	GUEST_PTR DbgSsReserved[2];
	ULONG HardErrorDisabled;
	GUEST_PTR Instrumentation[16];
	GUEST_PTR WinSockData;
	ULONG GdiBatchCount;
	ULONG Spare2;
	ULONG Spare3;
	ULONG Spare4;
	GUEST_PTR ReservedForOle;
	ULONG WaitingOnLoaderLock;
	GUEST_PTR Reserved5[3];
	GUEST_PTR TlsExpansionSlots;
	// wibo
	WORD CurrentFsSelector;
	WORD CurrentGsSelector;
#ifdef __x86_64__
	WORD CodeSelector;
	WORD DataSelector;
#endif
	void *CurrentStackPointer;
#ifdef __x86_64__
	void *HostFsBase;
	void *HostGsBase;
	bool HasFsGsBase;
	WORD HostCodeSelector;
#endif
	// Stack pointer of the innermost host-to-guest activation; SEH dispatch never resumes above it.
	void *GuestBoundary;
#ifdef __x86_64__
	// Innermost Win64 thunk call record (see emitThunkWin64). Read by SEH dispatch to find guest frames.
	void *GuestCallFrame;
#endif
} TEB;
typedef GUEST_PTR PTEB;

#ifdef WIBO_GUEST64
// Native x64 TEB offsets that Win64 code reads through gs:.
_Static_assert(offsetof(NT_TIB, Self) == 0x30, "Self pointer offset mismatch");
_Static_assert(offsetof(TEB, ThreadLocalStoragePointer) == 0x58, "TLS pointer offset mismatch");
_Static_assert(offsetof(TEB, Peb) == 0x60, "PEB pointer offset mismatch");
_Static_assert(offsetof(TEB, LastErrorValue) == 0x68, "LastErrorValue offset mismatch");
_Static_assert(offsetof(TEB, DeallocationStack) == 0x1478, "DeallocationStack offset mismatch");
_Static_assert(offsetof(TEB, TlsSlots) == 0x1480, "TLS slots offset mismatch");
#else
_Static_assert(offsetof(NT_TIB, Self) == TEB_SELF, "Self pointer offset mismatch");
_Static_assert(offsetof(TEB, ThreadLocalStoragePointer) == 0x2C, "TLS pointer offset mismatch");
_Static_assert(offsetof(TEB, Peb) == 0x30, "PEB pointer offset mismatch");
_Static_assert(offsetof(TEB, LastErrorValue) == 0x34, "LastErrorValue offset mismatch");
_Static_assert(offsetof(TEB, GdiTebBatch) == 0x1FC, "GdiTebBatch offset mismatch");
_Static_assert(offsetof(TEB, DeallocationStack) == 0xE0C, "DeallocationStack offset mismatch");
_Static_assert(offsetof(TEB, TlsSlots) == 0xE10, "TLS slots offset mismatch");
_Static_assert(offsetof(TEB, CurrentFsSelector) == TEB_FS_SEL, "FS selector offset mismatch");
_Static_assert(offsetof(TEB, CurrentGsSelector) == TEB_GS_SEL, "GS selector offset mismatch");
#ifdef TEB_CS_SEL
_Static_assert(offsetof(TEB, CodeSelector) == TEB_CS_SEL, "CS selector offset mismatch");
#endif
#ifdef TEB_DS_SEL
_Static_assert(offsetof(TEB, DataSelector) == TEB_DS_SEL, "DS selector offset mismatch");
#endif
_Static_assert(offsetof(TEB, CurrentStackPointer) == TEB_SP, "Stack pointer offset mismatch");
_Static_assert(offsetof(TEB, GuestBoundary) == TEB_BOUNDARY, "GuestBoundary offset mismatch");
#ifdef TEB_GUEST_FRAME
_Static_assert(offsetof(TEB, GuestCallFrame) == TEB_GUEST_FRAME, "GuestCallFrame offset mismatch");
#endif
#ifdef TEB_FSBASE
_Static_assert(offsetof(TEB, HostFsBase) == TEB_FSBASE, "HostFsBase offset mismatch");
#endif
#ifdef TEB_GSBASE
_Static_assert(offsetof(TEB, HostGsBase) == TEB_GSBASE, "HostGsBase offset mismatch");
#endif
#ifdef TEB_HAS_FSGSBASE
_Static_assert(offsetof(TEB, HasFsGsBase) == TEB_HAS_FSGSBASE, "HasFsGsBase offset mismatch");
#endif
#ifdef TEB_HOST_CS_SEL
_Static_assert(offsetof(TEB, HostCodeSelector) == TEB_HOST_CS_SEL, "HostCodeSelector offset mismatch");
#endif
#endif

typedef struct _MEMORY_BASIC_INFORMATION {
	GUEST_PTR BaseAddress;
	GUEST_PTR AllocationBase;
	DWORD AllocationProtect;
	SIZE_T RegionSize;
	DWORD State;
	DWORD Protect;
	DWORD Type;
} MEMORY_BASIC_INFORMATION, *PMEMORY_BASIC_INFORMATION;

#define _IOB_ENTRIES 20

typedef struct _iobuf {
	GUEST_PTR _ptr;
	int _cnt;
	GUEST_PTR _base;
	int _flag;
	int _file;
	int _charbuf;
	int _bufsiz;
	GUEST_PTR _tmpfname;
} _FILE;
// Designated initializer for a _FILE with the given descriptor (C++ default-constructed files use -1).
#define _FILE_INIT(fd) {._file = (fd)}
