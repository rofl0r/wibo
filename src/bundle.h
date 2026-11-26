#ifndef WIBO_BUNDLE_H
#define WIBO_BUNDLE_H

#include <stdbool.h>
#include <stdio.h>

// Argument a bundled wibo passes to the children it spawns, so they do not prepend the bundle's main executable.
#define WIBO_BUNDLE_CHILD_ARG "--bundle-child"

// Detects an embedded pak (linked-in payload first, then a trailer appended to /proc/self/exe). Call once at startup.
void wibo_bundleInit(void);
bool wibo_bundleActive(void);

// Returns a malloc'd host path to the bundle's main executable. Requires an active bundle.
char *wibo_bundleMainPath(void);

// Returns the stored name of an embedded file that lives in dir, or NULL.
const char *wibo_bundleFindName(const char *dir, const char *name);

// fopen(path, "rb") that serves embedded files from memory.
FILE *wibo_openImage(const char *path);

#endif
