#include "cutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void outOfMemory(void) {
	fputs("wibo: out of memory\n", stderr);
	abort();
}

void *xmalloc(size_t size) {
	void *p = malloc(size ? size : 1);
	if (!p)
		outOfMemory();
	return p;
}

void *xcalloc(size_t count, size_t size) {
	void *p = calloc(count ? count : 1, size ? size : 1);
	if (!p)
		outOfMemory();
	return p;
}

void *xrealloc(void *ptr, size_t size) {
	void *p = realloc(ptr, size ? size : 1);
	if (!p)
		outOfMemory();
	return p;
}

char *xstrdup(const char *s) {
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n) {
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

void Buf_init(Buf *b) {
	b->data = NULL;
	b->len = 0;
	b->cap = 0;
}

void Buf_free(Buf *b) {
	free(b->data);
	Buf_init(b);
}

void Buf_reserve(Buf *b, size_t extra) {
	size_t need = b->len + extra;
	if (need <= b->cap)
		return;
	size_t cap = b->cap ? b->cap * 2 : 64;
	if (cap < need)
		cap = need;
	b->data = xrealloc(b->data, cap);
	b->cap = cap;
}

void Buf_append(Buf *b, const void *data, size_t n) {
	if (!n)
		return;
	Buf_reserve(b, n);
	memcpy(b->data + b->len, data, n);
	b->len += n;
}

void Buf_appendc(Buf *b, char c) {
	Buf_append(b, &c, 1);
}

void Buf_appendstr(Buf *b, const char *s) {
	Buf_append(b, s, strlen(s));
}

char *Buf_cstr(Buf *b) {
	Buf_reserve(b, 1);
	b->data[b->len] = '\0';
	return b->data;
}

typedef struct HashEntry {
	struct HashEntry *next;
	void *key;
	size_t keylen;
	void *value;
} HashEntry;

struct hashmap {
	HashEntry **buckets;
	size_t nbuckets; // Always a power of two.
	size_t count;
};

static const size_t kHashInitialBuckets = 16;

static uint64_t hashBytes(const void *key, size_t len) {
	const unsigned char *p = key;
	uint64_t h = 1469598103934665603ull; // FNV-1a offset basis.
	for (size_t i = 0; i < len; i++) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	return h;
}

// Returns the link that points at the matching entry, or at NULL where it would be inserted.
static HashEntry **findSlot(const hashmap *m, const void *key, size_t keylen) {
	HashEntry **slot = &m->buckets[hashBytes(key, keylen) & (m->nbuckets - 1)];
	while (*slot && !((*slot)->keylen == keylen && (keylen == 0 || memcmp((*slot)->key, key, keylen) == 0)))
		slot = &(*slot)->next;
	return slot;
}

static void growHashmap(hashmap *m) {
	size_t newCount = m->nbuckets * 2;
	HashEntry **newBuckets = xcalloc(newCount, sizeof *newBuckets);
	for (size_t i = 0; i < m->nbuckets; i++) {
		HashEntry *e = m->buckets[i];
		while (e) {
			HashEntry *next = e->next;
			size_t idx = hashBytes(e->key, e->keylen) & (newCount - 1);
			e->next = newBuckets[idx];
			newBuckets[idx] = e;
			e = next;
		}
	}
	free(m->buckets);
	m->buckets = newBuckets;
	m->nbuckets = newCount;
}

hashmap *hashmap_create(void) {
	hashmap *m = xcalloc(1, sizeof *m);
	m->nbuckets = kHashInitialBuckets;
	m->buckets = xcalloc(m->nbuckets, sizeof *m->buckets);
	return m;
}

void hashmap_free(hashmap *m) {
	if (!m)
		return;
	for (size_t i = 0; i < m->nbuckets; i++) {
		HashEntry *e = m->buckets[i];
		while (e) {
			HashEntry *next = e->next;
			free(e->key);
			free(e);
			e = next;
		}
	}
	free(m->buckets);
	free(m);
}

size_t hashmap_count(const hashmap *m) {
	return m->count;
}

bool hashmap_find(const hashmap *m, const void *key, size_t keylen, void **out) {
	HashEntry *e = *findSlot(m, key, keylen);
	if (!e)
		return false;
	if (out)
		*out = e->value;
	return true;
}

void hashmap_put(hashmap *m, const void *key, size_t keylen, void *value) {
	HashEntry **slot = findSlot(m, key, keylen);
	if (*slot) {
		(*slot)->value = value;
		return;
	}
	if ((m->count + 1) * 4 > m->nbuckets * 3) {
		growHashmap(m);
		slot = findSlot(m, key, keylen);
	}
	HashEntry *e = xmalloc(sizeof *e);
	e->key = xmalloc(keylen);
	memcpy(e->key, key, keylen);
	e->keylen = keylen;
	e->value = value;
	e->next = NULL;
	*slot = e;
	m->count++;
}

bool hashmap_remove(hashmap *m, const void *key, size_t keylen, void **out) {
	HashEntry **slot = findSlot(m, key, keylen);
	HashEntry *e = *slot;
	if (!e)
		return false;
	if (out)
		*out = e->value;
	*slot = e->next;
	free(e->key);
	free(e);
	m->count--;
	return true;
}

void hashmap_foreach(const hashmap *m, hashmap_cb cb, void *ctx) {
	for (size_t i = 0; i < m->nbuckets; i++) {
		for (HashEntry *e = m->buckets[i]; e; e = e->next)
			cb(e->key, e->keylen, e->value, ctx);
	}
}

size_t wstr_len(const uint16_t *s) {
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

size_t wstr_nlen(const uint16_t *s, size_t max) {
	size_t n = 0;
	while (n < max && s[n])
		n++;
	return n;
}

char *wstr_to_ascii(const uint16_t *src, size_t len) {
	if (!src)
		len = 0;
	else if (len == SIZE_MAX)
		len = wstr_len(src);
	char *out = xmalloc(len + 1);
	for (size_t i = 0; i < len; i++)
		out[i] = (char)(src[i] & 0xFF);
	out[len] = '\0';
	return out;
}

uint16_t *ascii_to_wstr(const char *src, size_t len) {
	if (!src)
		len = 0;
	else if (len == SIZE_MAX)
		len = strlen(src);
	uint16_t *out = xmalloc((len + 1) * sizeof *out);
	for (size_t i = 0; i < len; i++)
		out[i] = (unsigned char)src[i];
	out[len] = 0;
	return out;
}
