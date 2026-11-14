#ifndef PLATFORM_DRM_DEVICE_RENDERER_H
#define PLATFORM_DRM_DEVICE_RENDERER_H

#include <stdbool.h>
#include <stdint.h>

struct nx_allocator;
struct nx_buffer;
struct nx_drm_backend;
struct nx_drm_plane;
struct nx_drm_syncobj_timeline;
struct nx_renderer;
struct nx_swapchain;

/* Blits frames to secondary GPUs. */
struct nx_drm_renderer {
	struct nx_renderer *nx_rend;
	struct nx_allocator *allocator;
};

struct nx_drm_surface {
	struct nx_drm_renderer *renderer;
	struct nx_swapchain *swapchain;

	struct nx_drm_syncobj_timeline *timeline;
	uint64_t point;
};

bool drm_renderer_init(struct nx_drm_backend *drm,
	struct nx_drm_renderer *renderer);
void drm_renderer_finish(struct nx_drm_renderer *renderer);

void drm_surface_finish(struct nx_drm_surface *surf);

struct nx_buffer *drm_surface_blit(struct nx_drm_surface *surf,
	struct nx_buffer *buffer,
	struct nx_drm_syncobj_timeline *wait_timeline, uint64_t wait_point);

/* Prepares the plane's blit target. */
bool drm_plane_init_surface(struct nx_drm_plane *plane,
	struct nx_drm_renderer *renderer, const struct nx_buffer *buffer);

/* Falls back to primary-GPU scanout. */
bool drm_mgpu_init(struct nx_drm_backend *drm);

#endif
