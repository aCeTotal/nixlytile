#include "passes.h"

#include <stdlib.h>
#include <string.h>

/* Buffer in its pool's ring. */
struct member {
	struct recording recording; /* first: the map's value */
	struct member *prev, *next;
	VkCommandBuffer cmd;
};

PER_THREAD struct recording_hit recording_hits[RECORDING_HITS];
_Atomic uint64_t recording_epoch;

static uint64_t
key_of(VkCommandBuffer cmd)
{
	return (uint64_t)(uintptr_t)cmd;
}

/* Epoch first; absence never cached. */
struct recording *
recording_find(struct device *dev, VkCommandBuffer cmd)
{
	uint64_t epoch = atomic_load_explicit(&recording_epoch, memory_order_relaxed);
	struct recording *r = (struct recording *)(uintptr_t)
		handlemap_get(&dev->passes->recordings, key_of(cmd));

	if (!r)
		return NULL;
	memmove(&recording_hits[1], &recording_hits[0],
		sizeof(recording_hits) - sizeof(recording_hits[0]));
	recording_hits[0] = (struct recording_hit){ cmd, r, epoch };
	return r;
}

/* Before removal: thread hits die. */
static void
expire(void)
{
	atomic_fetch_add(&recording_epoch, 1);
}

/* Sentinel of pool's ring. */
static struct member *
ring_of(struct device *dev, VkCommandPool pool)
{
	struct member *ring = (struct member *)(uintptr_t)
		handlemap_get(&dev->passes->pools, (uint64_t)pool);

	if (ring)
		return ring;
	ring = malloc(sizeof(*ring));
	if (!ring)
		return NULL;
	ring->prev = ring->next = ring;
	if (handlemap_put(&dev->passes->pools, (uint64_t)pool, (uint64_t)(uintptr_t)ring))
		return ring;
	free(ring);
	return NULL;
}

static int
track(struct device *dev, struct member *ring, VkCommandBuffer cmd)
{
	struct member *m = malloc(sizeof(*m));

	if (!m)
		return 0;
	*m = (struct member){ .prev = ring->prev, .next = ring, .cmd = cmd };
	if (!handlemap_put(&dev->passes->recordings, key_of(cmd), (uint64_t)(uintptr_t)m)) {
		free(m);
		return 0;
	}
	ring->prev->next = m;
	ring->prev = m;
	return 1;
}

static void
untrack(struct device *dev, VkCommandBuffer cmd)
{
	struct member *m;

	if (!cmd)
		return;
	expire();
	m = (struct member *)(uintptr_t)handlemap_take(&dev->passes->recordings, key_of(cmd));
	if (!m)
		return;
	m->prev->next = m->next;
	m->next->prev = m->prev;
	free(m);
}

static void
drop_ring(struct device *dev, struct member *ring)
{
	struct member *m, *next;

	expire();
	for (m = ring->next; m != ring; m = next) {
		next = m->next;
		handlemap_take(&dev->passes->recordings, key_of(m->cmd));
		free(m);
	}
	free(ring);
}

void
recording_pool_drop(uint64_t value, void *dev)
{
	drop_ring(dev, (struct member *)(uintptr_t)value);
}

/* Untracked buffers never escape. */
VKAPI_ATTR VkResult VKAPI_CALL
recording_allocate(VkDevice device, const VkCommandBufferAllocateInfo *request,
	VkCommandBuffer *out)
{
	struct device *dev = device_of(device);
	VkResult made = dev->vk.AllocateCommandBuffers(device, request, out);
	struct member *ring;
	uint32_t i = 0;

	if (made != VK_SUCCESS)
		return made;
	ring = ring_of(dev, request->commandPool);
	while (ring && i < request->commandBufferCount && track(dev, ring, out[i]))
		i++;
	if (i == request->commandBufferCount)
		return VK_SUCCESS;
	while (i--)
		untrack(dev, out[i]);
	dev->vk.FreeCommandBuffers(device, request->commandPool,
		request->commandBufferCount, out);
	memset(out, 0, request->commandBufferCount * sizeof(*out));
	return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VKAPI_ATTR void VKAPI_CALL
recording_free(VkDevice device, VkCommandPool pool, uint32_t count,
	const VkCommandBuffer *cmds)
{
	struct device *dev = device_of(device);
	uint32_t i;

	for (i = 0; i < count; i++)
		untrack(dev, cmds[i]);
	dev->vk.FreeCommandBuffers(device, pool, count, cmds);
}

VKAPI_ATTR void VKAPI_CALL
recording_pool_destroy(VkDevice device, VkCommandPool pool,
	const VkAllocationCallbacks *alloc)
{
	struct device *dev = device_of(device);
	struct member *ring = pool ? (struct member *)(uintptr_t)
		handlemap_take(&dev->passes->pools, (uint64_t)pool) : NULL;

	if (ring)
		drop_ring(dev, ring);
	dev->vk.DestroyCommandPool(device, pool, alloc);
}
