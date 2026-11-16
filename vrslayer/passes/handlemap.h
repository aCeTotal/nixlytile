#ifndef NIXLY_VRS_HANDLEMAP_H
#define NIXLY_VRS_HANDLEMAP_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

struct slots;

/* Handle to value; lock-free reads. */
struct handle_map {
	_Atomic(struct slots *) live;
	struct slots *retired;
	pthread_mutex_t lock;
	uint32_t count;
	uint32_t used;              /* entries plus tombstones */
};

void handlemap_init(struct handle_map *m);
void handlemap_destroy(struct handle_map *m);
uint64_t handlemap_get(struct handle_map *m, uint64_t key);
int handlemap_put(struct handle_map *m, uint64_t key, uint64_t value);
uint64_t handlemap_take(struct handle_map *m, uint64_t key);
void handlemap_each(struct handle_map *m, void (*visit)(uint64_t value, void *ctx),
	void *ctx);

#endif
