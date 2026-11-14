#ifndef NX_VULKAN_DEVICE_DEVICE_H
#define NX_VULKAN_DEVICE_DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/render/drm_format_set.h"

struct nx_vk_format_props;

struct nx_vk_instance {
	VkInstance instance;
	VkDebugUtilsMessengerEXT messenger;

	struct {
		PFN_vkCreateDebugUtilsMessengerEXT createDebugUtilsMessengerEXT;
		PFN_vkDestroyDebugUtilsMessengerEXT destroyDebugUtilsMessengerEXT;
	} api;
};

// DMA-BUF modifier purpose.
enum nx_vk_modifier_usage {
	NX_VK_MODIFIER_RENDER,
	NX_VK_MODIFIER_TEXTURE,
	NX_VK_MODIFIER_USAGE_COUNT,
};

struct nx_vk_device {
	struct nx_vk_instance *instance;

	VkPhysicalDevice phdev;
	VkDevice dev;

	int drm_fd;

	bool sync_file_import_export;
	bool implicit_sync_interop;
	bool sampler_ycbcr_conversion;

	// One queue renders and transfers.
	uint32_t queue_family;
	VkQueue queue;

	struct {
		PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR;
		PFN_vkWaitSemaphoresKHR vkWaitSemaphoresKHR;
		PFN_vkGetSemaphoreCounterValueKHR vkGetSemaphoreCounterValueKHR;
		PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR;
		PFN_vkImportSemaphoreFdKHR vkImportSemaphoreFdKHR;
		PFN_vkQueueSubmit2KHR vkQueueSubmit2KHR;
	} api;

	uint32_t format_prop_count;
	struct nx_vk_format_props *format_props;
	struct nx_drm_format_set dmabuf_formats[NX_VK_MODIFIER_USAGE_COUNT];
	struct nx_drm_format_set shm_texture_formats;
};

struct nx_vk_instance *vulkan_instance_create(void);
void vulkan_instance_destroy(struct nx_vk_instance *ini);
bool vulkan_has_extension(const VkExtensionProperties *avail, uint32_t avail_len,
	const char *name);

// VK_NULL_HANDLE when nothing matches.
VkPhysicalDevice vulkan_find_drm_phdev(struct nx_vk_instance *ini, int drm_fd);
int vulkan_open_phdev_drm_fd(VkPhysicalDevice phdev);

struct nx_vk_device *vulkan_device_create(struct nx_vk_instance *ini,
	VkPhysicalDevice phdev);
void vulkan_device_destroy(struct nx_vk_device *dev);
void vulkan_device_probe_caps(struct nx_vk_device *dev, const VkExtensionProperties *avail,
	uint32_t avail_len);

// -1 when no type fits.
int vulkan_find_mem_type(struct nx_vk_device *dev, VkMemoryPropertyFlags flags,
	uint32_t req_bits);
// Allocated and bound, or NULL.
VkDeviceMemory vulkan_alloc_image_memory(struct nx_vk_device *dev, VkImage image,
	VkMemoryPropertyFlags flags);

const char *vulkan_strerror(VkResult err);

#define nx_vk_error(fmt, res) \
	nx_log(NX_ERROR, fmt ": %s (%d)", vulkan_strerror(res), res)

#endif
