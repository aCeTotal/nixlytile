#include "layer.h"

#include <stdio.h>

#define STAGING_MEMORY (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)

/* Uploads never allocate. */
static struct gpu_buffer
create_staging(struct device *dev, VkExtent2D grid)
{
	VkBufferCreateInfo create = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = UPLOAD_SLOTS * grid_bytes(grid),
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};

	return gpu_buffer(dev, &create, STAGING_MEMORY);
}

static int
rate_other(const struct rate_image *r, uint64_t key)
{
	uint64_t held = atomic_load(&r->key);

	return held && held != key;
}

/* Ordered fill; slots never free. */
static struct rate_image *
rate_probe(struct fovea *f, uint64_t key)
{
	int i = 0;

	while (i < RATE_SIZES && rate_other(&f->rates[i], key))
		i++;
	return i < RATE_SIZES ? &f->rates[i] : NULL;
}

/* Losers probe again next pass. */
static void
rate_claim(struct fovea *f, struct rate_image *r, uint64_t key)
{
	uint64_t empty = 0;

	if (atomic_compare_exchange_strong(&r->key, &empty, key))
		atomic_store(&f->asked, 1);
}

static VkImageView
view_of(const struct rate_image *r, VkImageView pending)
{
	switch (atomic_load(&r->state)) {
	case RATE_READY:
		return atomic_load(&r->native) ? VK_NULL_HANDLE : r->attachment.view;
	case RATE_FAILED:
		return VK_NULL_HANDLE;
	default:
		return pending;
	}
}

/* Zone map, pending, or none: native. */
VkImageView
rate_view(struct device *dev, VkExtent2D size, VkImageView pending)
{
	struct fovea *f = &dev->fovea;
	uint64_t key = extent_key(size);
	enum upload_state upload = atomic_load(&f->upload);
	struct rate_image *r;

	if (upload == UPLOAD_FAILED)
		return VK_NULL_HANDLE;
	if (upload != UPLOAD_READY)
		return pending;
	r = rate_probe(f, key);
	if (!r)
		return VK_NULL_HANDLE;
	if (atomic_load(&r->key) != key) {
		rate_claim(f, r, key);
		return pending;
	}
	return view_of(r, pending);
}

static int
rate_create(struct device *dev, struct rate_image *r)
{
	VkImageCreateInfo create = rate_image_create(r->grid);

	r->attachment = gpu_image(dev, &create);
	if (!r->attachment.view)
		return 0;
	r->staging = create_staging(dev, r->grid);
	return r->staging.buffer && dev->vk.MapMemory(dev->handle, r->staging.memory, 0,
		VK_WHOLE_SIZE, 0, (void **)&r->mapped) == VK_SUCCESS;
}

/* Failure leaves the size native. */
static void
build_size(struct device *dev, struct rate_image *r)
{
	if (atomic_load(&r->state) != RATE_WANTED)
		return;
	r->size = extent_of(atomic_load(&r->key));
	r->grid = texel_grid(dev->texel, r->size);
	if (!rate_create(dev, r)) {
		gpu_image_free(dev, &r->attachment);
		gpu_buffer_free(dev, &r->staging);
		atomic_store(&r->state, RATE_FAILED);
		fprintf(stderr, "nixly-vrs: no rate image for %ux%u, size native\n",
			r->size.width, r->size.height);
		return;
	}
	atomic_store(&r->state, RATE_FRESH);
	atomic_store(&dev->fovea.dirty, 1);
}

/* Worker: every claimed size. */
void
rate_build(struct device *dev)
{
	struct fovea *f = &dev->fovea;
	int i;

	for (i = 0; i < RATE_SIZES && atomic_load(&f->rates[i].key)
			&& !atomic_load(&f->worker.quit); i++)
		build_size(dev, &f->rates[i]);
}

/* Device teardown; worker joined. */
void
rate_destroy(struct device *dev)
{
	int i;

	for (i = 0; i < RATE_SIZES; i++) {
		gpu_image_free(dev, &dev->fovea.rates[i].attachment);
		gpu_buffer_free(dev, &dev->fovea.rates[i].staging);
	}
}
