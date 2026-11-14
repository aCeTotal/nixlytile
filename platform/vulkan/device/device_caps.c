#include <assert.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/render/dmabuf.h"
#include "platform/vulkan/device/device.h"

static uint32_t find_graphics_queue_family(VkPhysicalDevice phdev) {
	uint32_t count;
	vkGetPhysicalDeviceQueueFamilyProperties(phdev, &count, NULL);
	assert(count > 0);
	VkQueueFamilyProperties props[count];
	vkGetPhysicalDeviceQueueFamilyProperties(phdev, &count, props);

	uint32_t i = 0;
	while (i < count && !(props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
		i++;
	}
	assert(i < count);
	return i;
}

static VkExternalSemaphoreFeatureFlags sync_fd_semaphore_features(VkPhysicalDevice phdev) {
	const VkPhysicalDeviceExternalSemaphoreInfo info = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
		.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	};
	VkExternalSemaphoreProperties props = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
	};
	vkGetPhysicalDeviceExternalSemaphoreProperties(phdev, &info, &props);
	return props.externalSemaphoreFeatures;
}

static void init_sync_caps(struct nx_vk_device *dev, VkExternalSemaphoreFeatureFlags features) {
	bool exportable = features & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT;
	bool importable = features & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
	if (!exportable) {
		nx_log(NX_DEBUG, "VkSemaphore is not exportable to a sync_file");
	}
	if (!importable) {
		nx_log(NX_DEBUG, "VkSemaphore is not importable from a sync_file");
	}

	bool dmabuf_sync_file = dmabuf_check_sync_file_import_export();
	if (!dmabuf_sync_file) {
		nx_log(NX_DEBUG, "DMA-BUF sync_file import/export not supported");
	}

	dev->sync_file_import_export = exportable && importable;
	dev->implicit_sync_interop = dev->sync_file_import_export && dmabuf_sync_file;
	if (dev->implicit_sync_interop) {
		nx_log(NX_INFO, "Implicit sync interop supported");
		return;
	}
	nx_log(NX_ERROR, "Vulkan: implicit_sync_interop not available "
		"(exportable_semaphore=%d, importable_semaphore=%d, "
		"dmabuf_sync_file_import_export=%d) — "
		"DMA-BUF sync will use blocking poll() fallback. "
		"Check kernel version and Vulkan driver.",
		exportable, importable, dmabuf_sync_file);
}

static bool has_sampler_ycbcr_conversion(VkPhysicalDevice phdev) {
	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
	};
	VkPhysicalDeviceFeatures2 features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &ycbcr_features,
	};
	vkGetPhysicalDeviceFeatures2(phdev, &features);
	return ycbcr_features.samplerYcbcrConversion;
}

// Queue, sync and YCbCr support.
void vulkan_device_probe_caps(struct nx_vk_device *dev, const VkExtensionProperties *avail,
		uint32_t avail_len) {
	dev->queue_family = find_graphics_queue_family(dev->phdev);

	VkExternalSemaphoreFeatureFlags semaphore_features = 0;
	if (vulkan_has_extension(avail, avail_len, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)) {
		semaphore_features = sync_fd_semaphore_features(dev->phdev);
	}
	init_sync_caps(dev, semaphore_features);

	dev->sampler_ycbcr_conversion = has_sampler_ycbcr_conversion(dev->phdev);
	nx_log(NX_DEBUG, "Sampler YCbCr conversion %s",
		dev->sampler_ycbcr_conversion ? "supported" : "not supported");
}
