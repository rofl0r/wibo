#include "setup.h"

#include "common.h"
#include "types.h"

#include <asm/prctl.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

// Win64 guests address the TEB through gs (gs:0x30 is NT_TIB.Self); glibc keeps fs for its own TLS.
bool tebThreadSetup(TEB *teb) {
	if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)(uintptr_t)teb) != 0) {
		DEBUG_LOG("setup_linux64: arch_prctl(ARCH_SET_GS) failed: %s\n", strerror(errno));
		return false;
	}
	return true;
}

bool tebThreadTeardown(TEB *teb) {
	(void)teb;
	return true;
}
