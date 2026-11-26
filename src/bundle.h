#ifndef WIBO_BUNDLE_H
#define WIBO_BUNDLE_H

#include <stdbool.h>
#include <stdio.h>

// Argument a bundled wibo passes to the children it spawns, so they do not prepend the bundle's main executable.
#define WIBO_BUNDLE_CHILD_ARG "--bundle-child"

// Detects a bundle trailer appended to the executable argv0 names. Call once at startup.
void wibo_bundleInit(const char *argv0);
bool wibo_bundleActive(void);

// Returns a malloc'd host path to the bundle's main executable. Requires an active bundle.
char *wibo_bundleMainPath(void);

// Returns the stored name of an embedded file that lives in dir, or NULL.
const char *wibo_bundleFindName(const char *dir, const char *name);

// fopen(path, "rb") that serves bundled files from memory.
FILE *wibo_openImage(const char *path);

// Handles `wibo --bundle -o OUT FILE...`: writes a standalone executable to OUT. Returns the process exit status.
int wibo_bundleMain(int argc, char **argv, const char *argv0);

#endif
