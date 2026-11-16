#include "layer.h"

#include <stdlib.h>
#include <string.h>

#define FSR_EXTENSION VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME
#define RATES_MAX     16
#define RATE_FEATURES (VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR \
	| VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
#define MAX(a, b)     ((a) > (b) ? (a) : (b))

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

/* Both rate features set. */
static int
has_rates(const VkPhysicalDeviceFragmentShadingRateFeaturesKHR *f)
{
	return f->pipelineFragmentShadingRate && f->attachmentFragmentShadingRate;
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
	return has_rates(&fsr);
}

static int
power_of_two(uint32_t v)
{
	return v && !(v & (v - 1));
}

/* In range, within aspect limit. */
static int
texel_fits(VkExtent2D t, const VkPhysicalDeviceFragmentShadingRatePropertiesKHR *p)
{
	VkExtent2D max = p->maxFragmentShadingRateAttachmentTexelSize;
	uint64_t aspect = p->maxFragmentShadingRateAttachmentTexelSizeAspectRatio;

	return power_of_two(t.width) && power_of_two(t.height)
		&& t.width <= max.width && t.height <= max.height
		&& t.width <= t.height * aspect && t.height <= t.width * aspect;
}

/* Smallest texel, short side widened. */
static VkExtent2D
pick_texel(const VkPhysicalDeviceFragmentShadingRatePropertiesKHR *p)
{
	VkExtent2D t = p->minFragmentShadingRateAttachmentTexelSize;
	uint32_t aspect = p->maxFragmentShadingRateAttachmentTexelSizeAspectRatio;

	if (!aspect)
		return t;
	t.width = MAX(t.width, t.height / aspect);
	t.height = MAX(t.height, t.width / aspect);
	return t;
}

static struct rate_limits
limits_of(const VkPhysicalDeviceFragmentShadingRatePropertiesKHR *p)
{
	return (struct rate_limits){
		.native = (p->fragmentShadingRateWithShaderSampleMask ? 0 : NATIVE_SHADER_MASK)
			| (p->fragmentShadingRateWithSampleMask ? 0 : NATIVE_SAMPLE_MASK)
			| (p->fragmentShadingRateWithConservativeRasterization ? 0 : NATIVE_CONSERVATIVE)
			| (p->fragmentShadingRateWithCustomSampleLocations ? 0 : NATIVE_LOCATIONS)
			| (p->fragmentShadingRateNonTrivialCombinerOps ? 0 : NATIVE_DISCARD),
		.samples = p->maxFragmentShadingRateRasterizationSamples,
	};
}

/* Texel and limits; 0: unusable. */
static int
read_properties(struct device_setup *s)
{
	VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsr = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR,
	};
	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &fsr,
	};

	s->inst->GetPhysicalDeviceProperties2(s->phys, &props);
	s->texel = pick_texel(&fsr);
	s->limits = limits_of(&fsr);
	return texel_fits(s->texel, &fsr);
}

/* Single-sample sizes fit the map. */
static void
read_rates(struct device_setup *s)
{
	VkPhysicalDeviceFragmentShadingRateKHR rates[RATES_MAX];
	VkExtent2D sizes[RATES_MAX];
	uint32_t i, n = RATES_MAX, count = 0;

	for (i = 0; i < RATES_MAX; i++)
		rates[i] = (VkPhysicalDeviceFragmentShadingRateKHR){
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR,
		};
	if (s->inst->GetPhysicalDeviceFragmentShadingRatesKHR(s->phys, &n, rates) < 0)
		n = 0;
	for (i = 0; i < n; i++) {
		sizes[count] = rates[i].fragmentSize;
		count += (rates[i].sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0;
	}
	s->fit = map_fit(sizes, count);
}

static int
rate_format(const struct device_setup *s)
{
	VkFormatProperties props;

	s->inst->GetPhysicalDeviceFormatProperties(s->phys, RATE_FORMAT, &props);
	return (props.optimalTilingFeatures & RATE_FEATURES) == RATE_FEATURES;
}

/* Requests clashing with injected rates. */
static int
conflicts(const VkBaseInStructure *p)
{
	switch (p->sType) {
	case VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO:
		return ((const VkDeviceGroupDeviceCreateInfo *)p)->physicalDeviceCount > 1;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV:
		return ((const VkPhysicalDeviceShadingRateImageFeaturesNV *)p)
			->shadingRateImage;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT:
		return ((const VkPhysicalDeviceFragmentDensityMapFeaturesEXT *)p)
			->fragmentDensityMap;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
		return !has_rates((const VkPhysicalDeviceFragmentShadingRateFeaturesKHR *)p);
	default:
		return 0;
	}
}

static int
app_conflicts(const struct device_setup *s)
{
	const VkBaseInStructure *p = s->app->pNext;

	while (p && !conflicts(p))
		p = p->pNext;
	return p != NULL;
}

/* AMD: feature taxes depth images. */
static int
compositor_reachable(void)
{
	link_connect();
	return link_page() != NULL;
}

static int
add_extension(struct device_setup *s)
{
	uint32_t i = 0, n = s->app->enabledExtensionCount;

	while (i < n && strcmp(s->app->ppEnabledExtensionNames[i], FSR_EXTENSION) != 0)
		i++;
	if (i < n)
		return 1;
	s->extensions = calloc(n + 1, sizeof(*s->extensions));
	if (!s->extensions)
		return 0;
	if (n)
		memcpy(s->extensions, s->app->ppEnabledExtensionNames,
			n * sizeof(*s->extensions));
	s->extensions[n] = FSR_EXTENSION;
	s->patched.enabledExtensionCount = n + 1;
	s->patched.ppEnabledExtensionNames = s->extensions;
	return 1;
}

/* 1: s->patched enables VRS. */
int
features_prepare(struct device_setup *s)
{
	if (s->inst->api_version < VK_API_VERSION_1_2
			|| !s->inst->GetPhysicalDeviceFeatures2
			|| !s->inst->GetPhysicalDeviceProperties2
			|| !s->inst->GetPhysicalDeviceFragmentShadingRatesKHR)
		return 0;
	if (app_conflicts(s) || !device_has_extension(s)
			|| !device_supports_attachment(s) || !rate_format(s)
			|| !read_properties(s) || !compositor_reachable())
		return 0;
	read_rates(s);
	s->patched = *s->app;
	if (chain_find(s->app->pNext,
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR))
		return add_extension(s);
	s->features = (VkPhysicalDeviceFragmentShadingRateFeaturesKHR){
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,
		.pNext = (void *)s->app->pNext,
		.pipelineFragmentShadingRate = VK_TRUE,
		.attachmentFragmentShadingRate = VK_TRUE,
	};
	s->patched.pNext = &s->features;
	return add_extension(s);
}

void
features_release(struct device_setup *s)
{
	free(s->extensions);
	s->extensions = NULL;
}
