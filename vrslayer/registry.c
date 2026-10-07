#include "layer.h"

#define REGISTRY_SLOTS 32

/* Keyed by loader dispatch table. */
static _Atomic(const void *) keys[REGISTRY_SLOTS];
static void *objects[REGISTRY_SLOTS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static const void *
key_of(const void *handle)
{
	return *(const void *const *)handle;
}

int
registry_add(const void *handle, void *object)
{
	int i;

	pthread_mutex_lock(&lock);
	for (i = 0; i < REGISTRY_SLOTS && atomic_load(&keys[i]); i++)
		;
	if (i < REGISTRY_SLOTS) {
		objects[i] = object;
		atomic_store(&keys[i], key_of(handle));
	}
	pthread_mutex_unlock(&lock);
	return i < REGISTRY_SLOTS;
}

void
registry_remove(const void *handle)
{
	const void *key = key_of(handle);
	int i;

	pthread_mutex_lock(&lock);
	for (i = 0; i < REGISTRY_SLOTS; i++)
		if (atomic_load(&keys[i]) == key)
			atomic_store(&keys[i], NULL);
	pthread_mutex_unlock(&lock);
}

void *
registry_find(const void *handle)
{
	const void *key = key_of(handle);
	int i;

	for (i = 0; i < REGISTRY_SLOTS; i++)
		if (atomic_load(&keys[i]) == key)
			return objects[i];
	return NULL;
}
