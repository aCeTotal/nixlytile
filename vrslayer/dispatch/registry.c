#include "layer.h"

#define REGISTRY_BITS  10
#define REGISTRY_SLOTS (1u << REGISTRY_BITS)
#define HASH_MUL       0x9e3779b97f4a7c15ull
#define HASH_BITS      64
#define ALIGN_BITS     4
#define TOMB           ((const void *)1)

/* Open addressing by dispatch table. */
static _Atomic(const void *) keys[REGISTRY_SLOTS];
static void *objects[REGISTRY_SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static const void *
key_of(const void *handle)
{
	return *(const void *const *)handle;
}

static uint32_t
home_of(const void *key)
{
	return (uint32_t)(((uintptr_t)key >> ALIGN_BITS) * HASH_MUL
		>> (HASH_BITS - REGISTRY_BITS));
}

static int
occupied(uint32_t at)
{
	const void *k = atomic_load(&keys[at]);

	return k && k != TOMB;
}

int
registry_add(const void *handle, void *object)
{
	const void *key = key_of(handle);
	uint32_t i, at = home_of(key);

	pthread_mutex_lock(&lock);
	for (i = 0; i < REGISTRY_SLOTS && occupied(at); i++)
		at = (at + 1) % REGISTRY_SLOTS;
	if (i < REGISTRY_SLOTS) {
		objects[at] = object;
		atomic_store(&keys[at], key);
	}
	pthread_mutex_unlock(&lock);
	return i < REGISTRY_SLOTS;
}

static int
locate(const void *key)
{
	uint32_t i, at = home_of(key);
	const void *k = atomic_load(&keys[at]);

	for (i = 1; i < REGISTRY_SLOTS && k && k != key; i++) {
		at = (at + 1) % REGISTRY_SLOTS;
		k = atomic_load(&keys[at]);
	}
	return k == key ? (int)at : -1;
}

void
registry_remove(const void *handle)
{
	int at;

	pthread_mutex_lock(&lock);
	at = locate(key_of(handle));
	if (at >= 0)
		atomic_store(&keys[at], TOMB);
	pthread_mutex_unlock(&lock);
}

void *
registry_find(const void *handle)
{
	int at = locate(key_of(handle));

	return at >= 0 ? objects[at] : NULL;
}
