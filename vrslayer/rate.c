#include "layer.h"

#define SLOT_ALIGN 256

static int
create_image(struct device *dev, struct rate_image *r)
{
	VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = VK_FORMAT_R8_UINT,
		.extent = { r->grid.width, r->grid.height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
			| VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkImageViewCreateInfo view = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = VK_FORMAT_R8_UINT,
		.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkMemoryRequirements req;

	if (dev->vk.CreateImage(dev->handle, &info, NULL, &r->image) != VK_SUCCESS)
		return 0;
	dev->vk.GetImageMemoryRequirements(dev->handle, r->image, &req);
	r->memory = device_allocate(dev, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (!r->memory || dev->vk.BindImageMemory(dev->handle, r->image, r->memory, 0)
			!= VK_SUCCESS)
		return 0;
	view.image = r->image;
	return dev->vk.CreateImageView(dev->handle, &view, NULL, &r->view) == VK_SUCCESS;
}

/* Staging slot per upload slot. */
static int
create_staging(struct device *dev, struct rate_image *r)
{
	VkBufferCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	VkMemoryRequirements req;
	void *mapped;

	r->slot_bytes = ((VkDeviceSize)r->grid.width * r->grid.height + SLOT_ALIGN - 1)
		& ~(VkDeviceSize)(SLOT_ALIGN - 1);
	info.size = r->slot_bytes * UPLOAD_SLOTS;
	if (dev->vk.CreateBuffer(dev->handle, &info, NULL, &r->staging) != VK_SUCCESS)
		return 0;
	dev->vk.GetBufferMemoryRequirements(dev->handle, r->staging, &req);
	r->staging_memory = device_allocate(dev, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
		| VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (!r->staging_memory || dev->vk.BindBufferMemory(dev->handle, r->staging,
			r->staging_memory, 0) != VK_SUCCESS)
		return 0;
	if (dev->vk.MapMemory(dev->handle, r->staging_memory, 0, VK_WHOLE_SIZE, 0,
			&mapped) != VK_SUCCESS)
		return 0;
	r->mapped = mapped;
	return 1;
}

static void
rate_free(struct device *dev, struct rate_image *r)
{
	dev->vk.DestroyImageView(dev->handle, r->view, NULL);
	dev->vk.DestroyImage(dev->handle, r->image, NULL);
	dev->vk.FreeMemory(dev->handle, r->memory, NULL);
	dev->vk.DestroyBuffer(dev->handle, r->staging, NULL);
	dev->vk.FreeMemory(dev->handle, r->staging_memory, NULL);
}

static void
rate_create(struct device *dev, struct rate_image *r, VkExtent2D size)
{
	r->size = size;
	r->grid = (VkExtent2D){
		(size.width + dev->texel.width - 1) / dev->texel.width,
		(size.height + dev->texel.height - 1) / dev->texel.height,
	};
	r->painted = UINT64_MAX;
	if (create_image(dev, r) && create_staging(dev, r)) {
		atomic_store(&r->state, RATE_FRESH);
		return;
	}
	rate_free(dev, r);
	atomic_store(&r->state, RATE_FAILED);
}

static struct rate_image *
rate_find(struct device *dev, VkExtent2D size)
{
	struct rate_image *r;
	int i;

	for (i = 0; i < RATE_SIZES; i++) {
		r = &dev->rates[i];
		if (atomic_load(&r->state) == RATE_EMPTY)
			return NULL;
		if (r->size.width == size.width && r->size.height == size.height)
			return r;
	}
	return NULL;
}

static void
rate_add(struct device *dev, VkExtent2D size)
{
	int i;

	pthread_mutex_lock(&dev->lock);
	for (i = 0; i < RATE_SIZES && atomic_load(&dev->rates[i].state); i++)
		;
	if (i < RATE_SIZES && !rate_find(dev, size))
		rate_create(dev, &dev->rates[i], size);
	pthread_mutex_unlock(&dev->lock);
}

VkImageView
rate_view(struct device *dev, VkExtent2D size)
{
	struct rate_image *r = rate_find(dev, size);

	if (!r) {
		rate_add(dev, size);
		return VK_NULL_HANDLE;
	}
	return atomic_load(&r->state) == RATE_READY ? r->view : VK_NULL_HANDLE;
}

/* First upload finished: usable everywhere. */
void
rate_settle(struct device *dev)
{
	struct rate_image *r;
	int i;

	for (i = 0; i < RATE_SIZES; i++) {
		r = &dev->rates[i];
		if (atomic_load(&r->state) == RATE_UPLOADING
				&& timing_passed(dev, r->upload_mark))
			atomic_store(&r->state, RATE_READY);
	}
}

void
rate_destroy(struct device *dev)
{
	int i, state;

	for (i = 0; i < RATE_SIZES; i++) {
		state = atomic_load(&dev->rates[i].state);
		if (state != RATE_EMPTY && state != RATE_FAILED)
			rate_free(dev, &dev->rates[i]);
	}
}
