#include "passes.h"

#include <stdio.h>
#include <stdlib.h>

static void
each_map(struct passes *ps, void (*visit)(struct handle_map *m))
{
	struct handle_map *maps[] = {
		&ps->shaders, &ps->objects, &ps->pipelines, &ps->renderpasses, &ps->framebuffers,
		&ps->recordings, &ps->pools,
	};
	size_t i;

	for (i = 0; i < LENGTH(maps); i++)
		visit(maps[i]);
}

/* Without neutral: dynamic rendering only. */
int
passes_create(struct device *dev, const VkDeviceCreateInfo *requested)
{
	struct passes *ps = calloc(1, sizeof(*ps));

	if (!ps)
		return 0;
	each_map(ps, handlemap_init);
	pthread_mutex_init(&ps->lock, NULL);
	dev->passes = ps;
	if (dev->vk.CreateRenderPass2)
		neutral_create(dev, requested);
	return 1;
}

void
passes_destroy(struct device *dev)
{
	struct passes *ps = dev->passes;

	if (!ps)
		return;
	handlemap_each(&ps->framebuffers, framebuffer_drop, dev);
	handlemap_each(&ps->renderpasses, pass_drop, dev);
	handlemap_each(&ps->shaders, shader_drop, dev);
	handlemap_each(&ps->pools, recording_pool_drop, dev);
	neutral_destroy(dev);
	each_map(ps, handlemap_destroy);
	pthread_mutex_destroy(&ps->lock);
	free(ps);
	dev->passes = NULL;
}

/* Other queue reads rates: race. */
void
passes_check(struct device *dev, VkCommandBuffer cmd)
{
	if (atomic_load(&dev->split) || !recording_of(dev, cmd)->zoned)
		return;
	atomic_store(&dev->split, 1);
	fprintf(stderr, "nixly-vrs: scenes off present queue, rates native\n");
}
