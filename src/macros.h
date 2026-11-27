#pragma once

#define TEB_SELF 0x18	 // Self
#define TEB_FS_SEL 0xf98 // CurrentFsSelector
#define TEB_GS_SEL 0xf9a // CurrentGsSelector

#ifdef __i386__

#define TEB_SP 0xf9c // CurrentStackPointer
#define TEB_BOUNDARY 0xfa0 // GuestBoundary
#define TEB_GUEST_FRAME 0xfa4 // GuestCallFrame

#endif // __i386__

#ifdef __x86_64__

#define TEB_CS_SEL 0xf9c	   // CodeSelector
#define TEB_DS_SEL 0xf9e	   // DataSelector
#define TEB_SP 0xfa0		   // CurrentStackPointer
#define TEB_FSBASE 0xfa8	   // HostFsBase
#define TEB_GSBASE 0xfb0	   // HostGsBase
#define TEB_HAS_FSGSBASE 0xfb8 // HasFsGsBase
#define TEB_HOST_CS_SEL 0xfba  // HostCodeSelector
#define TEB_BOUNDARY 0xfc0	   // GuestBoundary
#ifdef WIBO_GUEST64
#define TEB_GUEST_FRAME 0x17b0 // GuestCallFrame; the Win64 TEB layout moves the wibo fields
#else
#define TEB_GUEST_FRAME 0xfc8  // GuestCallFrame
#endif

#endif

#if defined(__linux__)
#define GNU_ASSEMBLER 1
#elif defined(__clang__)
#define GNU_ASSEMBLER 0
#else
#error "Unsupported platform"
#endif

#ifndef __USER_LABEL_PREFIX__
#define __USER_LABEL_PREFIX__
#endif

#define GLUE2_(a, b) a##b
#define GLUE(a, b) GLUE2_(a, b)
#define SYMBOL_NAME(name) GLUE(__USER_LABEL_PREFIX__, name)

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

#define STR_(S) #S
#define STR(S) STR_(S)
#define SYMBOL_NAME_STR_(name) STR(__USER_LABEL_PREFIX__) #name
#define SYMBOL_NAME_STR(name) SYMBOL_NAME_STR_(name)

#if __SIZEOF_POINTER__ == 8
#define ASM_SIZE_TYPE "quad"
#else
#define ASM_SIZE_TYPE "long"
#endif
// .pushsection/.popsection so that compiler output after the blob stays in .text.
#if __ELF__
#define ASM_RODATA_SECTION ".pushsection .rodata"
#else
#define ASM_RODATA_SECTION ".pushsection __TEXT, __const"
#endif

#define INCLUDE_BIN(symbol, file)                                 \
    asm(ASM_RODATA_SECTION "\n"                                   \
        ".globl " SYMBOL_NAME_STR(symbol) "\n"                    \
        SYMBOL_NAME_STR(symbol) ":\n"                             \
        ".incbin \"" file "\"\n"                                  \
        ".globl " SYMBOL_NAME_STR(GLUE(symbol, End)) "\n"         \
        SYMBOL_NAME_STR(GLUE(symbol, End)) ":\n"                  \
        ".popsection\n");                                         \
    extern const uint8_t symbol[];                                \
    extern const uint8_t GLUE(symbol, End)[];
#define INCLUDE_BIN_SIZE(symbol) ((size_t)((uintptr_t)&GLUE(symbol, End) - (uintptr_t)&symbol))
#endif
