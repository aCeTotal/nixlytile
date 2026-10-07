#include "layer.h"

#include <stdlib.h>
#include <string.h>

#define FSR_EXTENSION VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME
#define RATES_MAX     16

/* Requested size per ring. */
static const VkExtent2D ring_size[PATTERN_RINGS] = {
	{ 1, 1 }, { 2, 1 }, { 2, 2 }, { 4, 2 }, { 4, 4 },
};

static int
device_has_extension(const struct device_setup *s)
{
	VkExtensionProperties *props;
	uint32_t i, n = 0;
	int found = 0;

	s->inst->EnumerateDeviceExtensionProperties(s->phys, NULL, &n, NULL);
	props = calloc(n, sizeof(*props));
	if (!props)
		return 0;
	s->inst->EnumerateDeviceExtensionProperties(s->phys, NULL, &n, props);
	for (i = 0; i < n && !found; i++)
		found = strcmp(props[i].extensionName, FSR_EXTENSION) == 0;
	free(props);
	return found;
}

static int
device_supports_attachment(const struct device_setup *s)
{
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
	};
	VkPhysicalDeviceFeatures2 features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &fsr,
	};
	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
	};

	s->inst->GetPhysicalDeviceProperties2(s->phys, &props);
	if (props.properties.apiVersion < VK_API_VERSION_1_2)
		return 0;
	s->inst->GetPhysicalDeviceFeatures2(s->phys, &features);
	return fsr.attachmentFragmentShadingRate && fsr.pipelineFragmentShadingRate;
}

/* Largest supported size within request. */
static uint8_t
ring_code(const VkPhysicalDeviceFragmentShadingRateKHR *rates, uint32_t n,
	VkExtent2D want)
{
	VkExtent2D best = { 1, 1 };
	uint32_t i;

	for (i = 0; i < n; i++) {
		VkExtent2D r = rates[i].fragmentSize;
		if (!(rates[i].sampleCounts & VK_SAMPLE_COUNT_1_BIT)
				|| r.width > want.width || r.height > want.height
				|| r.width * r.height <= best.width * best.height)
			continue;
		best = r;
	}
	return (uint8_t)((__builtin_ctz(best.width) << 2) | __builtin_ctz(best.height));
}

static void
read_rates(struct device_setup *s)
{
	VkPhysicalDeviceFragmentShadingRateKHR rates[RATES_MAX];
	VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsr = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR,
	};
	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &fsr,
	};
	uint32_t i, n = RATES_MAX;

	for (i = 0; i < RATES_MAX; i++)
		rates[i] = (VkPhysicalDeviceFragmentShadingRateKHR){
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR,
		};
	s->inst->GetPhysicalDeviceProperties2(s->phys, &props);
	s->texel = fsr.minFragmentShadingRateAttachmentTexelSize;
	if (s->inst->GetPhysicalDeviceFragmentShadingRatesKHR(s->phys, &n, rates) < 0)
		n = 0;
	for (i = 0; i < PATTERN_RINGS; i++)
		s->codes[i] = ring_code(rates, n, ring_size[i]);
}

/* Features clashing with injected rates. */
static int
conflicts(const VkBaseInStructure *p)
{
	switch (p->sType) {
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV:
		return ((const VkPhysicalDeviceShadingRateImageFeaturesNV *)p)
			->shadingRateImage;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT:
		return ((const VkPhysicalDeviceFragmentDensityMapFeaturesEXT *)p)
			->fragmentDensityMap;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
		return ((const VkPhysicalDeviceShaderObjectFeaturesEXT *)p)
			->shaderObject;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
		return 1;
	default:
		return 0;
	}
}

static int
app_conflicts(const struct device_setup *s)
{
	const VkBaseInStructure *p;

	for (p = s->app->pNext; p; p = p->pNext)
		if (conflicts(p))
			return 1;
	return 0;
}

static int
add_extension(struct device_setup *s)
{
	uint32_t i, n = s->app->enabledExtensionCount;

	for (i = 0; i < n; i++)
		if (strcmp(s->app->ppEnabledExtensionNames[i], FSR_EXTENSION) == 0)
			return 1;
	s->extensions = calloc(n + 1, sizeof(*s->extensions));
	if (!s->extensions)
		return 0;
	if (n)
		memcpy(s->extensions, s->app->ppEnabledExtensionNames,
			n * sizeof(*s->extensions));
	s->extensions[n] = FSR_EXTENSION;
	s->info.enabledExtensionCount = n + 1;
	s->info.ppEnabledExtensionNames = s->extensions;
	return 1;
}

/* 1: s->info enables attachment VRS. */
int
features_prepare(struct device_setup *s)
{
	if (s->inst->api_version < VK_API_VERSION_1_2
			|| !s->inst->GetPhysicalDeviceFeatures2
			|| !s->inst->GetPhysicalDeviceProperties2
			|| !s->inst->GetPhysicalDeviceFragmentShadingRatesKHR)
		return 0;
	if (app_conflicts(s) || !device_has_extension(s)
			|| !device_supports_attachment(s))
		return 0;
	read_rates(s);
	s->info = *s->app;
	s->features = (VkPhysicalDeviceFragmentShadingRateFeaturesKHR){
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
		.pNext = (void *)s->app->pNext,
		.pipelineFragmentShadingRate = VK_TRUE,
		.attachmentFragmentShadingRate = VK_TRUE,
	};
	s->info.pNext = &s->features;
	return add_extension(s);
}

void
features_release(struct device_setup *s)
{
	free(s->extensions);
	s->extensions = NULL;
}
