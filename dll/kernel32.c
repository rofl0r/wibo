#include "kernel32.h"

#include "kernel32/internal.h"
#include "modules.h"

#include <stdio.h>
#include <unistd.h>

void FsObject_free(FsObject *self) {
	int fd = self->fd;
	self->fd = -1;
	if (fd >= 0 && self->closeOnDestroy) {
		close(fd);
	}
	if (self->deletePending && self->canonicalPath) {
		if (unlink(self->canonicalPath) != 0) {
			perror("Failed to delete file on close");
		}
	}
	free(self->canonicalPath);
	self->canonicalPath = NULL;
	pthread_mutex_destroy(&self->m);
}

const ModuleStub lib_kernel32 = {
	.names =
		(const char *[]){
			"kernel32",
			"kernelbase",
			NULL,
		},
	.byName = kernel32ThunkByName,
};
