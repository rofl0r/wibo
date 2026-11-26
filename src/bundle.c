#include "bundle.h"

#include "cutil.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// A bundle is this executable, followed by a payload and a 16-byte trailer. The trailer holds the payload size
// as a little-endian u64, then the magic. The payload is a text manifest followed by the file contents.
// Each manifest line is "name offset size\n"; offsets are absolute from the payload start. The first line is the
// main program. Its offset is the manifest length, and the files are stored back to back after the manifest.
#define BUNDLE_TRAILER_MAGIC "WIBOBNDL"
#define BUNDLE_TRAILER_SIZE 16
#define BUNDLE_MAX_NAME_LEN 255
#define BUNDLE_COPY_CHUNK (64 * 1024)

#define PE_MACHINE_I386 0x014c
#define PE_MACHINE_X86_64 0x8664

// The build target decides which PE images the bundle may carry: CC selects the architecture of the stub.
#ifdef WIBO_GUEST64
#define GUEST_PE_MACHINE PE_MACHINE_X86_64
#define GUEST_ARCH_NAME "PE32+ (x86_64)"
#else
#define GUEST_PE_MACHINE PE_MACHINE_I386
#define GUEST_ARCH_NAME "PE32 (i386)"
#endif

typedef struct {
	char *name;
	const uint8_t *data;
	size_t size;
} BundleEntry;

static struct {
	bool active;
	BundleEntry *entries;
	size_t count;
	char *dir; // Canonical directory of the running executable; bundled files are served as if they lived here.
} g_bundle;

static void fatal(const char *msg) {
	fprintf(stderr, "wibo: %s\n", msg);
	exit(1);
}

static uint64_t getU64(const uint8_t *p) {
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) {
		v = (v << 8) | p[i];
	}
	return v;
}

static void putU64(uint8_t *p, uint64_t v) {
	for (int i = 0; i < 8; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static bool isValidName(const char *name, size_t len) {
	if (len == 0 || len > BUNDLE_MAX_NAME_LEN) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)name[i];
		if (c < 0x21 || c > 0x7e || c == '/' || c == '\\') {
			return false;
		}
	}
	return true;
}

static bool parseDecimal(const uint8_t *buf, size_t limit, size_t *pos, uint64_t *out) {
	size_t p = *pos;
	uint64_t v = 0;
	if (p >= limit || buf[p] < '0' || buf[p] > '9') {
		return false;
	}
	while (p < limit && buf[p] >= '0' && buf[p] <= '9') {
		uint64_t digit = buf[p] - '0';
		if (v > (UINT64_MAX - digit) / 10) {
			return false;
		}
		v = v * 10 + digit;
		p++;
	}
	*out = v;
	*pos = p;
	return true;
}

// Parses one "name offset size\n" line at *pos. On success the name is a new string owned by the caller.
static bool parseLine(const uint8_t *buf, size_t limit, size_t *pos, char **name, uint64_t *offset, uint64_t *size) {
	size_t nameEnd = *pos;
	while (nameEnd < limit && buf[nameEnd] != ' ') {
		nameEnd++;
	}
	if (nameEnd >= limit || !isValidName((const char *)buf + *pos, nameEnd - *pos)) {
		return false;
	}
	size_t p = nameEnd + 1;
	if (!parseDecimal(buf, limit, &p, offset) || p >= limit || buf[p] != ' ') {
		return false;
	}
	p++;
	if (!parseDecimal(buf, limit, &p, size) || p >= limit || buf[p] != '\n') {
		return false;
	}
	*name = xstrndup((const char *)buf + *pos, nameEnd - *pos);
	*pos = p + 1;
	return true;
}

static bool hasEntryNamed(const BundleEntry *entries, size_t count, const char *name) {
	for (size_t i = 0; i < count; i++) {
		if (strcasecmp(entries[i].name, name) == 0) {
			return true;
		}
	}
	return false;
}

// Parses a payload into g_bundle. The payload must stay alive for the whole process; entries point into it.
static bool parsePayload(const uint8_t *payload, size_t size) {
	char *name = NULL;
	uint64_t offset = 0, dataSize = 0;
	size_t pos = 0;
	if (!parseLine(payload, size, &pos, &name, &offset, &dataSize)) {
		return false;
	}
	free(name);
	if (offset == 0 || offset > size) {
		return false;
	}
	size_t manifestLen = (size_t)offset;

	BundleEntry *entries = NULL;
	size_t count = 0, cap = 0;
	size_t expected = manifestLen;
	pos = 0;
	while (pos < manifestLen) {
		name = NULL;
		bool ok = parseLine(payload, manifestLen, &pos, &name, &offset, &dataSize);
		if (ok) {
			ok = offset == expected && dataSize <= size - expected && !hasEntryNamed(entries, count, name);
		}
		if (!ok) {
			free(name);
			for (size_t i = 0; i < count; i++) {
				free(entries[i].name);
			}
			free(entries);
			return false;
		}
		if (count == cap) {
			cap = cap ? cap * 2 : 8;
			entries = xrealloc(entries, cap * sizeof *entries);
		}
		entries[count].name = name;
		entries[count].data = payload + offset;
		entries[count].size = (size_t)dataSize;
		count++;
		expected = (size_t)(offset + dataSize);
	}
	if (count == 0 || pos != manifestLen || expected != size) {
		for (size_t i = 0; i < count; i++) {
			free(entries[i].name);
		}
		free(entries);
		return false;
	}

	g_bundle.entries = entries;
	g_bundle.count = count;
	return true;
}

static char *dirnameOf(const char *path) {
	const char *slash = strrchr(path, '/');
	if (!slash) {
		return xstrdup(".");
	}
	if (slash == path) {
		return xstrdup("/");
	}
	return xstrndup(path, (size_t)(slash - path));
}

static bool readFully(int fd, uint8_t *buf, size_t len, off_t offset) {
	while (len > 0) {
		ssize_t n = pread(fd, buf, len, offset);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		buf += n;
		len -= (size_t)n;
		offset += n;
	}
	return true;
}

// Resolves argv0 to the canonical path of the running executable. Like a shell, a bare name is searched in PATH.
// Writes at most PATH_MAX bytes to out and returns false if the file cannot be found.
static bool resolveSelf(const char *argv0, char *out) {
	const char *path = argv0;
	char candidate[PATH_MAX];
	if (!strchr(argv0, '/')) {
		const char *pathEnv = getenv("PATH");
		size_t nameLen = strlen(argv0);
		for (const char *p = pathEnv ? pathEnv : "";;) {
			const char *end = strchr(p, ':');
			if (!end) {
				end = p + strlen(p);
			}
			size_t dirLen = (size_t)(end - p);
			// An empty PATH entry means the current directory.
			const char *dir = dirLen ? p : ".";
			size_t prefix = dirLen ? dirLen : 1;
			if (prefix + 1 + nameLen < sizeof candidate) {
				memcpy(candidate, dir, prefix);
				candidate[prefix] = '/';
				memcpy(candidate + prefix + 1, argv0, nameLen + 1);
				if (access(candidate, X_OK) == 0) {
					path = candidate;
					break;
				}
			}
			if (!*end) {
				break;
			}
			p = end + 1;
		}
	}
	return realpath(path, out) != NULL;
}

// Loads the bundle appended to the executable at self. Returns false when there is no trailer.
static bool loadTrailer(const char *self) {
	int fd = open(self, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	struct stat st;
	uint8_t trailer[BUNDLE_TRAILER_SIZE];
	bool found = false;
	if (fstat(fd, &st) == 0 && st.st_size >= (off_t)BUNDLE_TRAILER_SIZE &&
		readFully(fd, trailer, sizeof trailer, st.st_size - BUNDLE_TRAILER_SIZE) &&
		memcmp(trailer + 8, BUNDLE_TRAILER_MAGIC, 8) == 0) {
		uint64_t payloadSize = getU64(trailer);
		uint64_t available = (uint64_t)st.st_size - BUNDLE_TRAILER_SIZE;
		if (payloadSize > available) {
			fatal("corrupt bundle trailer");
		}
		size_t size = (size_t)payloadSize;
		uint8_t *payload = xmalloc(size + 1);
		if (!readFully(fd, payload, size, (off_t)(available - payloadSize))) {
			fatal("failed to read bundle payload");
		}
		if (!parsePayload(payload, size)) {
			fatal("corrupt bundle payload");
		}
		found = true;
	}
	close(fd);
	return found;
}

void wibo_bundleInit(const char *argv0) {
	char self[PATH_MAX];
	if (!resolveSelf(argv0, self) || !loadTrailer(self)) {
		return;
	}
	g_bundle.dir = dirnameOf(self);
	g_bundle.active = true;
}

bool wibo_bundleActive(void) { return g_bundle.active; }

char *wibo_bundleMainPath(void) {
	if (!g_bundle.active) {
		return NULL;
	}
	const char *name = g_bundle.entries[0].name;
	bool rootDir = strcmp(g_bundle.dir, "/") == 0;
	size_t len = strlen(g_bundle.dir) + (rootDir ? 0 : 1) + strlen(name);
	char *path = xmalloc(len + 1);
	strcpy(path, g_bundle.dir);
	if (!rootDir) {
		strcat(path, "/");
	}
	strcat(path, name);
	return path;
}

static const BundleEntry *findEntry(const char *dir, const char *name) {
	if (!g_bundle.active || !dir || !name || !*name) {
		return NULL;
	}
	const BundleEntry *match = NULL;
	for (size_t i = 0; i < g_bundle.count && !match; i++) {
		if (strcasecmp(g_bundle.entries[i].name, name) == 0) {
			match = &g_bundle.entries[i];
		}
	}
	if (!match) {
		return NULL;
	}
	char *canonical = realpath(dir, NULL);
	bool same = canonical && strcmp(canonical, g_bundle.dir) == 0;
	free(canonical);
	return same ? match : NULL;
}

const char *wibo_bundleFindName(const char *dir, const char *name) {
	const BundleEntry *entry = findEntry(dir, name);
	return entry ? entry->name : NULL;
}

FILE *wibo_openImage(const char *path) {
	const char *slash = strrchr(path, '/');
	if (slash && g_bundle.active) {
		char *dir = dirnameOf(path);
		const BundleEntry *entry = findEntry(dir, slash + 1);
		free(dir);
		if (entry) {
			FILE *file = fmemopen((void *)entry->data, entry->size, "rb");
			if (file) {
				return file;
			}
		}
	}
	return fopen(path, "rb");
}

// Packer. Runs only in a plain wibo, where no bundle trailer is present.

typedef struct {
	char *name;
	uint8_t *data;
	size_t size;
} PackInput;

static const char *g_packOut;
static bool g_packCreated;

static void packFail(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void packFail(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "wibo --bundle: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	if (g_packCreated) {
		unlink(g_packOut);
	}
	exit(1);
}

static void writeAll(int fd, const void *buf, size_t len) {
	const uint8_t *p = buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			packFail("failed to write %s: %s", g_packOut, strerror(errno));
		}
		p += n;
		len -= (size_t)n;
	}
}

static uint8_t *readWhole(const char *path, size_t *sizeOut) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		packFail("cannot open %s: %s", path, strerror(errno));
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		packFail("%s is not a regular file", path);
	}
	size_t size = (size_t)st.st_size;
	uint8_t *buf = xmalloc(size + 1);
	size_t got = 0;
	while (got < size) {
		ssize_t n = read(fd, buf + got, size - got);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			packFail("cannot read %s: %s", path, n < 0 ? strerror(errno) : "unexpected end of file");
		}
		got += (size_t)n;
	}
	close(fd);
	*sizeOut = size;
	return buf;
}

// Returns the PE machine type of an image, or 0 if it has no PE header.
static uint16_t peMachine(const uint8_t *data, size_t size) {
	if (size < 0x40 || data[0] != 'M' || data[1] != 'Z') {
		return 0;
	}
	size_t peOff =
		(size_t)data[0x3c] | ((size_t)data[0x3d] << 8) | ((size_t)data[0x3e] << 16) | ((size_t)data[0x3f] << 24);
	if (peOff > size || size - peOff < 6 || memcmp(data + peOff, "PE\0\0", 4) != 0) {
		return 0;
	}
	return (uint16_t)(data[peOff + 4] | (data[peOff + 5] << 8));
}

static const char *peArchName(uint16_t machine) {
	switch (machine) {
	case PE_MACHINE_I386:
		return "PE32 (i386)";
	case PE_MACHINE_X86_64:
		return "PE32+ (x86_64)";
	default:
		return NULL;
	}
}

static size_t decimalDigits(uint64_t v) {
	size_t n = 1;
	while (v >= 10) {
		v /= 10;
		n++;
	}
	return n;
}

// Length of the manifest when the first file starts at manifestLen.
static uint64_t manifestLength(const PackInput *files, size_t count, uint64_t manifestLen) {
	uint64_t total = 0;
	uint64_t offset = manifestLen;
	for (size_t i = 0; i < count; i++) {
		total += strlen(files[i].name) + 1 + decimalDigits(offset) + 1 + decimalDigits(files[i].size) + 1;
		offset += files[i].size;
	}
	return total;
}

static void printBundleUsage(const char *exeName, FILE *out) {
	fprintf(out, "Usage:\n");
	fprintf(out, "  %s --bundle -o OUT FILE [FILE...]\n", exeName);
	fprintf(out, "\n");
	fprintf(out, "Packs FILE... into the standalone executable OUT. The first FILE is the program that runs;\n");
	fprintf(out, "the others are available to it by basename. All files must be images of this build's\n");
	fprintf(out, "architecture (PE32 for a default build, PE32+ for WIN64=1). Linux only.\n");
}

#endif

#ifdef __linux__

int wibo_bundleMain(int argc, char **argv, const char *argv0) {
	if (argc >= 1 && (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0)) {
		printBundleUsage(argv0, stdout);
		return 0;
	}
	if (argc < 3 || strcmp(argv[0], "-o") != 0) {
		printBundleUsage(argv0, stderr);
		return 2;
	}
	char self[PATH_MAX];
	if (!resolveSelf(argv0, self)) {
		packFail("cannot locate the wibo executable from argv[0] '%s'", argv0);
	}
	const char *outPath = argv[1];
	size_t count = (size_t)argc - 2;
	PackInput *files = xcalloc(count, sizeof *files);

	for (size_t i = 0; i < count; i++) {
		const char *path = argv[i + 2];
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		if (!isValidName(base, strlen(base))) {
			packFail("'%s' cannot be bundled: names must be 1-%d printable ASCII characters without '/', '\\' or "
					 "spaces",
					 path, BUNDLE_MAX_NAME_LEN);
		}
		bool duplicate = false;
		for (size_t j = 0; j < i; j++) {
			duplicate = duplicate || strcasecmp(files[j].name, base) == 0;
		}
		if (duplicate) {
			packFail("duplicate bundle name '%s' (names are compared case-insensitively)", base);
		}
		files[i].name = xstrdup(base);
		files[i].data = readWhole(path, &files[i].size);

		uint16_t machine = peMachine(files[i].data, files[i].size);
		if (!peArchName(machine)) {
			packFail("%s is not a PE32 or PE32+ image", path);
		}
		if (machine != GUEST_PE_MACHINE) {
			packFail("%s is %s, but this wibo build bundles %s images only", path, peArchName(machine),
					 GUEST_ARCH_NAME);
		}
	}

	uint64_t manifestLen = 0;
	for (;;) {
		uint64_t next = manifestLength(files, count, manifestLen);
		if (next == manifestLen) {
			break;
		}
		manifestLen = next;
	}
	uint64_t payloadSize = manifestLen;
	for (size_t i = 0; i < count; i++) {
		payloadSize += files[i].size;
	}

	int in = open(self, O_RDONLY | O_CLOEXEC);
	if (in < 0) {
		packFail("cannot read the wibo executable: %s", strerror(errno));
	}
	int out = open(outPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
	if (out < 0) {
		packFail("cannot create %s: %s", outPath, strerror(errno));
	}
	g_packOut = outPath;
	g_packCreated = true;

	uint8_t *chunk = xmalloc(BUNDLE_COPY_CHUNK);
	for (;;) {
		ssize_t n = read(in, chunk, BUNDLE_COPY_CHUNK);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0) {
			packFail("cannot read the wibo executable: %s", strerror(errno));
		}
		if (n == 0) {
			break;
		}
		writeAll(out, chunk, (size_t)n);
	}
	close(in);
	free(chunk);

	uint64_t offset = manifestLen;
	char line[BUNDLE_MAX_NAME_LEN + 64];
	for (size_t i = 0; i < count; i++) {
		int n = snprintf(line, sizeof line, "%s %llu %llu\n", files[i].name, (unsigned long long)offset,
						 (unsigned long long)files[i].size);
		writeAll(out, line, (size_t)n);
		offset += files[i].size;
	}
	for (size_t i = 0; i < count; i++) {
		writeAll(out, files[i].data, files[i].size);
	}

	uint8_t trailer[BUNDLE_TRAILER_SIZE];
	putU64(trailer, payloadSize);
	memcpy(trailer + 8, BUNDLE_TRAILER_MAGIC, 8);
	writeAll(out, trailer, sizeof trailer);

	if (fchmod(out, 0755) != 0 || close(out) != 0) {
		packFail("cannot finish %s: %s", outPath, strerror(errno));
	}
	return 0;
}

#else

int wibo_bundleMain(int argc, char **argv, const char *argv0) {
	(void)argc;
	(void)argv;
	fprintf(stderr, "%s: --bundle is only supported on Linux\n", argv0);
	return 1;
}

void wibo_bundleInit(const char *argv0) { (void)argv0; }

bool wibo_bundleActive(void) { return false; }

char *wibo_bundleMainPath(void) { return NULL; }

const char *wibo_bundleFindName(const char *dir, const char *name) {
	(void)dir;
	(void)name;
	return NULL;
}

FILE *wibo_openImage(const char *path) { return fopen(path, "rb"); }

#endif
