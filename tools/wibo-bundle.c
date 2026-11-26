// wibo-bundle: packs a Win64 executable and its DLLs into a single Linux executable or a pak file.
//
//   wibo-bundle -o OUT --stub build/release/wibo main.exe [dll...]   writes a self-contained executable
//   wibo-bundle --pak -o OUT main.exe [dll...]                       writes only the pak (for make WIBO_BUNDLE_FILES=...)
//
// The first input is the program that runs; the others are served to it as if they sat next to it.

#include "bundle_format.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

typedef struct {
	const char *path;
	const char *name;
	uint8_t *data;
	size_t size;
} Input;

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static void die(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "wibo-bundle: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	exit(1);
}

static void usage(void) {
	fprintf(stderr, "usage: wibo-bundle -o OUT --stub STUB main.exe [dll...]\n"
					"       wibo-bundle --pak -o OUT main.exe [dll...]\n");
	exit(2);
}

static void *xalloc(size_t n) {
	void *p = malloc(n ? n : 1);
	if (!p) {
		die("out of memory");
	}
	return p;
}

static uint8_t *readFile(const char *path, size_t *sizeOut) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		die("cannot open %s: %s", path, strerror(errno));
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		die("cannot seek %s", path);
	}
	long len = ftell(f);
	if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
		die("cannot size %s", path);
	}
	uint8_t *buf = xalloc((size_t)len);
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
		die("cannot read %s", path);
	}
	fclose(f);
	*sizeOut = (size_t)len;
	return buf;
}

static const char *baseName(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static void validateInput(Input *in) {
	in->name = baseName(in->path);
	size_t nameLen = strlen(in->name);
	if (nameLen == 0 || nameLen > WIBO_PAK_MAX_NAME_LEN || strchr(in->name, '\\')) {
		die("invalid file name for %s (must be 1-255 bytes with no '\\')", in->path);
	}
	in->data = readFile(in->path, &in->size);
	if (in->size == 0) {
		die("%s is empty", in->path);
	}
	if (in->size < 2 || in->data[0] != 'M' || in->data[1] != 'Z') {
		die("%s is not a PE image (missing MZ header)", in->path);
	}
}

static size_t alignUp(size_t v, size_t a) {
	return (v + a - 1) & ~(a - 1);
}

// Builds the pak in memory. Data blocks are 16-byte aligned so they are aligned when mapped.
static uint8_t *buildPak(Input *inputs, size_t count, size_t *sizeOut) {
	size_t headerEnd = WIBO_PAK_HEADER_SIZE;
	for (size_t i = 0; i < count; i++) {
		headerEnd += 4 + strlen(inputs[i].name) + 16;
	}
	size_t total = headerEnd;
	uint64_t *offsets = xalloc(count * sizeof *offsets);
	for (size_t i = 0; i < count; i++) {
		total = alignUp(total, 16);
		offsets[i] = total;
		total += inputs[i].size;
	}

	uint8_t *pak = xalloc(total);
	memset(pak, 0, total);
	memcpy(pak, WIBO_PAK_MAGIC, WIBO_PAK_MAGIC_SIZE);
	wibo_pakPutU32(pak + 8, WIBO_PAK_VERSION);
	wibo_pakPutU32(pak + 12, (uint32_t)count);
	wibo_pakPutU32(pak + 16, 0); // The first input is the main program.

	size_t pos = WIBO_PAK_HEADER_SIZE;
	for (size_t i = 0; i < count; i++) {
		size_t nameLen = strlen(inputs[i].name);
		wibo_pakPutU32(pak + pos, (uint32_t)nameLen);
		memcpy(pak + pos + 4, inputs[i].name, nameLen);
		pos += 4 + nameLen;
		wibo_pakPutU64(pak + pos, offsets[i]);
		wibo_pakPutU64(pak + pos + 8, inputs[i].size);
		pos += 16;
		memcpy(pak + offsets[i], inputs[i].data, inputs[i].size);
	}
	free(offsets);
	*sizeOut = total;
	return pak;
}

static void writeFile(const char *path, const uint8_t *parts[], const size_t sizes[], size_t n, bool executable) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		die("cannot create %s: %s", path, strerror(errno));
	}
	for (size_t i = 0; i < n; i++) {
		if (sizes[i] && fwrite(parts[i], 1, sizes[i], f) != sizes[i]) {
			die("write error on %s", path);
		}
	}
	if (fclose(f) != 0) {
		die("write error on %s", path);
	}
	if (executable && chmod(path, 0755) != 0) {
		die("cannot chmod %s: %s", path, strerror(errno));
	}
}

int main(int argc, char **argv) {
	const char *out = NULL;
	const char *stubPath = NULL;
	bool pakOnly = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			out = argv[++i];
		} else if (strcmp(argv[i], "--stub") == 0 && i + 1 < argc) {
			stubPath = argv[++i];
		} else if (strcmp(argv[i], "--pak") == 0) {
			pakOnly = true;
		} else {
			usage();
		}
	}
	if (!out || i >= argc || (!pakOnly && !stubPath)) {
		usage();
	}
	if (pakOnly && stubPath) {
		die("--stub and --pak are mutually exclusive");
	}

	size_t count = (size_t)(argc - i);
	Input *inputs = xalloc(count * sizeof *inputs);
	for (size_t k = 0; k < count; k++) {
		inputs[k].path = argv[i + (int)k];
		validateInput(&inputs[k]);
		for (size_t j = 0; j < k; j++) {
			if (strcasecmp(inputs[j].name, inputs[k].name) == 0) {
				die("duplicate file name '%s' (%s and %s)", inputs[k].name, inputs[j].path, inputs[k].path);
			}
		}
	}

	size_t pakSize = 0;
	uint8_t *pak = buildPak(inputs, count, &pakSize);

	if (pakOnly) {
		const uint8_t *parts[] = {pak};
		const size_t sizes[] = {pakSize};
		writeFile(out, parts, sizes, 1, false);
		return 0;
	}

	size_t stubSize = 0;
	uint8_t *stub = readFile(stubPath, &stubSize);
	if (stubSize >= WIBO_BUNDLE_TRAILER_SIZE &&
		memcmp(stub + stubSize - 8, WIBO_BUNDLE_TRAILER_MAGIC, 8) == 0) {
		die("%s already carries a bundle; use a plain wibo as --stub", stubPath);
	}
	uint8_t trailer[WIBO_BUNDLE_TRAILER_SIZE];
	wibo_pakPutU64(trailer, pakSize);
	memcpy(trailer + 8, WIBO_BUNDLE_TRAILER_MAGIC, 8);

	const uint8_t *parts[] = {stub, pak, trailer};
	const size_t sizes[] = {stubSize, pakSize, sizeof trailer};
	writeFile(out, parts, sizes, 3, true);
	return 0;
}
