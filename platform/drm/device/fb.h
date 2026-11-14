#ifndef PLATFORM_DRM_DEVICE_FB_H
#define PLATFORM_DRM_DEVICE_FB_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-util.h>
#include "platform/container/addon.h"

struct nx_buffer;
struct nx_drm_backend;
struct nx_drm_format_set;

/* Framebuffer cached on its buffer. */
struct nx_drm_fb {
	struct nx_buffer *nx_buf;
	struct nx_addon addon;
	struct nx_drm_backend *backend;
	struct wl_list link; // nx_drm_backend.fbs

	uint32_t id;
};

bool drm_fb_import(struct nx_drm_fb **fb, struct nx_drm_backend *drm,
	struct nx_buffer *buf, const struct nx_drm_format_set *formats);
void drm_fb_destroy(struct nx_drm_fb *fb);

void drm_fb_clear(struct nx_drm_fb **fb);
void drm_fb_copy(struct nx_drm_fb **new, struct nx_drm_fb *old);
void drm_fb_move(struct nx_drm_fb **new, struct nx_drm_fb **old);
struct nx_drm_fb *drm_fb_lock(struct nx_drm_fb *fb);

#endif
