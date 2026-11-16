#include "passes.h"

#include <stdlib.h>
#include <string.h>

/* Matches the view's image. */
static VkFramebufferAttachmentImageInfo
rate_image(VkExtent2D grid)
{
	return (VkFramebufferAttachmentImageInfo){
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO,
		.usage = RATE_USAGE,
		.width = grid.width,
		.height = grid.height,
		.layerCount = 1,
	};
}

static VkExtent2D
grid_of(const struct device *dev, const struct framebuffer *f, VkImageView view)
{
	if (view == dev->passes->neutral.image.view)
		return dev->passes->neutral.grid;
	return texel_grid(dev->texel, (VkExtent2D){ f->create.width, f->create.height });
}

/* Outlives pass, fits compatible twins. */
static VkFramebuffer
create_twin(struct device *dev, const struct framebuffer *f, struct twin_spec spec)
{
	VkImageView views[ATTACH_MAX];
	VkFramebufferAttachmentImageInfo images[ATTACH_MAX];
	VkFramebufferAttachmentsCreateInfo list = {
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO,
		.attachmentImageInfoCount = f->create.attachmentCount + 1,
		.pAttachmentImageInfos = images,
	};
	VkFramebufferCreateInfo create = f->create;
	VkFramebuffer twin = VK_NULL_HANDLE;
	uint32_t n = f->create.attachmentCount;

	create.renderPass = spec.pass;
	create.attachmentCount = n + 1;
	if (f->views) {
		memcpy(views, f->views, n * sizeof(*views));
		views[n] = spec.view;
		create.pAttachments = views;
	} else {
		memcpy(images, f->images, n * sizeof(*images));
		images[n] = rate_image(grid_of(dev, f, spec.view));
		create.pNext = &list;
	}
	if (dev->vk.CreateFramebuffer(dev->handle, &create, NULL, &twin) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return twin;
}

static size_t
formats_of(const VkFramebufferAttachmentsCreateInfo *list)
{
	uint32_t i;
	size_t n = 0;

	for (i = 0; i < list->attachmentImageInfoCount; i++)
		n += list->pAttachmentImageInfos[i].viewFormatCount;
	return n;
}

/* Deep copy: views or images. */
static struct framebuffer *
record(const VkFramebufferCreateInfo *in)
{
	const VkFramebufferAttachmentsCreateInfo *list = (const void *)
		chain_find(in->pNext, VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO);
	int imageless = (in->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) != 0;
	uint32_t i, n = in->attachmentCount;
	struct framebuffer *f;
	VkFormat *formats;

	if (n >= ATTACH_MAX || (imageless && !list))
		return NULL;
	f = calloc(1, sizeof(*f) + (imageless ? n * sizeof(*f->images)
		+ formats_of(list) * sizeof(*formats) : n * sizeof(*f->views)));
	if (!f)
		return NULL;
	f->create = *in;
	f->create.pNext = NULL;
	f->create.pAttachments = NULL;
	if (!imageless) {
		f->views = memcpy(f + 1, in->pAttachments, n * sizeof(*f->views));
		return f;
	}
	f->images = memcpy(f + 1, list->pAttachmentImageInfos, n * sizeof(*f->images));
	formats = (VkFormat *)(f->images + n);
	for (i = 0; i < n; i++) {
		f->images[i].pNext = NULL;
		f->images[i].pViewFormats = memcpy(formats, list->pAttachmentImageInfos[i]
			.pViewFormats, f->images[i].viewFormatCount * sizeof(*formats));
		formats += f->images[i].viewFormatCount;
	}
	return f;
}

static void
drop_record(struct device *dev, struct framebuffer *f)
{
	dev->vk.DestroyFramebuffer(dev->handle, f->neutral, NULL);
	dev->vk.DestroyFramebuffer(dev->handle, atomic_load(&f->zoned), NULL);
	free(f);
}

/* Neutral grid covers framebuffer. */
static int
covered(const struct device *dev, const VkFramebufferCreateInfo *in)
{
	VkExtent2D grid = dev->passes->neutral.grid;

	return in->width <= grid.width * dev->texel.width
		&& in->height <= grid.height * dev->texel.height;
}

static void
track(struct device *dev, const VkFramebufferCreateInfo *in, VkFramebuffer handle)
{
	const struct pass *p = pass_of(dev, in->renderPass);
	struct framebuffer *f = p && covered(dev, in) ? record(in) : NULL;

	if (!f)
		return;
	f->neutral = create_twin(dev, f,
		(struct twin_spec){ p->twin, dev->passes->neutral.image.view });
	if (!f->neutral || !handlemap_put(&dev->passes->framebuffers, (uint64_t)handle,
			(uint64_t)(uintptr_t)f))
		drop_record(dev, f);
}

struct framebuffer *
framebuffer_of(struct device *dev, VkFramebuffer handle)
{
	if (!handle)
		return NULL;
	return (struct framebuffer *)(uintptr_t)
		handlemap_get(&dev->passes->framebuffers, (uint64_t)handle);
}

/* Zoned twin, built under lock. */
static void
adopt(struct device *dev, struct framebuffer *f, struct twin_spec spec)
{
	VkFramebuffer twin = create_twin(dev, f, spec);

	if (!twin)
		return;
	atomic_store(&f->zoned, twin);
	atomic_store(&f->zoned_view, spec.view);
}

/* Neutral if no zoned twin. */
struct twin
framebuffer_twin(struct device *dev, struct framebuffer *f, struct twin_spec spec)
{
	struct twin neutral = { f->neutral, dev->passes->neutral.image.view };
	struct twin twin = neutral;

	if (spec.view == neutral.view)
		return neutral;
	if (atomic_load(&f->zoned_view) == spec.view)
		return (struct twin){ atomic_load(&f->zoned), spec.view };
	pthread_mutex_lock(&dev->passes->lock);
	if (!atomic_load(&f->zoned_view))
		adopt(dev, f, spec);
	if (atomic_load(&f->zoned_view) == spec.view)
		twin = (struct twin){ atomic_load(&f->zoned), spec.view };
	pthread_mutex_unlock(&dev->passes->lock);
	return twin;
}

void
framebuffer_drop(uint64_t value, void *dev)
{
	drop_record(dev, (struct framebuffer *)(uintptr_t)value);
}

VKAPI_ATTR VkResult VKAPI_CALL
framebuffer_create(VkDevice device, const VkFramebufferCreateInfo *create,
	const VkAllocationCallbacks *alloc, VkFramebuffer *out)
{
	struct device *dev = device_of(device);
	VkResult created = dev->vk.CreateFramebuffer(device, create, alloc, out);

	if (created == VK_SUCCESS)
		track(dev, create, *out);
	return created;
}

VKAPI_ATTR void VKAPI_CALL
framebuffer_destroy(VkDevice device, VkFramebuffer handle,
	const VkAllocationCallbacks *alloc)
{
	struct device *dev = device_of(device);
	struct framebuffer *f = handle ? (struct framebuffer *)(uintptr_t)
		handlemap_take(&dev->passes->framebuffers, (uint64_t)handle) : NULL;

	if (f)
		drop_record(dev, f);
	dev->vk.DestroyFramebuffer(device, handle, alloc);
}
