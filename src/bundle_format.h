#ifndef WIBO_BUNDLE_FORMAT_H
#define WIBO_BUNDLE_FORMAT_H

// On-disk format shared by the runtime (src/bundle.c) and the bundler (tools/wibo-bundle.c).
// All integers are little-endian.
//
// Pak layout:
//   header:  "WIBOPAK1" (8) | version u32 | count u32 | mainIndex u32 | reserved u32
//   entries: nameLen u32 | name (nameLen bytes, no NUL, basename only) | dataOffset u64 | size u64
//   data:    file contents, addressed by dataOffset relative to the start of the pak
//
// A bundled executable is the wibo stub, then the pak, then a trailer: pakSize u64 | "WIBOBNDL".

#include <stddef.h>
#include <stdint.h>

#define WIBO_PAK_MAGIC "WIBOPAK1"
#define WIBO_PAK_MAGIC_SIZE 8
#define WIBO_PAK_VERSION 1u
#define WIBO_PAK_HEADER_SIZE 24u
// nameLen + dataOffset + size, with an empty name.
#define WIBO_PAK_MIN_ENTRY_SIZE 20u
#define WIBO_PAK_MAX_NAME_LEN 255u

#define WIBO_BUNDLE_TRAILER_MAGIC "WIBOBNDL"
#define WIBO_BUNDLE_TRAILER_SIZE 16u

static inline uint32_t wibo_pakGetU32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t wibo_pakGetU64(const uint8_t *p) {
	return (uint64_t)wibo_pakGetU32(p) | ((uint64_t)wibo_pakGetU32(p + 4) << 32);
}

static inline void wibo_pakPutU32(uint8_t *p, uint32_t v) {
	for (int i = 0; i < 4; i++) {
		p[i] = (uint8_t)(v >> (8 * i));
	}
}

static inline void wibo_pakPutU64(uint8_t *p, uint64_t v) {
	wibo_pakPutU32(p, (uint32_t)v);
	wibo_pakPutU32(p + 4, (uint32_t)(v >> 32));
}

#endif
