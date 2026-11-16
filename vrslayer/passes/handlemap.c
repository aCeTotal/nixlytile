#include "handlemap.h"

#include <stdlib.h>

#define EMPTY      0ULL
#define TOMBSTONE  UINT64_MAX
#define FIRST_SIZE 64u
#define SPARSE     4u          /* slots per entry after growth */
#define MIN_SPARSE 2u          /* slots per entry before growth */
#define MIX        0xff51afd7ed558ccdULL
#define MIX_SHIFT  33

struct slot {
	_Atomic uint64_t key;
	_Atomic uint64_t value;
};

/* Retired tables stay readable. */
struct slots {
	uint32_t mask;
	struct slots *older;
	struct slot slot[];
};

/* Murmur3 finalizer. */
static uint32_t
hash(uint64_t key)
{
	key ^= key >> MIX_SHIFT;
	key *= MIX;
	key ^= key >> MIX_SHIFT;
	return (uint32_t)key;
}

/* Key's slot, else first empty. */
static struct slot *
probe(struct slots *s, uint64_t key)
{
	uint32_t i = hash(key);
	struct slot *slot;
	uint64_t k;

	do {
		slot = &s->slot[i++ & s->mask];
		k = atomic_load_explicit(&slot->key, memory_order_acquire);
	} while (k != key && k != EMPTY);
	return slot;
}

/* Key's slot, else first reusable. */
static struct slot *
probe_put(struct slots *s, uint64_t key)
{
	uint32_t i = hash(key);
	struct slot *slot, *tomb = NULL;
	uint64_t k;

	do {
		slot = &s->slot[i++ & s->mask];
		k = atomic_load_explicit(&slot->key, memory_order_relaxed);
		tomb = !tomb && k == TOMBSTONE ? slot : tomb;
	} while (k != key && k != EMPTY);
	return k != key && tomb ? tomb : slot;
}

static int
occupied(uint64_t key)
{
	return key != EMPTY && key != TOMBSTONE;
}

/* Value first: no torn reads. */
static void
store(struct slot *slot, uint64_t key, uint64_t value)
{
	atomic_store_explicit(&slot->value, value, memory_order_relaxed);
	atomic_store_explicit(&slot->key, key, memory_order_release);
}

static void
carry(struct slots *s, const struct slot *from)
{
	uint64_t key = atomic_load_explicit(&from->key, memory_order_relaxed);

	if (occupied(key))
		store(probe(s, key), key,
			atomic_load_explicit(&from->value, memory_order_relaxed));
}

static struct slots *
rehash(const struct slots *old, uint32_t size)
{
	struct slots *s = calloc(1, sizeof(*s) + size * sizeof(s->slot[0]));
	uint32_t i;

	if (!s)
		return NULL;
	s->mask = size - 1;
	for (i = 0; old && i <= old->mask; i++)
		carry(s, &old->slot[i]);
	return s;
}

/* Load stays at most half. */
static struct slots *
room(struct handle_map *m)
{
	struct slots *s = atomic_load_explicit(&m->live, memory_order_relaxed), *fresh;
	uint32_t size = FIRST_SIZE;

	if (s && MIN_SPARSE * (m->used + 1) <= s->mask + 1)
		return s;
	while (size < SPARSE * (m->count + 1))
		size *= 2;
	fresh = rehash(s, size);
	if (!fresh)
		return NULL;
	if (s) {
		s->older = m->retired;
		m->retired = s;
	}
	m->used = m->count;
	atomic_store_explicit(&m->live, fresh, memory_order_release);
	return fresh;
}

void
handlemap_init(struct handle_map *m)
{
	atomic_init(&m->live, NULL);
	m->retired = NULL;
	m->count = m->used = 0;
	pthread_mutex_init(&m->lock, NULL);
}

void
handlemap_destroy(struct handle_map *m)
{
	struct slots *s, *older;

	free(atomic_load(&m->live));
	for (s = m->retired; s; s = older) {
		older = s->older;
		free(s);
	}
	pthread_mutex_destroy(&m->lock);
}

/* 0 when absent. */
uint64_t
handlemap_get(struct handle_map *m, uint64_t key)
{
	struct slots *s = atomic_load_explicit(&m->live, memory_order_acquire);
	struct slot *slot;

	if (!s)
		return 0;
	slot = probe(s, key);
	if (atomic_load_explicit(&slot->key, memory_order_acquire) != key)
		return 0;
	return atomic_load_explicit(&slot->value, memory_order_relaxed);
}

/* Tombstone reuse keeps used flat. */
static void
account(struct handle_map *m, uint64_t old, uint64_t key)
{
	if (old == key)
		return;
	m->count++;
	m->used += old == EMPTY;
}

int
handlemap_put(struct handle_map *m, uint64_t key, uint64_t value)
{
	struct slots *s;
	struct slot *slot = NULL;

	pthread_mutex_lock(&m->lock);
	s = room(m);
	if (s)
		slot = probe_put(s, key);
	if (slot) {
		account(m, atomic_load_explicit(&slot->key, memory_order_relaxed), key);
		store(slot, key, value);
	}
	pthread_mutex_unlock(&m->lock);
	return slot != NULL;
}

/* 0 when absent. */
uint64_t
handlemap_take(struct handle_map *m, uint64_t key)
{
	struct slots *s;
	struct slot *slot;
	uint64_t value = 0;

	pthread_mutex_lock(&m->lock);
	s = atomic_load_explicit(&m->live, memory_order_relaxed);
	slot = s ? probe(s, key) : NULL;
	if (slot && atomic_load_explicit(&slot->key, memory_order_relaxed) == key) {
		value = atomic_load_explicit(&slot->value, memory_order_relaxed);
		atomic_store_explicit(&slot->key, TOMBSTONE, memory_order_release);
		m->count--;
	}
	pthread_mutex_unlock(&m->lock);
	return value;
}

static void
visit_slot(const struct slot *slot, void (*visit)(uint64_t value, void *ctx),
	void *ctx)
{
	if (occupied(atomic_load_explicit(&slot->key, memory_order_relaxed)))
		visit(atomic_load_explicit(&slot->value, memory_order_relaxed), ctx);
}

void
handlemap_each(struct handle_map *m, void (*visit)(uint64_t value, void *ctx),
	void *ctx)
{
	struct slots *s;
	uint32_t i;

	pthread_mutex_lock(&m->lock);
	s = atomic_load_explicit(&m->live, memory_order_relaxed);
	for (i = 0; s && i <= s->mask; i++)
		visit_slot(&s->slot[i], visit, ctx);
	pthread_mutex_unlock(&m->lock);
}
