#include "layer.h"

/* Bitmask of types having flags. */
static uint32_t
types_with(const struct device *dev, VkMemoryPropertyFlags flags)
{
	uint32_t i, mask = 0;

	for (i = 0; i < dev->memory.memoryTypeCount; i++)
		mask |= (uint32_t)((dev->memory.memoryTypes[i].propertyFlags & flags) == flags) << i;
	return mask;
}

static uint32_t
memory_type(const struct device *dev, uint32_t bits, VkMemoryPropertyFlags flags)
{
	uint32_t fit = bits & types_with(dev, flags);

	return fit ? (uint32_t)__builtin_ctz(fit) : UINT32_MAX;
}

/* Preferred flags, else any type. */
static VkDeviceMemory
allocate(struct device *dev, VkMemoryRequirements req, VkMemoryPropertyFlags flags)
{
	VkMemoryAllocateInfo allocation = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = memory_type(dev, req.memoryTypeBits, flags),
	};
	VkDeviceMemory memory = VK_NULL_HANDLE;

	if (allocation.memoryTypeIndex == UINT32_MAX)
		allocation.memoryTypeIndex = memory_type(dev, req.memoryTypeBits, 0);
	if (dev->vk.AllocateMemory(dev->handle, &allocation, NULL, &memory) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return memory;
}

/* Zeroed on failure. */
struct gpu_buffer
gpu_buffer(struct device *dev, const VkBufferCreateInfo *create, VkMemoryPropertyFlags flags)
{
	struct gpu_buffer b = {0};
	VkMemoryRequirements req;

	if (dev->vk.CreateBuffer(dev->handle, create, NULL, &b.buffer) != VK_SUCCESS)
		return (struct gpu_buffer){0};
	dev->vk.GetBufferMemoryRequirements(dev->handle, b.buffer, &req);
	b.memory = allocate(dev, req, flags);
	if (!b.memory || dev->vk.BindBufferMemory(dev->handle, b.buffer, b.memory, 0)
			!= VK_SUCCESS)
		gpu_buffer_free(dev, &b);
	return b;
}

void
gpu_buffer_free(struct device *dev, struct gpu_buffer *b)
{
	dev->vk.DestroyBuffer(dev->handle, b->buffer, NULL);
	dev->vk.FreeMemory(dev->handle, b->memory, NULL);
	*b = (struct gpu_buffer){0};
}

/* Device-local; zeroed on failure. */
struct gpu_image
gpu_image(struct device *dev, const VkImageCreateInfo *create)
{
	VkImageViewCreateInfo view = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = create->format,
		.subresourceRange = WHOLE_COLOR,
	};
	struct gpu_image g = {0};
	VkMemoryRequirements req;

	if (dev->vk.CreateImage(dev->handle, create, NULL, &g.image) != VK_SUCCESS)
		return (struct gpu_image){0};
	dev->vk.GetImageMemoryRequirements(dev->handle, g.image, &req);
	g.memory = allocate(dev, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	view.image = g.image;
	if (!g.memory || dev->vk.BindImageMemory(dev->handle, g.image, g.memory, 0) != VK_SUCCESS
			|| dev->vk.CreateImageView(dev->handle, &view, NULL, &g.view) != VK_SUCCESS)
		gpu_image_free(dev, &g);
	return g;
}

void
gpu_image_free(struct device *dev, struct gpu_image *g)
{
	dev->vk.DestroyImageView(dev->handle, g->view, NULL);
	dev->vk.DestroyImage(dev->handle, g->image, NULL);
	dev->vk.FreeMemory(dev->handle, g->memory, NULL);
	*g = (struct gpu_image){0};
}
