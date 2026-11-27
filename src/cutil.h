#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Allocation: all of these abort the process on out-of-memory.
void *xmalloc(size_t size);            // Allocate uninitialized memory.
void *xcalloc(size_t count, size_t size); // Allocate zeroed memory for count * size bytes.
void *xrealloc(void *ptr, size_t size);   // Resize an allocation (ptr may be NULL).
char *xstrdup(const char *s);             // Duplicate a NUL-terminated string.
char *xstrndup(const char *s, size_t n);  // Duplicate at most n bytes of s, NUL-terminated.

// Growable byte buffer. A zero-initialized Buf is empty; Buf_free resets it.
typedef struct {
	char *data;
	size_t len;
	size_t cap;
} Buf;

void Buf_init(Buf *b);                                // Reset to empty without freeing.
void Buf_free(Buf *b);                                // Free storage and reset to empty.
void Buf_reserve(Buf *b, size_t extra);               // Ensure room for extra more bytes.
void Buf_append(Buf *b, const void *data, size_t n);  // Append n raw bytes.
void Buf_appendc(Buf *b, char c);                     // Append one byte.
void Buf_appendstr(Buf *b, const char *s);            // Append a NUL-terminated string (without the NUL).
char *Buf_cstr(Buf *b);                               // NUL-terminate in place and return the data (never NULL).

// Typed vector. Use VEC(T) as a struct type, typically via a typedef: typedef VEC(Foo) FooVec;
// Zero-initialized vectors are empty. Macros take a pointer to the vector.
#define VEC(T)                                                                                                         \
	struct {                                                                                                           \
		T *data;                                                                                                       \
		size_t len;                                                                                                    \
		size_t cap;                                                                                                    \
	}

// Ensure capacity for n more elements.
#define vec_reserve(v, n)                                                                                              \
	do {                                                                                                               \
		size_t vec_need_ = (v)->len + (n);                                                                             \
		if (vec_need_ > (v)->cap) {                                                                                    \
			size_t vec_cap_ = (v)->cap ? (v)->cap * 2 : 8;                                                             \
			if (vec_cap_ < vec_need_)                                                                                  \
				vec_cap_ = vec_need_;                                                                                  \
			(v)->data = xrealloc((v)->data, vec_cap_ * sizeof *(v)->data);                                             \
			(v)->cap = vec_cap_;                                                                                       \
		}                                                                                                              \
	} while (0)

// Append x (evaluated once before any reallocation).
#define vec_push(v, x)                                                                                                 \
	do {                                                                                                               \
		__auto_type vec_val_ = (x);                                                                                    \
		vec_reserve((v), 1);                                                                                           \
		(v)->data[(v)->len++] = vec_val_;                                                                              \
	} while (0)

// Remove and return the last element; the vector must not be empty.
#define vec_pop(v) ((v)->data[--(v)->len])

// Remove element at index i, preserving order of the remaining elements.
#define vec_erase(v, i)                                                                                                \
	do {                                                                                                               \
		size_t vec_idx_ = (i);                                                                                         \
		memmove(&(v)->data[vec_idx_], &(v)->data[vec_idx_ + 1], ((v)->len - vec_idx_ - 1) * sizeof *(v)->data);        \
		(v)->len--;                                                                                                    \
	} while (0)

#define vec_clear(v) ((v)->len = 0)

// Free storage and reset to empty.
#define vec_free(v)                                                                                                    \
	do {                                                                                                               \
		free((v)->data);                                                                                               \
		(v)->data = NULL;                                                                                              \
		(v)->len = 0;                                                                                                  \
		(v)->cap = 0;                                                                                                  \
	} while (0)

typedef VEC(char *) StringVec;
typedef VEC(void *) PtrVec;

// String-keyed hash map with void* values. Keys are copied on insert, values are not owned.
// Keys are arbitrary byte strings; pass strlen(s) for C strings. Not thread-safe.
typedef struct hashmap hashmap;
typedef void (*hashmap_cb)(const void *key, size_t keylen, void *value, void *ctx);

hashmap *hashmap_create(void);                                                    // Allocate an empty map.
void hashmap_free(hashmap *m);                                                    // Free the map (not the values).
size_t hashmap_count(const hashmap *m);                                           // Number of entries.
bool hashmap_find(const hashmap *m, const void *key, size_t keylen, void **out);  // Look up; out may be NULL.
void hashmap_put(hashmap *m, const void *key, size_t keylen, void *value);        // Insert or overwrite.
bool hashmap_remove(hashmap *m, const void *key, size_t keylen, void **out);      // Erase; out may be NULL.
void hashmap_foreach(const hashmap *m, hashmap_cb cb, void *ctx);                 // Visit all entries (no mutation).

static inline bool str_eq(const char *a, const char *b) { return strcmp(a, b) == 0; } // NULL-free strings only.

// UTF-16 helpers. Each code unit is zero-extended from or truncated to a byte, matching the loader's ASCII handling.
size_t wstr_len(const uint16_t *s);                                 // Length in code units, up to the NUL.
size_t wstr_nlen(const uint16_t *s, size_t max);                    // Length in code units, up to NUL or max.
char *wstr_to_ascii(const uint16_t *src, size_t len);               // Low bytes of len units (SIZE_MAX = to NUL); xmalloc'd.
uint16_t *ascii_to_wstr(const char *src, size_t len);               // Zero-extended bytes (SIZE_MAX = strlen); xmalloc'd.
