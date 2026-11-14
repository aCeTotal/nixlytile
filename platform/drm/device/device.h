#ifndef PLATFORM_DRM_DEVICE_DEVICE_H
#define PLATFORM_DRM_DEVICE_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include "platform/drm/device/properties.h"
#include "platform/drm/device/renderer.h"
#include "platform/drm/drm.h"
#include "platform/geometry/box.h"
#include "platform/render/drm_format_set.h"

struct nx_color_transform;
struct nx_drm_interface;

struct nx_drm_viewport {
	struct nx_fbox src_box;
	struct nx_box dst_box;
};

struct nx_drm_plane {
	uint32_t id;

	/* Multi-GPU only */
	struct nx_drm_surface mgpu_surf;

	/* Shown at the next vblank */
	struct nx_drm_fb *queued_fb;
	struct nx_drm_syncobj_timeline *queued_release_timeline;
	uint64_t queued_release_point;
	/* On screen now */
	struct nx_drm_fb *current_fb;
	struct nx_drm_syncobj_timeline *current_release_timeline;
	uint64_t current_release_point;
	/* Of the last committed fb */
	struct nx_drm_viewport viewport;

	struct nx_drm_format_set formats;

	struct nx_output_cursor_size *cursor_sizes;
	size_t cursor_sizes_len;

	struct nx_drm_plane_props props;
};

struct nx_drm_crtc {
	uint32_t id;

	/* Atomic only */
	bool own_mode_id;
	uint32_t mode_id;
	uint32_t gamma_lut;
	bool cursor_plane_on;

	/* Immutable, so read once */
	size_t gamma_lut_size;

	struct nx_drm_plane *primary;
	struct nx_drm_plane *cursor;

	struct nx_drm_crtc_props props;
};

struct nx_drm_backend {
	struct nx_backend backend;

	struct nx_drm_backend *parent;
	const struct nx_drm_interface *iface;
	bool addfb2_modifiers;
	bool cap_addfb2_modifiers;
	/* Modifier AddFB2 failed, plain worked */
	bool addfb2_modifiers_broken;

	int fd;
	char *name;
	struct nx_device *dev;

	size_t num_crtcs;
	struct nx_drm_crtc *crtcs;

	size_t num_planes;
	struct nx_drm_plane *planes;

	struct wl_event_source *drm_event;

	struct wl_listener session_destroy;
	struct wl_listener session_active;
	struct wl_listener parent_destroy;
	struct wl_listener dev_change;
	struct wl_listener dev_remove;

	struct wl_list fbs; // nx_drm_fb.link
	struct wl_list connectors; // nx_drm_connector.link
	struct wl_list page_flips; // nx_drm_page_flip.link

	/* Multi-GPU only */
	struct nx_drm_renderer mgpu_renderer;
	struct nx_drm_format_set mgpu_formats;

	struct nx_session *session;

	uint64_t cursor_width, cursor_height;

	bool supports_tearing_page_flips;
	/* Fenceless retry worked after EPERM */
	bool in_fence_fd_broken;
};

struct nx_drm_backend *drm_backend_from_backend(struct nx_backend *backend);
bool drm_check_features(struct nx_drm_backend *drm);

bool drm_init_resources(struct nx_drm_backend *drm);
void drm_finish_resources(struct nx_drm_backend *drm);
/* NULL loads a linear ramp. */
bool drm_legacy_crtc_set_gamma(struct nx_drm_backend *drm,
	struct nx_drm_crtc *crtc, struct nx_color_transform *transform);

bool drm_init_planes(struct nx_drm_backend *drm);
void drm_plane_finish_surface(struct nx_drm_plane *plane);
void drm_plane_release(struct nx_drm_syncobj_timeline **timeline,
	uint64_t *point);

#endif
