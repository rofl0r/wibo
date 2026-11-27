#include "handles.h"
#include "types.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>

// Max index that still yields HANDLE < 0x8000 with (index + 1) << 2
#define kHandleAlignShift 2
#define kCompatMaxIndex ((0x7FFFu >> kHandleAlignShift) - 1)
// Delay reuse of small handles to avoid accidental stale aliasing
#define kQuarantineLen 64

static inline uint32_t indexOf(HANDLE h) {
	uint32_t v = (uint32_t)h;
	if (v == 0 || (v & ((1U << kHandleAlignShift) - 1)) != 0) {
		return UINT32_MAX;
	}
	return (v >> kHandleAlignShift) - 1;
}

static inline HANDLE makeHandle(uint32_t index) {
	uint32_t v = (index + 1) << kHandleAlignShift;
	return (HANDLE)v;
}

static inline bool isPseudo(HANDLE h) { return (int32_t)h < 0; }

static Handles g_handles;
static pthread_once_t g_handlesOnce = PTHREAD_ONCE_INIT;

static void onHandleZero(ObjectBase *obj) { Namespace_remove(&wibo_g_namespace, obj); }

static void initHandles(void) { Handles_init(&g_handles, onHandleZero); }

void Handles_init(Handles *self, Handles_OnHandleZeroFn onHandleZero) {
	pthread_rwlock_init(&self->m, NULL);
	self->mSlots = (HandleEntryVec){0};
	self->mOnHandleZero = onHandleZero;
	self->mFreeBelow = (U32Vec){0};
	self->mFreeAbove = (U32Vec){0};
	self->mQuarantine = (U32Vec){0};
	self->nextIndex = 0;
}

void Handles_clear(Handles *self) {
	// Release the references outside the lock: destroy callbacks may touch the handle table.
	HandleEntryVec slots;
	pthread_rwlock_wrlock(&self->m);
	slots = self->mSlots;
	self->mSlots = (HandleEntryVec){0};
	vec_clear(&self->mFreeBelow);
	vec_clear(&self->mFreeAbove);
	self->nextIndex = 0;
	pthread_rwlock_unlock(&self->m);

	for (size_t i = 0; i < slots.len; i++) {
		ObjectBase *obj = slots.data[i].obj;
		if (obj) {
			ObjectBase_unref(obj);
		}
	}
	vec_free(&slots);
}

void Handles_free(Handles *self) {
	Handles_clear(self);
	vec_free(&self->mSlots);
	vec_free(&self->mFreeBelow);
	vec_free(&self->mFreeAbove);
	vec_free(&self->mQuarantine);
	pthread_rwlock_destroy(&self->m);
}

HANDLE Handles_alloc(Handles *self, ObjectBase *obj, uint32_t grantedAccess, uint32_t flags) {
	pthread_rwlock_wrlock(&self->m);

	// Attempt to, in order:
	// 1) use a fresh index in the compat range (0..kCompatMaxIndex)
	// 2) reuse a recently-freed index in the compat range
	// 3) reuse a recently-freed index above the compat range
	// 4) use a fresh index above the compat range
	Handles_Entry empty = {0};
	uint32_t idx;
	if (self->nextIndex <= kCompatMaxIndex) {
		idx = self->nextIndex++;
		if (idx >= self->mSlots.len) {
			vec_push(&self->mSlots, empty);
		}
	} else if (self->mFreeBelow.len) {
		idx = vec_pop(&self->mFreeBelow);
	} else if (self->mFreeAbove.len) {
		idx = vec_pop(&self->mFreeAbove);
	} else {
		idx = (uint32_t)self->mSlots.len;
		vec_push(&self->mSlots, empty);
	}

	// Initialize entry
	Handles_Entry *e = &self->mSlots.data[idx];
	e->obj = obj; // Transfer ownership
	e->meta.grantedAccess = grantedAccess;
	e->meta.flags = flags;
	e->meta.typeCache = obj->type;
	if (e->meta.generation == 0) {
		e->meta.generation = 1;
	}

	HANDLE h = makeHandle(idx);
	atomic_fetch_add_explicit(&obj->handleCount, 1, memory_order_relaxed);
	pthread_rwlock_unlock(&self->m);
	return h;
}

ObjectBase *Handles_get(Handles *self, HANDLE h, HandleMeta *metaOut) {
	if (h == NO_HANDLE || isPseudo(h)) {
		return NULL; // pseudo-handles have no entries
	}

	ObjectBase *obj = NULL;
	pthread_rwlock_rdlock(&self->m);
	uint32_t idx = indexOf(h);
	if (idx < self->mSlots.len && self->mSlots.data[idx].obj) {
		const Handles_Entry *e = &self->mSlots.data[idx];
		if (metaOut) {
			*metaOut = e->meta;
		}
		obj = e->obj;
		ObjectBase_ref(obj);
	}
	pthread_rwlock_unlock(&self->m);
	return obj;
}

ObjectBase *Handles_getAs(Handles *self, HANDLE h, ObjectType type, HandleMeta *metaOut) {
	HandleMeta metaLocal = {0};
	if (!metaOut) {
		metaOut = &metaLocal;
	}
	ObjectBase *obj = Handles_get(self, h, metaOut);
	if (obj && metaOut->typeCache != type) {
		ObjectBase_unref(obj);
		return NULL;
	}
	return obj;
}

ObjectBase *Handles_getWithFlag(Handles *self, HANDLE h, uint16_t flag, HandleMeta *metaOut) {
	ObjectBase *obj = Handles_get(self, h, metaOut);
	if (obj && !(obj->flags & flag)) {
		ObjectBase_unref(obj);
		return NULL;
	}
	return obj;
}

bool Handles_release(Handles *self, HANDLE h) {
	if (isPseudo(h)) {
		return true; // no-op, success
	}

	pthread_rwlock_wrlock(&self->m);
	uint32_t idx = indexOf(h);
	if (idx >= self->mSlots.len) {
		pthread_rwlock_unlock(&self->m);
		return false;
	}
	Handles_Entry *e = &self->mSlots.data[idx];
	if (!e->obj || (e->meta.flags & HANDLE_FLAG_PROTECT_FROM_CLOSE)) {
		pthread_rwlock_unlock(&self->m);
		return false;
	}

	ObjectBase *obj = e->obj;
	uint16_t generation = e->meta.generation + 1;
	*e = (Handles_Entry){0}; // Clear entry
	e->meta.generation = generation;
	uint32_t handleCount = atomic_fetch_sub_explicit(&obj->handleCount, 1, memory_order_relaxed) - 1;

	if (idx <= kCompatMaxIndex) {
		vec_push(&self->mQuarantine, idx);
		if (self->mQuarantine.len > kQuarantineLen) {
			vec_push(&self->mFreeBelow, self->mQuarantine.data[0]);
			vec_erase(&self->mQuarantine, 0);
		}
	} else {
		vec_push(&self->mFreeAbove, idx);
	}
	pthread_rwlock_unlock(&self->m);

	if (handleCount == 0 && self->mOnHandleZero) {
		self->mOnHandleZero(obj);
	}
	ObjectBase_unref(obj);
	return true;
}

bool Handles_setInformation(Handles *self, HANDLE h, uint32_t mask, uint32_t value) {
	if (isPseudo(h)) {
		return true; // no-op, success
	}

	pthread_rwlock_wrlock(&self->m);
	uint32_t idx = indexOf(h);
	if (idx >= self->mSlots.len || !self->mSlots.data[idx].obj) {
		pthread_rwlock_unlock(&self->m);
		return false;
	}
	Handles_Entry *e = &self->mSlots.data[idx];

	const uint32_t kAllowedFlags = HANDLE_FLAG_INHERIT | HANDLE_FLAG_PROTECT_FROM_CLOSE;
	mask &= kAllowedFlags;

	e->meta.flags = (e->meta.flags & ~mask) | (value & mask);
	pthread_rwlock_unlock(&self->m);
	return true;
}

bool Handles_getInformation(Handles *self, HANDLE h, uint32_t *outFlags) {
	if (!outFlags) {
		return false;
	}
	if (isPseudo(h)) {
		*outFlags = 0;
		return true;
	}
	pthread_rwlock_rdlock(&self->m);
	uint32_t idx = indexOf(h);
	if (idx >= self->mSlots.len || !self->mSlots.data[idx].obj) {
		pthread_rwlock_unlock(&self->m);
		return false;
	}
	*outFlags = self->mSlots.data[idx].meta.flags;
	pthread_rwlock_unlock(&self->m);
	return true;
}

bool Handles_duplicateTo(Handles *self, HANDLE src, Handles *dst, HANDLE *out, uint32_t desiredAccess, bool inherit,
						 uint32_t options) {
	HandleMeta meta = {0};
	ObjectBase *obj = Handles_get(self, src, &meta);
	if (!obj) {
		return false;
	}

	bool closeSource = (options & DUPLICATE_CLOSE_SOURCE) != 0;
	if (closeSource && (meta.flags & HANDLE_FLAG_PROTECT_FROM_CLOSE) != 0) {
		// Cannot close source if it is protected
		ObjectBase_unref(obj);
		return false;
	}

	uint32_t effAccess = (options & DUPLICATE_SAME_ACCESS) ? meta.grantedAccess : (desiredAccess & meta.grantedAccess);
	const uint32_t flags = (inherit ? HANDLE_FLAG_INHERIT : 0);

	// Reuse the same handle if duplicating with DUPLICATE_CLOSE_SOURCE within the same table and no changes
	if (dst == self && closeSource && effAccess == meta.grantedAccess && flags == meta.flags) {
		ObjectBase_unref(obj);
		*out = src;
		return true;
	}

	*out = Handles_alloc(dst, obj, effAccess, flags);

	if (closeSource) {
		Handles_release(self, src);
	}
	return true;
}

typedef struct {
	ObjectBase *obj;
	Namespace_Entry *found;
} NamespaceRemoveCtx;

static void findNamespaceEntry(const void *key, size_t keylen, void *value, void *ctx) {
	(void)key;
	(void)keylen;
	NamespaceRemoveCtx *rc = ctx;
	Namespace_Entry *e = value;
	if (!rc->found && e->obj == rc->obj && !e->permanent) {
		rc->found = e;
	}
}

static void freeNamespaceEntry(const void *key, size_t keylen, void *value, void *ctx) {
	(void)key;
	(void)keylen;
	(void)ctx;
	Namespace_Entry *e = value;
	free(e->name);
	free(e);
}

static inline size_t namespaceKeyLen(size_t nameLen) { return nameLen * sizeof(uint16_t); }

void Namespace_init(Namespace *self) {
	pthread_rwlock_init(&self->m, NULL);
	self->table = hashmap_create();
}

void Namespace_free(Namespace *self) {
	pthread_rwlock_wrlock(&self->m);
	if (self->table) {
		hashmap_foreach(self->table, freeNamespaceEntry, NULL);
		hashmap_free(self->table);
		self->table = NULL;
	}
	pthread_rwlock_unlock(&self->m);
	pthread_rwlock_destroy(&self->m);
}

bool Namespace_insert(Namespace *self, const uint16_t *name, size_t nameLen, ObjectBase *obj, bool permanent) {
	if (nameLen == 0 || !obj) {
		return false;
	}
	pthread_rwlock_wrlock(&self->m);
	// Namespace holds a weak ref
	if (!self->table) {
		self->table = hashmap_create();
	}
	const size_t keylen = namespaceKeyLen(nameLen);
	bool inserted = false;
	if (!hashmap_find(self->table, name, keylen, NULL)) {
		Namespace_Entry *e = xmalloc(sizeof *e);
		e->obj = obj;
		e->permanent = permanent;
		e->name = xmalloc(keylen);
		memcpy(e->name, name, keylen);
		e->nameLen = nameLen;
		hashmap_put(self->table, name, keylen, e);
		inserted = true;
	}
	pthread_rwlock_unlock(&self->m);
	return inserted;
}

void Namespace_remove(Namespace *self, ObjectBase *obj) {
	pthread_rwlock_wrlock(&self->m);
	if (self->table) {
		NamespaceRemoveCtx rc = {obj, NULL};
		hashmap_foreach(self->table, findNamespaceEntry, &rc);
		if (rc.found) {
			Namespace_Entry *e = rc.found;
			hashmap_remove(self->table, e->name, namespaceKeyLen(e->nameLen), NULL);
			free(e->name);
			free(e);
		}
	}
	pthread_rwlock_unlock(&self->m);
}

ObjectBase *Namespace_get(Namespace *self, const uint16_t *name, size_t nameLen) {
	if (nameLen == 0) {
		return NULL;
	}
	ObjectBase *obj = NULL;
	pthread_rwlock_rdlock(&self->m);
	void *value = NULL;
	if (self->table && hashmap_find(self->table, name, namespaceKeyLen(nameLen), &value)) {
		Namespace_Entry *e = value;
		assert(e->obj);
		obj = e->obj;
		ObjectBase_ref(obj);
	}
	pthread_rwlock_unlock(&self->m);
	return obj;
}

ObjectBase *Namespace_getAs(Namespace *self, const uint16_t *name, size_t nameLen, ObjectType type) {
	ObjectBase *obj = Namespace_get(self, name, nameLen);
	if (obj && obj->type != type) {
		ObjectBase_unref(obj);
		return NULL;
	}
	return obj;
}

ObjectBase *Namespace_getOrCreate(Namespace *self, const uint16_t *name, size_t nameLen,
								  ObjectBase *(*make)(void *ctx), void *ctx, bool *created) {
	if (created) {
		*created = false;
	}
	if (nameLen == 0) {
		// No name: create unconditionally
		ObjectBase *obj = make(ctx);
		if (created) {
			*created = obj != NULL;
		}
		return obj;
	}
	ObjectBase *existing = Namespace_get(self, name, nameLen);
	if (existing) {
		// Return even if the type doesn't match (callers check the type)
		return existing;
	}
	ObjectBase *newObj = make(ctx);
	if (!newObj) {
		return NULL;
	}
	if (!Namespace_insert(self, name, nameLen, newObj, false)) {
		// Race: someone else inserted it first
		ObjectBase_unref(newObj);
		return Namespace_get(self, name, nameLen);
	}
	if (created) {
		*created = true;
	}
	return newObj;
}

void WaitableObject_init(WaitableObject *self, ObjectType type) {
	ObjectBase_init(&self->base, type, NULL);
	self->base.flags |= Of_Waitable;
	self->signaled = false;
	pthread_mutex_init(&self->m, NULL);
	pthread_cond_init(&self->cv, NULL);
	pthread_mutex_init(&self->waitersMutex, NULL);
	self->waiters = (WaiterVec){0};
}

void WaitableObject_free(WaitableObject *self) {
	pthread_mutex_destroy(&self->m);
	pthread_cond_destroy(&self->cv);
	pthread_mutex_destroy(&self->waitersMutex);
	vec_free(&self->waiters);
}

void WaitableObject_registerWaiter(WaitableObject *self, void *context, DWORD index, WaitableObject_WaiterCallback cb) {
	if (!cb) {
		return;
	}
	WaitableObject_Waiter waiter = {cb, context, index};
	pthread_mutex_lock(&self->waitersMutex);
	vec_push(&self->waiters, waiter);
	pthread_mutex_unlock(&self->waitersMutex);
}

void WaitableObject_unregisterWaiter(WaitableObject *self, void *context) {
	pthread_mutex_lock(&self->waitersMutex);
	size_t kept = 0;
	for (size_t i = 0; i < self->waiters.len; i++) {
		if (self->waiters.data[i].context != context) {
			self->waiters.data[kept++] = self->waiters.data[i];
		}
	}
	self->waiters.len = kept;
	pthread_mutex_unlock(&self->waitersMutex);
}

void WaitableObject_notifyWaiters(WaitableObject *self, bool abandoned) {
	// Callbacks may register or unregister waiters, so iterate over a snapshot taken under the lock.
	WaiterVec snapshot = {0};
	pthread_mutex_lock(&self->waitersMutex);
	vec_reserve(&snapshot, self->waiters.len);
	for (size_t i = 0; i < self->waiters.len; i++) {
		snapshot.data[i] = self->waiters.data[i];
	}
	snapshot.len = self->waiters.len;
	pthread_mutex_unlock(&self->waitersMutex);

	for (size_t i = 0; i < snapshot.len; i++) {
		WaitableObject_Waiter w = snapshot.data[i];
		if (w.callback) {
			w.callback(w.context, self, w.index, abandoned);
		}
	}
	vec_free(&snapshot);
}

Namespace wibo_g_namespace = {PTHREAD_RWLOCK_INITIALIZER, NULL};

Handles *wibo_handles(void) {
	pthread_once(&g_handlesOnce, initHandles);
	return &g_handles;
}
