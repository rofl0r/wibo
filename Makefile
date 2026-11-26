# wibo build system (GNU Make). See README.md for details.
#
#   make                build ./wibo32 (PE32 guests) and, when CC targets x86_64, ./wibo64 (PE32+ guests)
#   make wibo32         build ./wibo32 (PE32 guests)
#   make wibo64         build ./wibo64 (PE32+ guests; needs an x86_64 CC)
#   make DEBUG=1        build with -O0 -g3
#   make test [TEST=wibo.<name>]   build the fixtures and run them under each runtime
#   make test-bundle    run the --bundle checks under each runtime
#
# Bundles: `./wibo32 --bundle -o app app.exe lib.dll` writes a standalone executable (see README.md).
#
# Variables (command line or environment):
#   CC                     target C compiler (default gcc). Its predefined macros select the architecture
#                          (__x86_64__ or __i386__) and the OS (__linux__ or __APPLE__). A 32-bit build needs a
#                          32-bit compiler, e.g. make CC="gcc -m32" (multilib) or a cross compiler. No -m32 is
#                          added implicitly. On x86_64 hosts the default wibo32 is a 64-bit binary that runs PE32
#                          guests in-process.
#   HOSTCC                 compiler for host tools such as gentramp (default: CC)
#   DEBUG                  1 builds with -O0 -g3; 0 builds with -O3 -DNDEBUG (default 0)
#   WIBO_VERSION           version string (default: git describe, else "unknown")
#   WIBO_ENABLE_LIBURING   1 enables the io_uring backend on Linux (default 0)
#   WIBO_LIBURING_STATIC   1 links liburing statically (default 0)
#   WIBO_ENABLE_WINE_DLLS  1 embeds the Wine CRT DLLs other than msvcrt in wibo32 (default 0).
#                          msvcrt itself is always the built-in dll/msvcrt.c.
#   MINGW32_CC, MINGW32_WINDRES  fixture toolchain for wibo32 (default i686-w64-mingw32-gcc, -windres)
#   MINGW64_CC, MINGW64_WINDRES  fixture toolchain for wibo64 (default x86_64-w64-mingw32-gcc, -windres)
#   TEST_TIMEOUT           seconds allowed per fixture test (default 1500)
#   CFLAGS, LDFLAGS        passed to the wibo compile and link steps
#   EXTRA_CFLAGS, EXTRA_LDFLAGS, EXTRA_LIBS  appended to the wibo build flags
#
# BUILD_VARIANT is internal: the top-level makefile sets it to re-invoke itself for each runtime.

.DEFAULT_GOAL := all

ifeq ($(origin CC),default)
CC := gcc
endif
DEBUG ?= 0
ifneq ($(filter-out 0 1,$(DEBUG)),)
$(error DEBUG must be 0 or 1, got '$(DEBUG)')
endif

# The target architecture is whatever CC generates. Predefined macros are the source of truth, so the
# Makefile and the C sources (which test __x86_64__ / __i386__) cannot disagree.
CC_PREDEFINES := $(shell echo | $(CC) -dM -E - 2>/dev/null)
ifeq ($(CC_PREDEFINES),)
$(error CC='$(CC)' did not run)
endif
ifneq ($(findstring __x86_64__,$(CC_PREDEFINES)),)
TARGET_ARCH := x86_64
else ifneq ($(findstring __i386__,$(CC_PREDEFINES)),)
TARGET_ARCH := i686
else
$(error CC='$(CC)' targets neither x86 nor x86_64)
endif

ifneq ($(findstring __APPLE__,$(CC_PREDEFINES)),)
TARGET_OS := Darwin
else ifneq ($(findstring __linux__,$(CC_PREDEFINES)),)
TARGET_OS := Linux
else
$(error CC='$(CC)' targets neither Linux nor Darwin)
endif

WIBO_ENABLE_LIBURING ?= 0
WIBO_LIBURING_STATIC ?= 0
WIBO_ENABLE_WINE_DLLS ?= 0
TEST_TIMEOUT ?= 1500
HOSTCC ?= $(CC)
MINGW32_CC ?= i686-w64-mingw32-gcc
MINGW32_WINDRES ?= i686-w64-mingw32-windres
MINGW64_CC ?= x86_64-w64-mingw32-gcc
MINGW64_WINDRES ?= x86_64-w64-mingw32-windres

# wibo64 is only a candidate when CC generates x86_64 code; wibo32 is always built.
RUNTIMES := wibo32 $(if $(filter x86_64,$(TARGET_ARCH)),wibo64)

BUILD_ROOT := build
BUILD_DIR := $(BUILD_ROOT)/$(BUILD_VARIANT)-$(TARGET_ARCH)
OBJ_DIR := $(BUILD_DIR)/obj
GEN_DIR := $(BUILD_DIR)/generated
TEST_BIN_DIR := $(BUILD_DIR)/test
VENDOR_DIR := $(BUILD_ROOT)/vendor
VENDOR_ABS := $(abspath $(VENDOR_DIR))
# The runtime executable lives in the top-level directory; only its object files go under BUILD_DIR.
WIBO_NAME := $(BUILD_VARIANT)
WIBO := $(CURDIR)/$(WIBO_NAME)
TOOLS_DIR := $(BUILD_DIR)/tools
GENTRAMP := $(TOOLS_DIR)/gentramp

GENTRAMP_ARCH := $(if $(filter x86_64,$(TARGET_ARCH)),x86_64,x86)

ifneq ($(BUILD_VARIANT),)
ifeq ($(filter $(BUILD_VARIANT),$(RUNTIMES)),)
$(error runtime '$(BUILD_VARIANT)' cannot be built with CC='$(CC)' (targets $(TARGET_ARCH)))
endif
GUEST64 := $(if $(filter wibo64,$(BUILD_VARIANT)),1,0)
MINGW_CC := $(if $(filter 1,$(GUEST64)),$(MINGW64_CC),$(MINGW32_CC))
MINGW_WINDRES := $(if $(filter 1,$(GUEST64)),$(MINGW64_WINDRES),$(MINGW32_WINDRES))

ifeq ($(DEBUG),1)
OPT_FLAGS := -O0 -g3
else
OPT_FLAGS := -O3 -DNDEBUG
endif

# -maccumulate-outgoing-args is GCC-only; CMake applied it only to GNU compilers.
CC_IS_CLANG := $(shell echo | $(CC) -dM -E - 2>/dev/null | grep -c __clang__)
ifeq ($(CC_IS_CLANG),0)
GNU_ONLY_FLAGS := -maccumulate-outgoing-args
endif

ifeq ($(TARGET_OS),Darwin)
PLATFORM_SRCS := src/async_io_kqueue.c src/processes_darwin.c src/setup_darwin.c
PLATFORM_LDFLAGS := -Wl,-no_pie,-no_fixup_chains -Wl,-segalign,0x1000 -Wl,-pagezero_size,0x1000 \
	-Wl,-image_base,0x7E001000 -Wl,-segaddr,RESV32,0x1000 -Wl,-segprot,RESV32,-,-
PLATFORM_LIBS :=
else
PLATFORM_SRCS := src/async_io_epoll.c src/processes_linux.c $(if $(filter 1,$(GUEST64)),src/setup_linux64.c,src/setup_linux.c)
# GNU ld on ELF rejects --image-base; -Ttext-segment places the image at the same address.
PLATFORM_LDFLAGS := -no-pie -Wl,-Ttext-segment=0x70000000
PLATFORM_LIBS := -latomic
endif

ifeq ($(WIBO_ENABLE_LIBURING)-$(TARGET_OS),1-Linux)
ENABLE_URING := 1
URING_SRCS := src/async_io_uring.c
ifeq ($(WIBO_LIBURING_STATIC),1)
URING_LIBS := -Wl,-Bstatic -luring -Wl,-Bdynamic
else
URING_LIBS := -luring
endif
else
ENABLE_URING := 0
endif

HAVE_GETTID := $(shell printf '%s\n' '#include <pthread.h>' \
	'int main(void) { return (int)(long)(void *)pthread_gettid_np; }' \
	| $(CC) -D_GNU_SOURCE -x c - -o /dev/null -lpthread >/dev/null 2>&1 && echo 1)
ifeq ($(HAVE_GETTID),1)
GETTID_DEFS := -DHAVE_PTHREAD_GETTID_NP=1
endif

WIBO_VERSION ?=
ifeq ($(WIBO_VERSION),)
WIBO_VERSION := $(shell git describe --tags --dirty --always 2>/dev/null)
endif
ifeq ($(WIBO_VERSION),)
WIBO_VERSION := unknown
endif
SHA256SUM := $(shell command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo "shasum -a 256")

CORE_SRCS := \
	src/access.c \
	src/async_io.c \
	src/bundle.c \
	src/cutil.c \
	src/errors.c \
	src/files.c \
	src/handles.c \
	src/heap.c \
	src/loader.c \
	src/main.c \
	src/modules.c \
	src/processes_common.c \
	src/resources.c \
	src/setup.S \
	src/strutil.c \
	src/tls.c

KERNEL32_NAMES := debugapi errhandlingapi fibersapi fileapi handleapi heapapi interlockedapi \
	ioapiset libloaderapi memoryapi namedpipeapi processenv processthreadsapi profileapi \
	stringapiset synchapi sysinfoapi timezoneapi winbase wincon winnls winnt wow64apiset
ADVAPI32_NAMES := processthreadsapi securitybaseapi winbase wincrypt winreg

DLL_SRCS := \
	dll/advapi32.c \
	dll/advapi32/md5.c \
	$(addprefix dll/advapi32/,$(addsuffix .c,$(ADVAPI32_NAMES))) \
	dll/bcrypt.c \
	dll/dbghelp.c \
	dll/kernel32.c \
	$(addprefix dll/kernel32/,$(addsuffix .c,$(KERNEL32_NAMES))) \
	dll/lmgr.c \
	dll/mscoree.c \
	dll/ntdll.c \
	dll/ole32.c \
	dll/oleaut32.c \
	dll/rpcrt4.c \
	dll/shlwapi.c \
	dll/user32.c \
	dll/vcruntime.c \
	dll/version.c \
	dll/ws2.c

WINE_DLLS := crtdll msvcirt msvcrt20 msvcrt40 msvcr71 msvcr90 msvcr100 msvcr120 ucrtbase
WINEDLL_RELEASE_TAG := 2026-09-29
WINEDLL_URL := https://github.com/encounter/winedll/releases/download/$(WINEDLL_RELEASE_TAG)
SHA256_crtdll := 3caf465b10ae4cb2127bc535e91a39c644b605253130d523fbf122760f422c0b
SHA256_msvcirt := 2086a637fb2aecb754510761f0dd04b79694e10526c4f8964e262c8cccca9db4
SHA256_msvcrt20 := f603f5e4c20e54f2475c940199064f2f81fac5f3ae5e1b7a73bd130691a1e78c
SHA256_msvcrt40 := d37ea23b4c36b2377817617d2a93fb53bd3bfde40a5c0ecec502b623439fe50e
SHA256_msvcr71 := 3f56fd7107c45626c9e6904e6bea7e832933ea4879a743195e7fa09ab085ddac
SHA256_msvcr90 := 9294a314266a6a7cd828f6d3d84b956283e1db13830b8fb27fd5429cbd966f29
SHA256_msvcr100 := 624d89f0cfb67d9153582910e81d3748971959a16d3872e4503afd7708912692
SHA256_msvcr120 := f1dd6500a7aa4cf58c3c17f3d3b091f00138f7b465c363efef4bf20119a1bcaf
SHA256_ucrtbase := 1d5e5bbf3a766e031d7d208659c097c944985cbcbc2725d3f926f220d6e22c16

# msvcrt is always the built-in subset (dll/msvcrt.c) for both runtimes. The other Wine CRT DLLs are embedded
# only in wibo32 with WIBO_ENABLE_WINE_DLLS=1; wibo64 skips them because the PE32 builds fail its bitness guard.
NATIVE_CRT_DLLS := msvcrt
ifeq ($(GUEST64),1)
EMBEDDED_WINE_DLLS :=
else ifeq ($(WIBO_ENABLE_WINE_DLLS),1)
EMBEDDED_WINE_DLLS := $(WINE_DLLS)
else
EMBEDDED_WINE_DLLS :=
endif
EMBED_DLL_SRCS := $(addprefix dll/,$(addsuffix .c,$(EMBEDDED_WINE_DLLS)))

# Every WIBO_HAS_* flag is defined for all sources, matching CMake's target-wide definitions.
WINE_DLL_FLAG_NAMES := crtdll:CRTDLL msvcrt:MSVCRT msvcirt:MSVCIRT msvcrt20:MSVCRT20 \
	msvcrt40:MSVCRT40 msvcr71:MSVCR71 msvcr90:MSVCR90 msvcr100:MSVCR100 msvcr120:MSVCR120 \
	ucrtbase:UCRTBASE
WINE_DLL_DEFS := $(foreach p,$(WINE_DLL_FLAG_NAMES),-DWIBO_HAS_$(word 2,$(subst :, ,$(p)))=$(if $(filter $(word 1,$(subst :, ,$(p))),$(EMBEDDED_WINE_DLLS) $(NATIVE_CRT_DLLS)),1,0))
DLL_SRCS += dll/msvcrt.c

ALL_TRAMP_MODULES := advapi32 bcrypt dbghelp entry mscoree version rpcrt4 shlwapi ws2 vcruntime lmgr \
	ole32 oleaut32 user32 ntdll kernel32
ALL_TRAMP_MODULES += msvcrt
# One thunk set per build: wibo64 generates Win64-ABI thunks, wibo32 generates the guest-ABI set.
TRAMP_MODULES := $(ALL_TRAMP_MODULES)
ifeq ($(GUEST64),1)
TRAMP_ARCH_ARGS := --arch x86_64 --guest-abi win64
else
TRAMP_ARCH_ARGS := --arch $(GENTRAMP_ARCH)
endif
TRAMP_HDRS_advapi32 := $(addprefix dll/advapi32/,$(addsuffix .h,$(ADVAPI32_NAMES)))
TRAMP_HDRS_bcrypt := dll/bcrypt.h
TRAMP_HDRS_dbghelp := dll/dbghelp.h
TRAMP_HDRS_entry := src/entry.h
TRAMP_HDRS_mscoree := dll/mscoree.h
TRAMP_HDRS_version := dll/version.h
TRAMP_HDRS_rpcrt4 := dll/rpcrt4.h
TRAMP_HDRS_shlwapi := dll/shlwapi.h
TRAMP_HDRS_ws2 := dll/ws2.h
TRAMP_HDRS_vcruntime := dll/vcruntime.h
TRAMP_HDRS_lmgr := dll/lmgr.h
TRAMP_HDRS_ole32 := dll/ole32.h
TRAMP_HDRS_oleaut32 := dll/oleaut32.h
TRAMP_HDRS_user32 := dll/user32.h
TRAMP_HDRS_ntdll := dll/ntdll.h
TRAMP_HDRS_kernel32 := $(addprefix dll/kernel32/,$(addsuffix .h,$(KERNEL32_NAMES)))
TRAMP_HDRS_msvcrt := dll/msvcrt.h
TRAMP_STAMPS := $(foreach m,$(TRAMP_MODULES),$(GEN_DIR)/$(m)_trampolines.stamp)

# gentramp writes the .S and .h pair; a stamp file records the run so parallel builds never invoke it twice.
define TRAMPOLINE_RULE
$$(GEN_DIR)/$(1)_trampolines.stamp: $$(GENTRAMP) $$(TRAMP_HDRS_$(1)) | $$(GEN_DIR)
	$$(GENTRAMP) --dll $(1) --headers $$(TRAMP_HDRS_$(1)) $$(TRAMP_ARCH_ARGS) \
		--out-asm $$(GEN_DIR)/$(1)_trampolines.S --out-hdr $$(GEN_DIR)/$(1)_trampolines.h \
		-I . -I dll -I src -D WIBO_CODEGEN=1 --cc "$$(CC) $$(GUEST64_DEFS)"
	@touch $$@
endef
$(foreach m,$(TRAMP_MODULES),$(eval $(call TRAMPOLINE_RULE,$(m))))

TRAMP_OBJS := $(addprefix $(OBJ_DIR)/generated/,$(addsuffix _trampolines.S.o,$(TRAMP_MODULES)))

WIBO_SRCS := $(CORE_SRCS) $(DLL_SRCS) $(EMBED_DLL_SRCS) $(PLATFORM_SRCS) $(URING_SRCS)
WIBO_OBJS := $(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(WIBO_SRCS))) $(TRAMP_OBJS)
WIBO_DEPS := $(WIBO_OBJS:.o=.d)

# -std is C-only; the assembler sources reuse the same flags without it.
GUEST64_DEFS := $(if $(filter 1,$(GUEST64)),-DWIBO_GUEST64=1)
# User CFLAGS follow OPT_FLAGS so they can override the optimization level.
WIBO_COMMON_FLAGS := -Wall -Wextra $(OPT_FLAGS) $(CFLAGS) \
	$(GNU_ONLY_FLAGS) $(WINE_DLL_DEFS) $(GETTID_DEFS) $(GUEST64_DEFS) -DWIBO_ENABLE_LIBURING=$(ENABLE_URING) \
	-D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 \
	-I dll -I src -I $(GEN_DIR) -MMD -MP $(EXTRA_CFLAGS)
WIBO_CFLAGS := $(WIBO_COMMON_FLAGS)
WIBO_CSTD := -std=gnu11
WIBO_LDFLAGS := $(GNU_ONLY_FLAGS) $(LDFLAGS) $(PLATFORM_LDFLAGS) $(EXTRA_LDFLAGS)
WIBO_LIBS := $(PLATFORM_LIBS) $(URING_LIBS) $(EXTRA_LIBS)

# Objects are rebuilt when the compile flags change (e.g. after toggling DEBUG or CC); the stamp only
# changes mtime when its content does.
CFLAGS_STAMP := $(BUILD_DIR)/cflags.stamp
$(CFLAGS_STAMP): FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(CC) $(WIBO_CFLAGS) $(WIBO_CSTD)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

# The link is redone when the linker command or the object directory changes. The stamp is keyed by the executable
# because a CC switch (e.g. CC="gcc -m32") moves the objects to another build directory but writes the same output.
LINK_STAMP := $(BUILD_ROOT)/$(WIBO_NAME).link.stamp
$(LINK_STAMP): FORCE | $(BUILD_DIR)
	@printf '%s\n' '$(CC) $(WIBO_LDFLAGS) $(WIBO_LIBS) $(OBJ_DIR)' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

define EMBED_RULE
$$(OBJ_DIR)/dll/$(1).c.o: OBJ_CFLAGS := -DEMBED_PATH='"$$(VENDOR_ABS)/$(1).dll"'
$$(OBJ_DIR)/dll/$(1).c.o: $$(VENDOR_DIR)/$(1).dll
endef
$(foreach d,$(EMBEDDED_WINE_DLLS),$(eval $(call EMBED_RULE,$(d))))

ifeq ($(TARGET_OS),Darwin)
$(OBJ_DIR)/src/setup_darwin.c.o: OBJ_CFLAGS += -fms-extensions
endif

.PHONY: check-mingw gentramp fixtures test test-bundle list-tests FORCE

$(WIBO): $(WIBO_OBJS) $(LINK_STAMP) | $(BUILD_DIR)
	$(CC) $(WIBO_LDFLAGS) -o $@ $(WIBO_OBJS) $(WIBO_LIBS)

$(OBJ_DIR)/%.c.o: %.c $(CFLAGS_STAMP) | $(TRAMP_STAMPS) $(GEN_DIR)/version_info.h
	@mkdir -p $(@D)
	$(CC) $(WIBO_CFLAGS) $(WIBO_CSTD) $(OBJ_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.S.o: %.S $(CFLAGS_STAMP)
	@mkdir -p $(@D)
	$(CC) $(WIBO_COMMON_FLAGS) $(OBJ_CFLAGS) -c $< -o $@

$(TRAMP_OBJS): $(OBJ_DIR)/generated/%_trampolines.S.o: $(GEN_DIR)/%_trampolines.stamp $(CFLAGS_STAMP)
	@mkdir -p $(@D)
	$(CC) $(WIBO_COMMON_FLAGS) -c $(GEN_DIR)/$*_trampolines.S -o $@


$(GEN_DIR)/version_info.h: src/version_info.h.in FORCE | $(GEN_DIR)
	sed -e 's|@WIBO_VERSION_STRING@|$(WIBO_VERSION)|' \
		-e 's|@WIBO_SYSTEM_NAME@|$(TARGET_OS)|' \
		-e 's|@WIBO_SYSTEM_PROCESSOR@|$(TARGET_ARCH)|' $< > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(GENTRAMP): tools/gentramp.c | $(TOOLS_DIR)
	$(HOSTCC) -std=gnu11 -O2 -Wall -Wextra -o $@ $<

gentramp: $(GENTRAMP)


$(VENDOR_DIR)/%.dll:
	@mkdir -p $(@D)
	curl -fsSL -o $@.part $(WINEDLL_URL)/$*.dll
	@if ! echo "$(SHA256_$*)  $@.part" | $(SHA256SUM) -c - >/dev/null 2>&1; then \
		rm -f $@.part; echo "error: SHA256 mismatch for $*.dll" >&2; exit 1; fi
	mv $@.part $@

FIXTURE_NAMES := test_external_dll test_dll_attach_failure test_thread_notifications \
	test_bcrypt test_shlwapi test_oleaut32 test_ws2 test_charupperbuff test_charlowerbuff \
	test_resources test_threading test_temppath test_outputdebugstring test_fullpath \
	test_gettempfilename test_getprocaddress test_getmodulehandleex test_tls test_tls_reloc \
	test_fls test_fpu_state test_handleapi test_findfile test_getfileattributesex \
	test_movefileex test_locale test_lstrcpyn test_critical_section test_synchapi \
	test_processes test_heap test_heapvalidate test_actctx test_overlapped_io test_time \
	test_ntdll_time test_virtualalloc test_virtualquery test_createfilemapping test_clsids \
	test_rtl test_rtl_bitmap test_ntquery test_ntqueryfile test_ntreadfile test_ntwritefile \
	test_pipe_io test_namedpipe test_sysdir test_stringapiset test_srw_lock test_init_once \
	test_init_once_execute test_gettickcount64 test_slist_interlocked test_locale_ex \
	test_sync_ex test_wait_on_address test_env_strings test_msvcrt_printf test_msvcrt_stdio \
	test_unhandled_exception test_msvcrt_mem test_dbghelp test_msvcrt_errno_locale test_msvcrt_putenv
FIXTURE_DLL_NAMES := external_exports dll_attach_failure thread_notifications tls_reloc

TEST_LIBS_test_bcrypt := -lbcrypt
TEST_LIBS_test_shlwapi := -lshlwapi
TEST_LIBS_test_oleaut32 := -loleaut32
TEST_LIBS_test_ws2 := -lws2_32
TEST_LIBS_test_charupperbuff := -luser32
TEST_LIBS_test_charlowerbuff := -luser32
TEST_LIBS_test_clsids := -lole32
TEST_LIBS_test_wait_on_address := -lsynchronization
TEST_LIBS_test_resources := -lversion
TEST_OBJS_test_resources := $(TEST_BIN_DIR)/test_resources_res.o
TEST_DLL_SRC_tls_reloc := test/tls_reloc_dll.c

$(foreach t,$(FIXTURE_NAMES),$(eval TEST_SRC_$(t) ?= test/$(t).c))
$(foreach d,$(FIXTURE_DLL_NAMES),$(eval TEST_DLL_SRC_$(d) ?= test/$(d).c))

define FIXTURE_EXE_RULE
$$(TEST_BIN_DIR)/$(1).exe: $$(TEST_SRC_$(1)) $$(TEST_OBJS_$(1)) | $$(TEST_BIN_DIR)
	$$(MINGW_CC) -Wall -Wextra -O2 -I test -o $$@ $$(TEST_SRC_$(1)) $$(TEST_OBJS_$(1)) $$(TEST_LIBS_$(1))
endef
define FIXTURE_DLL_RULE
$$(TEST_BIN_DIR)/$(1).dll: $$(TEST_DLL_SRC_$(1)) | $$(TEST_BIN_DIR)
	$$(MINGW_CC) -Wall -Wextra -O2 -shared -I test -o $$@ $$(TEST_DLL_SRC_$(1))
endef
$(foreach t,$(FIXTURE_NAMES),$(eval $(call FIXTURE_EXE_RULE,$(t))))
$(foreach d,$(FIXTURE_DLL_NAMES),$(eval $(call FIXTURE_DLL_RULE,$(d))))

$(TEST_BIN_DIR)/test_resources_res.o: test/test_resources.rc | $(TEST_BIN_DIR)
	cd $(TEST_BIN_DIR) && $(MINGW_WINDRES) $(abspath test/test_resources.rc) -O coff -o test_resources_res.o

FIXTURE_EXE_OUTS := $(addprefix $(TEST_BIN_DIR)/,$(addsuffix .exe,$(FIXTURE_NAMES)))
FIXTURE_DLL_OUTS := $(addprefix $(TEST_BIN_DIR)/,$(addsuffix .dll,$(FIXTURE_DLL_NAMES)))
FIXTURE_OUTS := $(FIXTURE_EXE_OUTS) $(FIXTURE_DLL_OUTS)
$(FIXTURE_OUTS): | check-mingw

TEST ?=
ifneq ($(TEST),)
TEST_SELECTED := $(patsubst wibo.%,%,$(TEST))
$(foreach t,$(TEST_SELECTED),$(if $(filter $(t),$(FIXTURE_NAMES)),,$(error Unknown test '$(t)'; run 'make list-tests')))
else
TEST_SELECTED := $(FIXTURE_NAMES)
endif
# DLL fixtures are always built: the selected executable may load any of them.
TEST_OUTS := $(addprefix $(TEST_BIN_DIR)/,$(addsuffix .exe,$(TEST_SELECTED))) $(FIXTURE_DLL_OUTS)
TEST_TIMEOUT_CMD := $(if $(shell command -v timeout 2>/dev/null),timeout $(TEST_TIMEOUT))

check-mingw:
	@if ! command -v $(MINGW_CC) >/dev/null 2>&1 || ! command -v $(MINGW_WINDRES) >/dev/null 2>&1; then \
		echo "error: $(MINGW_CC) and $(MINGW_WINDRES) are required for fixture tests" >&2; exit 1; fi

fixtures: $(FIXTURE_OUTS)

test: $(WIBO) $(TEST_OUTS)
	@fail=0; \
	for t in $(TEST_SELECTED); do \
		echo "[ RUN  ] wibo.$$t"; \
		if (cd test && $(TEST_TIMEOUT_CMD) $(abspath $(WIBO)) $(abspath $(TEST_BIN_DIR))/$$t.exe); then \
			echo "[  OK  ] wibo.$$t"; \
		else \
			echo "[ FAIL ] wibo.$$t"; fail=1; \
		fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "error: fixture test(s) failed" >&2; exit 1; fi

# Bundle checks use the fixtures of this build. Set BUNDLE_RUNNER=qemu-i386 to run the 32-bit stub on a 64-bit host.
test-bundle: $(WIBO) $(FIXTURE_OUTS) $(FIXTURE_DLL_OUTS)
	BUNDLE_RUNNER="$(BUNDLE_RUNNER)" sh test/bundle_test.sh $(abspath $(WIBO)) $(abspath $(TEST_BIN_DIR))

list-tests:
	@for t in $(FIXTURE_NAMES); do echo wibo.$$t; done

$(BUILD_DIR) $(GEN_DIR) $(TEST_BIN_DIR) $(TOOLS_DIR) $(VENDOR_DIR):
	@mkdir -p $@

FORCE:

-include $(WIBO_DEPS)

endif

# Top level: each runtime is built and tested by a sub-make with BUILD_VARIANT set.
ifeq ($(BUILD_VARIANT),)
.DEFAULT_GOAL := all
.PHONY: all $(RUNTIMES) test test-bundle fixtures list-tests print-wibo clean distclean FORCE

all: $(RUNTIMES)

$(RUNTIMES): FORCE
	@$(MAKE) --no-print-directory BUILD_VARIANT=$@ $(CURDIR)/$@

ifeq ($(filter wibo64,$(RUNTIMES)),)
wibo64:
	@echo "error: wibo64 needs a C compiler that targets x86_64 (CC='$(CC)' targets $(TARGET_ARCH))" >&2; exit 1
endif

test: $(addsuffix .test,$(RUNTIMES))
%.test: FORCE
	@$(MAKE) --no-print-directory BUILD_VARIANT=$* test

test-bundle: $(addsuffix .test-bundle,$(RUNTIMES))
%.test-bundle: FORCE
	@$(MAKE) --no-print-directory BUILD_VARIANT=$* test-bundle

fixtures: $(addsuffix .fixtures,$(RUNTIMES))
%.fixtures: FORCE
	@$(MAKE) --no-print-directory BUILD_VARIANT=$* fixtures

list-tests:
	@$(MAKE) --no-print-directory BUILD_VARIANT=$(firstword $(RUNTIMES)) $@

print-wibo:
	@for r in $(RUNTIMES); do echo $(CURDIR)/$$r; done

clean:
	rm -rf $(BUILD_ROOT)/wibo32-* $(BUILD_ROOT)/wibo64-* wibo32 wibo64

distclean:
	rm -rf $(BUILD_ROOT) wibo32 wibo64

FORCE:
endif
