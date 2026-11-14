#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "platform/log/log.h"
#include "platform/vulkan/device/device.h"

#define NIXLYTILE_ENGINE_VERSION VK_MAKE_API_VERSION(0, 0, 8, 0)

// Intentionally unused shader outputs.
static const char *const ignored_messages[] = {
	"UNASSIGNED-CoreValidation-Shader-OutputNotConsumed",
};

bool vulkan_has_extension(const VkExtensionProperties *avail, uint32_t avail_len,
		const char *name) {
	for (uint32_t i = 0; i < avail_len; i++) {
		if (strcmp(avail[i].extensionName, name) == 0) {
			return true;
		}
	}
	return false;
}

static bool is_ignored_message(const char *id) {
	if (!id) {
		return false;
	}
	for (size_t i = 0; i < sizeof(ignored_messages) / sizeof(ignored_messages[0]); ++i) {
		if (strcmp(id, ignored_messages[i]) == 0) {
			return true;
		}
	}
	return false;
}

static VKAPI_ATTR VkBool32 debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT type,
		const VkDebugUtilsMessengerCallbackDataEXT *debug_data, void *data) {
	if (is_ignored_message(debug_data->pMessageIdName)) {
		return false;
	}

	enum nx_log_importance importance = NX_INFO;
	if (severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ||
			severity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		importance = NX_ERROR;
	}

	nx_log(importance, "%s (%s)", debug_data->pMessage, debug_data->pMessageIdName);
	if (debug_data->queueLabelCount > 0 && debug_data->pQueueLabels[0].pLabelName) {
		nx_log(importance, "    last label '%s'", debug_data->pQueueLabels[0].pLabelName);
	}

	for (uint32_t i = 0; i < debug_data->objectCount; ++i) {
		if (debug_data->pObjects[i].pObjectName) {
			nx_log(importance, "    involving '%s'", debug_data->pMessage);
		}
	}

	return false;
}

static bool has_vulkan_1_1(void) {
	PFN_vkEnumerateInstanceVersion enumerate_version = (PFN_vkEnumerateInstanceVersion)
		vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
	uint32_t version;
	return enumerate_version && enumerate_version(&version) == VK_SUCCESS &&
		version >= VK_API_VERSION_1_1;
}

static void create_messenger(struct nx_vk_instance *ini,
		const VkDebugUtilsMessengerCreateInfoEXT *info) {
	ini->api.createDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(ini->instance, "vkCreateDebugUtilsMessengerEXT");
	ini->api.destroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(ini->instance, "vkDestroyDebugUtilsMessengerEXT");

	if (!ini->api.createDebugUtilsMessengerEXT) {
		nx_log(NX_ERROR, "vkCreateDebugUtilsMessengerEXT not found");
		return;
	}
	ini->api.createDebugUtilsMessengerEXT(ini->instance, info, NULL, &ini->messenger);
}

struct nx_vk_instance *vulkan_instance_create(void) {
	if (!has_vulkan_1_1()) {
		nx_log(NX_ERROR, "nixlytile requires vulkan 1.1 which is not available");
		return NULL;
	}

	uint32_t avail_extc = 0;
	VkResult res = vkEnumerateInstanceExtensionProperties(NULL, &avail_extc, NULL);
	if (res != VK_SUCCESS || avail_extc == 0) {
		nx_vk_error("Could not enumerate instance extensions (1)", res);
		return NULL;
	}

	VkExtensionProperties avail_ext_props[avail_extc + 1];
	res = vkEnumerateInstanceExtensionProperties(NULL, &avail_extc, avail_ext_props);
	if (res != VK_SUCCESS) {
		nx_vk_error("Could not enumerate instance extensions (2)", res);
		return NULL;
	}

	for (uint32_t i = 0; i < avail_extc; ++i) {
		nx_log(NX_DEBUG, "Vulkan instance extension %s v%"PRIu32,
			avail_ext_props[i].extensionName, avail_ext_props[i].specVersion);
	}

	struct nx_vk_instance *ini = calloc(1, sizeof(*ini));
	if (!ini) {
		nx_log_errno(NX_ERROR, "allocation failed");
		return NULL;
	}

	bool debug_utils = vulkan_has_extension(avail_ext_props, avail_extc,
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	const char *extensions[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };

	VkApplicationInfo application_info = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pEngineName = "nixlytile",
		.engineVersion = NIXLYTILE_ENGINE_VERSION,
		.apiVersion = VK_API_VERSION_1_1,
	};

	VkDebugUtilsMessengerCreateInfoEXT debug_info = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
		.pfnUserCallback = &debug_callback,
		.pUserData = ini,
	};

	// Also reports instance creation.
	VkInstanceCreateInfo instance_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pNext = debug_utils ? &debug_info : NULL,
		.pApplicationInfo = &application_info,
		.enabledExtensionCount = debug_utils ? 1 : 0,
		.ppEnabledExtensionNames = extensions,
	};

	res = vkCreateInstance(&instance_info, NULL, &ini->instance);
	if (res != VK_SUCCESS) {
		nx_vk_error("Could not create instance", res);
		vulkan_instance_destroy(ini);
		return NULL;
	}

	if (debug_utils) {
		create_messenger(ini, &debug_info);
	}
	return ini;
}

void vulkan_instance_destroy(struct nx_vk_instance *ini) {
	if (ini->messenger && ini->api.destroyDebugUtilsMessengerEXT) {
		ini->api.destroyDebugUtilsMessengerEXT(ini->instance, ini->messenger, NULL);
	}
	if (ini->instance) {
		vkDestroyInstance(ini->instance, NULL);
	}
	free(ini);
}
