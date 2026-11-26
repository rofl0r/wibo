#include "bundle.h"

#include "bundle_format.h"
#include "cutil.h"

#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

// Provided by the payload object that `make WIBO_BUNDLE_FILES=...` links in; undefined in plain builds.
extern const uint8_t wibo_embeddedPak[] __attribute__((weak));
extern const uint8_t wibo_embeddedPakEnd[] __attribute__((weak));

typedef struct {
	char *name;
	const uint8_t *data;
	size_t size;
} BundleEntry;

static struct {
	bool active;
	BundleEntry *entries;
	uint32_t count;
	uint32_t mainIndex;
	char *dir; // Canonical directory of the running executable; embedded files are served as if they lived here.
} g_bundle;

static bool isValidEntryName(const uint8_t *name, uint32_t len) {
	return len > 0 && len <= WIBO_PAK_MAX_NAME_LEN && memchr(name, '\0', len) == NULL &&
		   memchr(name, '/', len) == NULL && memchr(name, '\\', len) == NULL;
}

// Parses pak into g_bundle. The pak buffer must stay alive for the whole process; entries point into it.
static bool parsePak(const uint8_t *pak, size_t size) {
	if (size < WIBO_PAK_HEADER_SIZE || memcmp(pak, WIBO_PAK_MAGIC, WIBO_PAK_MAGIC_SIZE) != 0) {
		return false;
	}
	if (wibo_pakGetU32(pak + 8) != WIBO_PAK_VERSION) {
		return false;
	}
	uint32_t count = wibo_pakGetU32(pak + 12);
	uint32_t mainIndex = wibo_pakGetU32(pak + 16);
	if (count == 0 || count > (size - WIBO_PAK_HEADER_SIZE) / WIBO_PAK_MIN_ENTRY_SIZE || mainIndex >= count) {
		return false;
	}

	BundleEntry *entries = xcalloc(count, sizeof *entries);
	size_t pos = WIBO_PAK_HEADER_SIZE;
	for (uint32_t i = 0; i < count; i++) {
		if (size - pos < 4) {
			goto fail;
		}
		uint32_t nameLen = wibo_pakGetU32(pak + pos);
		pos += 4;
		if (size - pos < (size_t)nameLen + 16 || !isValidEntryName(pak + pos, nameLen)) {
			goto fail;
		}
		entries[i].name = xstrndup((const char *)(pak + pos), nameLen);
		pos += nameLen;
		uint64_t offset = wibo_pakGetU64(pak + pos);
		uint64_t dataSize = wibo_pakGetU64(pak + pos + 8);
		pos += 16;
		if (offset > size || dataSize > size - offset) {
			goto fail;
		}
		entries[i].data = pak + offset;
		entries[i].size = (size_t)dataSize;
		continue;
	fail:
		for (uint32_t j = 0; j <= i; j++) {
			free(entries[j].name);
		}
		free(entries);
		return false;
	}

	g_bundle.entries = entries;
	g_bundle.count = count;
	g_bundle.mainIndex = mainIndex;
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

static void installBundle(const uint8_t *pak, size_t size, const char *origin) {
	if (!parsePak(pak, size)) {
		fprintf(stderr, "wibo: corrupt bundle payload (%s)\n", origin);
		exit(1);
	}
	g_bundle.active = true;
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

// Loads a pak that wibo-bundle appended to this executable. Returns false when there is no trailer.
static bool loadTrailer(void) {
	int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	struct stat st;
	uint8_t trailer[WIBO_BUNDLE_TRAILER_SIZE];
	bool found = false;
	if (fstat(fd, &st) == 0 && st.st_size >= (off_t)WIBO_BUNDLE_TRAILER_SIZE &&
		readFully(fd, trailer, sizeof trailer, st.st_size - WIBO_BUNDLE_TRAILER_SIZE) &&
		memcmp(trailer + 8, WIBO_BUNDLE_TRAILER_MAGIC, 8) == 0) {
		uint64_t pakSize = wibo_pakGetU64(trailer);
		uint64_t available = (uint64_t)st.st_size - WIBO_BUNDLE_TRAILER_SIZE;
		if (pakSize > available) {
			fprintf(stderr, "wibo: corrupt bundle trailer\n");
			exit(1);
		}
		uint8_t *pak = xmalloc((size_t)pakSize);
		if (!readFully(fd, pak, (size_t)pakSize, (off_t)(available - pakSize))) {
			fprintf(stderr, "wibo: failed to read bundle payload\n");
			exit(1);
		}
		installBundle(pak, (size_t)pakSize, "trailer");
		found = true;
	}
	close(fd);
	return found;
}

void wibo_bundleInit(void) {
	const uint8_t *embeddedBegin = wibo_embeddedPak;
	const uint8_t *embeddedEnd = wibo_embeddedPakEnd;
	if (embeddedBegin && embeddedEnd > embeddedBegin) {
		installBundle(embeddedBegin, (size_t)(embeddedEnd - embeddedBegin), "embedded");
	} else if (!loadTrailer()) {
		return;
	}
	char *exe = realpath("/proc/self/exe", NULL);
	if (!exe) {
		fprintf(stderr, "wibo: cannot resolve /proc/self/exe for bundle\n");
		exit(1);
	}
	g_bundle.dir = dirnameOf(exe);
	free(exe);
}

bool wibo_bundleActive(void) {
	return g_bundle.active;
}

char *wibo_bundleMainPath(void) {
	if (!g_bundle.active) {
		return NULL;
	}
	const char *name = g_bundle.entries[g_bundle.mainIndex].name;
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
	for (uint32_t i = 0; i < g_bundle.count && !match; i++) {
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

#else

void wibo_bundleInit(void) {}

bool wibo_bundleActive(void) {
	return false;
}

char *wibo_bundleMainPath(void) {
	return NULL;
}

const char *wibo_bundleFindName(const char *dir, const char *name) {
	(void)dir;
	(void)name;
	return NULL;
}

FILE *wibo_openImage(const char *path) {
	return fopen(path, "rb");
}

#endif
