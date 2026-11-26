#pragma once

#include "common.h"
#include "cutil.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Object references: every function returning an ObjectBase * (Handles_get*, Namespace_get*) returns a new
// reference that the caller must release with ObjectBase_unref. Handles_alloc and Namespace_getOrCreate
// consume the reference passed in. New objects are created with one reference owned by the creator.
typedef enum {
	ObjectType_File,
	ObjectType_Directory,
	ObjectType_Mapping,
	ObjectType_Process,
	ObjectType_Token,
	ObjectType_Mutex,
	ObjectType_Event,
	ObjectType_Semaphore,
	ObjectType_Thread,
	ObjectType_Heap,
	ObjectType_RegistryKey,
	ObjectType_NamedPipe,
	ObjectType_NamedPipeState,
} ObjectType;

enum {
	Of_None = 0x0,
	Of_Waitable = 0x1,
	Of_FsObject = 0x2,
	Of_File = 0x4,
};

typedef struct ObjectBase ObjectBase;
// Frees the entire containing object; the object's base is embedded as the first member `base`.
typedef void (*ObjectBase_DestroyFn)(ObjectBase *self);

struct ObjectBase {
	ObjectType type;
	uint16_t flags;
	_Atomic uint32_t pointerCount;
	_Atomic uint32_t handleCount;
	ObjectBase_DestroyFn destroy;
};

static inline void ObjectBase_init(ObjectBase *self, ObjectType type, ObjectBase_DestroyFn destroy) {
	self->type = type;
	self->flags = Of_None;
	atomic_init(&self->pointerCount, 1);
	atomic_init(&self->handleCount, 0);
	self->destroy = destroy;
}

static inline void ObjectBase_ref(ObjectBase *self) {
	atomic_fetch_add_explicit(&self->pointerCount, 1, memory_order_relaxed);
}

static inline void ObjectBase_unref(ObjectBase *self) {
	if (atomic_fetch_sub_explicit(&self->pointerCount, 1, memory_order_release) == 1) {
		atomic_thread_fence(memory_order_acquire);
		self->destroy(self);
	}
}

typedef struct WaitableObject WaitableObject;
typedef void (*WaitableObject_WaiterCallback)(void *context, WaitableObject *obj, DWORD index, bool abandoned);

typedef struct {
	WaitableObject_WaiterCallback callback;
	void *context;
	DWORD index;
} WaitableObject_Waiter;

typedef VEC(WaitableObject_Waiter) WaiterVec;

struct WaitableObject {
	ObjectBase base;
	bool signaled; // protected by m
	pthread_mutex_t m;
	pthread_cond_t cv;
	pthread_mutex_t waitersMutex;
	WaiterVec waiters;
};

// Destroy callbacks of waitable subclasses call WaitableObject_free before freeing the object.
void WaitableObject_init(WaitableObject *self, ObjectType type);
void WaitableObject_free(WaitableObject *self);
void WaitableObject_registerWaiter(WaitableObject *self, void *context, DWORD index, WaitableObject_WaiterCallback cb);
void WaitableObject_unregisterWaiter(WaitableObject *self, void *context);
void WaitableObject_notifyWaiters(WaitableObject *self, bool abandoned);

#define HANDLE_FLAG_INHERIT 0x1
#define HANDLE_FLAG_PROTECT_FROM_CLOSE 0x2

#define DUPLICATE_CLOSE_SOURCE 0x1
#define DUPLICATE_SAME_ACCESS 0x2

typedef struct {
	uint32_t grantedAccess;
	uint32_t flags;
	ObjectType typeCache;
	uint16_t generation;
} HandleMeta;

// We have to stay under a HANDLE value of 0x7FFF for legacy applications,
// and handles values are aligned to 4.
#define MAX_HANDLES 0x2000

typedef void (*Handles_OnHandleZeroFn)(ObjectBase *obj);

typedef struct {
	ObjectBase *obj;
	HandleMeta meta;
} Handles_Entry;

typedef VEC(Handles_Entry) HandleEntryVec;
typedef VEC(uint32_t) U32Vec;

typedef struct {
	pthread_rwlock_t m;
	HandleEntryVec mSlots;
	Handles_OnHandleZeroFn mOnHandleZero;
	U32Vec mFreeBelow;
	U32Vec mFreeAbove;
	U32Vec mQuarantine;
	uint32_t nextIndex;
} Handles;

void Handles_init(Handles *self, Handles_OnHandleZeroFn onHandleZero);
void Handles_free(Handles *self);
void Handles_clear(Handles *self);
// Takes ownership of one reference to obj.
HANDLE Handles_alloc(Handles *self, ObjectBase *obj, uint32_t grantedAccess, uint32_t flags);
bool Handles_release(Handles *self, HANDLE h);
// metaOut may be NULL.
ObjectBase *Handles_get(Handles *self, HANDLE h, HandleMeta *metaOut);
// Matches obj->type exactly; metaOut may be NULL.
ObjectBase *Handles_getAs(Handles *self, HANDLE h, ObjectType type, HandleMeta *metaOut);
// Matches any object whose flags include `flag` (e.g. Of_Waitable, Of_FsObject, Of_File); metaOut may be NULL.
ObjectBase *Handles_getWithFlag(Handles *self, HANDLE h, uint16_t flag, HandleMeta *metaOut);
bool Handles_setInformation(Handles *self, HANDLE h, uint32_t mask, uint32_t value);
bool Handles_getInformation(Handles *self, HANDLE h, uint32_t *outFlags);
bool Handles_duplicateTo(Handles *self, HANDLE src, Handles *dst, HANDLE *out, uint32_t desiredAccess, bool inherit,
						 uint32_t options);

// Names are UTF-16 code units; nameLen is in code units.
typedef struct {
	ObjectBase *obj;
	bool permanent;
	uint16_t *name;
	size_t nameLen;
} Namespace_Entry;

typedef struct {
	pthread_rwlock_t m;
	// Keyed by the raw UTF-16 bytes; values are Namespace_Entry *.
	hashmap *table;
} Namespace;

void Namespace_init(Namespace *self);
void Namespace_free(Namespace *self);
bool Namespace_insert(Namespace *self, const uint16_t *name, size_t nameLen, ObjectBase *obj, bool permanent);
void Namespace_remove(Namespace *self, ObjectBase *obj);
ObjectBase *Namespace_get(Namespace *self, const uint16_t *name, size_t nameLen);
ObjectBase *Namespace_getAs(Namespace *self, const uint16_t *name, size_t nameLen, ObjectType type);
// `make` returns a new reference, or NULL on failure. An empty name always creates a new object.
// *created may be NULL.
ObjectBase *Namespace_getOrCreate(Namespace *self, const uint16_t *name, size_t nameLen,
								  ObjectBase *(*make)(void *ctx), void *ctx, bool *created);

extern Namespace wibo_g_namespace;
Handles *wibo_handles(void);
