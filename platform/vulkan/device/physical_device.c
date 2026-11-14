#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <xf86drm.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/device/device.h"

static const char *device_type_name(VkPhysicalDeviceType type) {
	switch (type) {
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
		return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
		return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_CPU:
		return "cpu";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
		return "vgpu";
	default:
		return "unknown";
	}
}

static void log_phdev(const VkPhysicalDeviceProperties *props) {
	nx_log(NX_INFO, "Vulkan device: '%s'", props->deviceName);
	nx_log(NX_INFO, "  Device type: '%s'", device_type_name(props->deviceType));
	nx_log(NX_INFO, "  Supported API version: %u.%u.%u",
		VK_VERSION_MAJOR(props->apiVersion), VK_VERSION_MINOR(props->apiVersion),
		VK_VERSION_PATCH(props->apiVersion));
	nx_log(NX_INFO, "  Driver version: %u.%u.%u",
		VK_VERSION_MAJOR(props->driverVersion), VK_VERSION_MINOR(props->driverVersion),
		VK_VERSION_PATCH(props->driverVersion));
}

static bool drm_node_matches(const VkPhysicalDeviceDrmPropertiesEXT *props, dev_t rdev) {
	return makedev(props->primaryMajor, props->primaryMinor) == rdev ||
		makedev(props->renderMajor, props->renderMinor) == rdev;
}

// Without DRM, match CPU devices.
static bool phdev_matches(VkPhysicalDevice phdev, int drm_fd, const struct stat *drm_stat) {
	VkPhysicalDeviceProperties phdev_props;
	vkGetPhysicalDeviceProperties(phdev, &phdev_props);
	log_phdev(&phdev_props);

	// vkGetPhysicalDeviceProperties2 needs 1.1.
	if (phdev_props.apiVersion < VK_API_VERSION_1_1) {
		return false;
	}

	uint32_t avail_extc = 0;
	VkResult res = vkEnumerateDeviceExtensionProperties(phdev, NULL, &avail_extc, NULL);
	if (res != VK_SUCCESS || avail_extc == 0) {
		nx_vk_error("  Could not enumerate device extensions", res);
		return false;
	}

	VkExtensionProperties avail_ext_props[avail_extc + 1];
	res = vkEnumerateDeviceExtensionProperties(phdev, NULL, &avail_extc, avail_ext_props);
	if (res != VK_SUCCESS) {
		nx_vk_error("  Could not enumerate device extensions", res);
		return false;
	}

	bool has_drm_props = vulkan_has_extension(avail_ext_props, avail_extc,
		VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
	bool has_driver_props = vulkan_has_extension(avail_ext_props, avail_extc,
		VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);

	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
	};
	VkPhysicalDeviceDrmPropertiesEXT drm_props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
	};
	if (has_drm_props) {
		drm_props.pNext = props.pNext;
		props.pNext = &drm_props;
	}
	VkPhysicalDeviceDriverPropertiesKHR driver_props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
	};
	if (has_driver_props) {
		driver_props.pNext = props.pNext;
		props.pNext = &driver_props;
	}
	vkGetPhysicalDeviceProperties2(phdev, &props);

	if (has_driver_props) {
		nx_log(NX_INFO, "  Driver name: %s (%s)",
			driver_props.driverName, driver_props.driverInfo);
	}

	if (drm_fd >= 0 && !has_drm_props) {
		nx_log(NX_DEBUG, "  Ignoring physical device \"%s\": "
			"VK_EXT_physical_device_drm not supported", phdev_props.deviceName);
		return false;
	}

	bool found = drm_fd >= 0 ? drm_node_matches(&drm_props, drm_stat->st_rdev) :
		phdev_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
	if (found) {
		nx_log(NX_INFO, "Found matching Vulkan physical device: %s",
			phdev_props.deviceName);
	}
	return found;
}

VkPhysicalDevice vulkan_find_drm_phdev(struct nx_vk_instance *ini, int drm_fd) {
	uint32_t num_phdevs;
	VkResult res = vkEnumeratePhysicalDevices(ini->instance, &num_phdevs, NULL);
	if (res != VK_SUCCESS) {
		nx_vk_error("Could not retrieve physical devices", res);
		return VK_NULL_HANDLE;
	}

	VkPhysicalDevice phdevs[1 + num_phdevs];
	res = vkEnumeratePhysicalDevices(ini->instance, &num_phdevs, phdevs);
	if (res != VK_SUCCESS) {
		nx_vk_error("Could not retrieve physical devices", res);
		return VK_NULL_HANDLE;
	}

	struct stat drm_stat = {0};
	if (drm_fd >= 0 && fstat(drm_fd, &drm_stat) != 0) {
		nx_log_errno(NX_ERROR, "fstat failed");
		return VK_NULL_HANDLE;
	}

	for (uint32_t i = 0; i < num_phdevs; ++i) {
		if (phdev_matches(phdevs[i], drm_fd, &drm_stat)) {
			return phdevs[i];
		}
	}
	return VK_NULL_HANDLE;
}

// Prefers the render node.
int vulkan_open_phdev_drm_fd(VkPhysicalDevice phdev) {
	VkPhysicalDeviceDrmPropertiesEXT drm_props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,
	};
	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &drm_props,
	};
	vkGetPhysicalDeviceProperties2(phdev, &props);

	if (!drm_props.hasRender && !drm_props.hasPrimary) {
		nx_log(NX_INFO, "Physical device is missing both render and primary nodes");
		return -1;
	}
	dev_t devid = drm_props.hasRender ?
		makedev(drm_props.renderMajor, drm_props.renderMinor) :
		makedev(drm_props.primaryMajor, drm_props.primaryMinor);

	drmDevice *device = NULL;
	if (drmGetDeviceFromDevId(devid, 0, &device) != 0) {
		nx_log_errno(NX_ERROR, "drmGetDeviceFromDevId failed");
		return -1;
	}

	const char *name = device->nodes[DRM_NODE_RENDER];
	if (!(device->available_nodes & (1 << DRM_NODE_RENDER))) {
		assert(device->available_nodes & (1 << DRM_NODE_PRIMARY));
		name = device->nodes[DRM_NODE_PRIMARY];
		nx_log(NX_DEBUG, "DRM device %s has no render node, "
			"falling back to primary node", name);
	}

	int drm_fd = open(name, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (drm_fd < 0) {
		nx_log_errno(NX_ERROR, "Failed to open DRM node %s", name);
	}
	drmFreeDevice(&device);
	return drm_fd;
}
