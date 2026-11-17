#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/device/device.h"
#include "platform/vulkan/device/format.h"

#define MAX_DEVICE_EXTENSIONS 32

#define load_proc(dev, name) ((PFN_##name)load_device_proc((dev)->dev, #name))

// DMA-BUF import, timelines and sync2.
static const char *const required_extensions[] = {
	VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
	VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
	VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
	VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
	VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
	VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
};

static const struct {
	const char *extension;
	const char *label;
} global_priority_extensions[] = {
	{ VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME, "KHR" },
	{ VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME, "EXT" },
};

static const VkDeviceQueueGlobalPriorityCreateInfo high_priority = {
	.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO,
	.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_HIGH,
};

struct extension_list {
	const char *names[MAX_DEVICE_EXTENSIONS];
	uint32_t len;
};

static void add_extension(struct extension_list *list, const char *name) {
	assert(list->len < MAX_DEVICE_EXTENSIONS);
	list->names[list->len++] = name;
}

static VkResult create_logical_device(struct nx_vk_device *dev,
		const struct extension_list *extensions, const void *queue_next) {
	const float prio = 1.f;
	VkDeviceQueueCreateInfo qinfo = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.pNext = queue_next,
		.queueFamilyIndex = dev->queue_family,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};

	VkPhysicalDeviceSamplerYcbcrConversionFeatures sampler_ycbcr_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
		.samplerYcbcrConversion = dev->sampler_ycbcr_conversion,
	};
	VkPhysicalDeviceSynchronization2FeaturesKHR sync2_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
		.pNext = &sampler_ycbcr_features,
		.synchronization2 = VK_TRUE,
	};
	VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timeline_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
		.pNext = &sync2_features,
		.timelineSemaphore = VK_TRUE,
	};
	VkDeviceCreateInfo dev_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &timeline_features,
		.queueCreateInfoCount = 1u,
		.pQueueCreateInfos = &qinfo,
		.enabledExtensionCount = extensions->len,
		.ppEnabledExtensionNames = extensions->names,
	};
	return vkCreateDevice(dev->phdev, &dev_info, NULL, &dev->dev);
}

static PFN_vkVoidFunction load_device_proc(VkDevice dev, const char *name) {
	PFN_vkVoidFunction proc = vkGetDeviceProcAddr(dev, name);
	if (proc == NULL) {
		abort();
	}
	return proc;
}

static void load_device_procs(struct nx_vk_device *dev) {
	dev->api.vkGetMemoryFdPropertiesKHR = load_proc(dev, vkGetMemoryFdPropertiesKHR);
	dev->api.vkWaitSemaphoresKHR = load_proc(dev, vkWaitSemaphoresKHR);
	dev->api.vkGetSemaphoreCounterValueKHR = load_proc(dev, vkGetSemaphoreCounterValueKHR);
	dev->api.vkQueueSubmit2KHR = load_proc(dev, vkQueueSubmit2KHR);
}

static void load_semaphore_fd_procs(struct nx_vk_device *dev) {
	dev->api.vkGetSemaphoreFdKHR = load_proc(dev, vkGetSemaphoreFdKHR);
	dev->api.vkImportSemaphoreFdKHR = load_proc(dev, vkImportSemaphoreFdKHR);
}

static bool query_format_props(struct nx_vk_device *dev) {
	size_t len;
	const struct nx_vk_format *formats = vulkan_get_format_list(&len);
	dev->format_props = calloc(len, sizeof(*dev->format_props));
	if (!dev->format_props) {
		nx_log_errno(NX_ERROR, "allocation failed");
		return false;
	}

	nx_log(NX_DEBUG, "Supported Vulkan formats:");
	for (size_t i = 0; i < len; ++i) {
		vulkan_format_props_query(dev, &formats[i]);
	}
	return true;
}

static int find_global_priority(const VkExtensionProperties *avail, uint32_t avail_len) {
	int count = sizeof(global_priority_extensions) / sizeof(global_priority_extensions[0]);
	for (int i = 0; i < count; i++) {
		if (vulkan_has_extension(avail, avail_len, global_priority_extensions[i].extension)) {
			return i;
		}
	}
	return -1;
}

static bool init_logical_device(struct nx_vk_device *dev, struct extension_list *extensions,
		int priority) {
	const void *queue_next = NULL;
	if (priority >= 0) {
		add_extension(extensions, global_priority_extensions[priority].extension);
		nx_log(NX_DEBUG, "Requesting a high-priority device queue (%s)",
			global_priority_extensions[priority].label);
		queue_next = &high_priority;
	} else {
		nx_log(NX_DEBUG, "Global priorities are not supported, "
			"falling back to regular queue priority");
	}

	VkResult res = create_logical_device(dev, extensions, queue_next);
	if (queue_next && (res == VK_ERROR_NOT_PERMITTED_EXT ||
			res == VK_ERROR_INITIALIZATION_FAILED)) {
		// Drivers may deny high priority.
		nx_log(NX_ERROR, "High-priority GPU queue denied (needs CAP_SYS_NICE), "
			"clients will delay compositor frames");
		res = create_logical_device(dev, extensions, NULL);
	} else if (queue_next && res == VK_SUCCESS) {
		nx_log(NX_INFO, "High-priority GPU queue granted");
	}
	if (res != VK_SUCCESS) {
		nx_vk_error("Failed to create vulkan device", res);
		return false;
	}
	return true;
}

struct nx_vk_device *vulkan_device_create(struct nx_vk_instance *ini,
		VkPhysicalDevice phdev) {
	uint32_t avail_extc = 0;
	VkResult res = vkEnumerateDeviceExtensionProperties(phdev, NULL, &avail_extc, NULL);
	if (res != VK_SUCCESS || avail_extc == 0) {
		nx_vk_error("Could not enumerate device extensions (1)", res);
		return NULL;
	}

	VkExtensionProperties avail[avail_extc + 1];
	res = vkEnumerateDeviceExtensionProperties(phdev, NULL, &avail_extc, avail);
	if (res != VK_SUCCESS) {
		nx_vk_error("Could not enumerate device extensions (2)", res);
		return NULL;
	}

	for (uint32_t i = 0; i < avail_extc; ++i) {
		nx_log(NX_DEBUG, "Vulkan device extension %s v%"PRIu32,
			avail[i].extensionName, avail[i].specVersion);
	}

	struct nx_vk_device *dev = calloc(1, sizeof(*dev));
	if (!dev) {
		nx_log_errno(NX_ERROR, "allocation failed");
		return NULL;
	}
	dev->phdev = phdev;
	dev->instance = ini;
	dev->drm_fd = -1;

	struct extension_list extensions = {0};
	for (size_t i = 0; i < sizeof(required_extensions) / sizeof(required_extensions[0]); i++) {
		if (!vulkan_has_extension(avail, avail_extc, required_extensions[i])) {
			nx_log(NX_ERROR, "vulkan: required device extension %s not found",
				required_extensions[i]);
			goto error;
		}
		add_extension(&extensions, required_extensions[i]);
	}

	vulkan_device_probe_caps(dev, avail, avail_extc);
	bool has_semaphore_fd = vulkan_has_extension(avail, avail_extc,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
	if (has_semaphore_fd) {
		add_extension(&extensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
	}

	if (!init_logical_device(dev, &extensions, find_global_priority(avail, avail_extc))) {
		goto error;
	}

	vkGetDeviceQueue(dev->dev, dev->queue_family, 0, &dev->queue);
	load_device_procs(dev);
	if (has_semaphore_fd) {
		load_semaphore_fd_procs(dev);
	}

	if (!query_format_props(dev)) {
		goto error;
	}
	return dev;

error:
	vulkan_device_destroy(dev);
	return NULL;
}

void vulkan_device_destroy(struct nx_vk_device *dev) {
	if (dev->dev) {
		vkDestroyDevice(dev->dev, NULL);
	}
	if (dev->drm_fd > 0) {
		close(dev->drm_fd);
	}

	for (size_t i = 0; i < NX_VK_MODIFIER_USAGE_COUNT; i++) {
		nx_drm_format_set_finish(&dev->dmabuf_formats[i]);
	}
	nx_drm_format_set_finish(&dev->shm_texture_formats);

	for (uint32_t i = 0; i < dev->format_prop_count; ++i) {
		vulkan_format_props_finish(&dev->format_props[i]);
	}
	free(dev->format_props);
	free(dev);
}
