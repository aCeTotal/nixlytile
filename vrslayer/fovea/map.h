#ifndef NIXLY_VRS_MAP_H
#define NIXLY_VRS_MAP_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#define RATE_AXIS_STEPS 3           /* 1, 2, 4 per axis */
#define LADDER_STEPS    5           /* 1x1 2x1 2x2 4x2 4x4 */
#define CODE_WIDTH_SHIFT 2          /* code: log2 w, h */
#define RATE_FORMAT     VK_FORMAT_R8_UINT

/* Zone settings for one display. */
struct zones {
	VkExtent2D display;
	uint32_t focus;             /* link packing; NONE: centre */
	uint32_t fovea;             /* radius, per mille height */
	uint32_t blend;             /* width, per mille height */
	uint32_t quality;           /* periphery, 0..VRS_QUALITY_MAX */
};

/* Supported code per wanted size. */
struct rate_fit {
	uint8_t code[RATE_AXIS_STEPS][RATE_AXIS_STEPS];   /* log2 w, log2 h */
};

/* Everything one map depends on. */
struct map {
	struct zones zones;
	struct rate_fit fit;
	VkExtent2D texel;
};

/* Rate texels covering size. */
static inline VkExtent2D
texel_grid(VkExtent2D texel, VkExtent2D size)
{
	return (VkExtent2D){
		(size.width + texel.width - 1) / texel.width,
		(size.height + texel.height - 1) / texel.height,
	};
}

/* One byte per texel. */
static inline VkDeviceSize
grid_bytes(VkExtent2D grid)
{
	return (VkDeviceSize)grid.width * grid.height;
}

struct rate_fit map_fit(const VkExtent2D *sizes, uint32_t count);
int zones_equal(const struct zones *a, const struct zones *b);
int map_native(const struct map *m, VkExtent2D size);
void map_fill(const struct map *m, VkExtent2D size, uint8_t *codes);

#endif
