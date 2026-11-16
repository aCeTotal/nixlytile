#ifndef NIXLY_VRS_FOVEA_H
#define NIXLY_VRS_FOVEA_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "gpu.h"
#include "map.h"

#define RATE_SIZES   16
#define UPLOAD_SLOTS 3              /* frames in flight */
#define RATE_USAGE   (VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR \
	| VK_IMAGE_USAGE_TRANSFER_DST_BIT)

/* Claimed slots start wanted. */
enum rate_state { RATE_WANTED, RATE_FRESH, RATE_READY, RATE_FAILED };

/* Present queue's copy objects. */
enum upload_state { UPLOAD_NONE, UPLOAD_WANTED, UPLOAD_READY, UPLOAD_FAILED };

/* Rate attachment per scene size. */
struct rate_image {
	_Atomic uint64_t key;       /* packed size; 0: free */
	_Atomic(enum rate_state) state;
	_Atomic int native;         /* map all 1x1 */
	VkExtent2D size;
	VkExtent2D grid;
	struct gpu_image attachment;
	struct gpu_buffer staging;  /* one region per slot */
	uint8_t *mapped;
	struct zones zones;         /* map last uploaded */
};

/* Copy, reusable once fenced. */
struct upload_slot {
	VkCommandBuffer cmd;
	VkFence fence;
};

/* Builds rate objects off-thread. */
struct worker {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	int kicked;                 /* under lock */
	_Atomic int quit;
	int running;
};

/* Zone maps per scene size. */
struct fovea {
	_Atomic(enum upload_state) upload;
	_Atomic int dirty;          /* maps await upload */
	_Atomic int asked;          /* sizes await worker */
	uint32_t family;            /* present queue family */
	struct zones zones;         /* latest settings */
	VkCommandPool pool;
	struct upload_slot slots[UPLOAD_SLOTS];
	uint32_t next;              /* slot to reuse */
	struct rate_image rates[RATE_SIZES];
	struct worker worker;
};

/* Exclusive unless the caller shares. */
static inline VkImageCreateInfo
rate_image_create(VkExtent2D grid)
{
	return (VkImageCreateInfo){
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = RATE_FORMAT,
		.extent = { grid.width, grid.height, 1 },
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = RATE_USAGE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
}

/* Discards contents for transfer writes. */
static inline VkImageMemoryBarrier
rate_writable(VkImage image)
{
	return (VkImageMemoryBarrier){
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = WHOLE_COLOR,
	};
}

struct device;

VkImageView rate_view(struct device *dev, VkExtent2D size, VkImageView pending);
void rate_build(struct device *dev);
void rate_destroy(struct device *dev);

void upload_request(struct device *dev, uint32_t family);
void upload_create(struct device *dev);
void upload_destroy(struct device *dev);
int upload_stale(struct device *dev);

void worker_start(struct device *dev);
void worker_kick(struct worker *w);
void worker_stop(struct device *dev);

int fovea_active(const struct device *dev);
void fovea_present(struct device *dev);

#endif
