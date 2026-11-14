#ifndef PLATFORM_OUTPUT_COMMIT_STATE_H
#define PLATFORM_OUTPUT_COMMIT_STATE_H

#include <pixman.h>
#include <stdbool.h>
#include <wayland-server-protocol.h>
#include "platform/render/color.h"
#include "platform/buffer/buffer.h"
#include "platform/geometry/box.h"

struct nx_output_mode;

enum nx_output_state_field {
	NX_OUTPUT_STATE_BUFFER = 1 << 0,
	NX_OUTPUT_STATE_DAMAGE = 1 << 1,
	NX_OUTPUT_STATE_MODE = 1 << 2,
	NX_OUTPUT_STATE_ENABLED = 1 << 3,
	NX_OUTPUT_STATE_SCALE = 1 << 4,
	NX_OUTPUT_STATE_TRANSFORM = 1 << 5,
	NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED = 1 << 6,
	NX_OUTPUT_STATE_RENDER_FORMAT = 1 << 7,
	NX_OUTPUT_STATE_SUBPIXEL = 1 << 8,
	NX_OUTPUT_STATE_WAIT_TIMELINE = 1 << 10,
	NX_OUTPUT_STATE_SIGNAL_TIMELINE = 1 << 11,
	NX_OUTPUT_STATE_COLOR_TRANSFORM = 1 << 12,
	NX_OUTPUT_STATE_IMAGE_DESCRIPTION = 1 << 13,
	NX_OUTPUT_STATE_CONTENT_TYPE = 1 << 14,
};

enum nx_output_state_mode_type {
	NX_OUTPUT_STATE_MODE_FIXED,
	NX_OUTPUT_STATE_MODE_CUSTOM,
};

// Colorimetry of the primary buffer.
struct nx_output_image_description {
	enum nx_color_named_primaries primaries;
	enum nx_color_transfer_function transfer_function;

	// Optional mastering metadata in cd/m².
	struct nx_color_primaries mastering_display_primaries;
	struct {
		double min, max;
	} mastering_luminance;
	double max_cll; // max content light level
	double max_fall; // max frame-average light level
};

// Double-buffered output state.
struct nx_output_state {
	uint32_t committed; // enum nx_output_state_field
	// May briefly disrupt the output.
	bool allow_reconfiguration;
	pixman_region32_t damage; // output-buffer-local coordinates
	bool enabled;
	float scale;
	enum wl_output_transform transform;
	bool adaptive_sync_enabled;
	uint32_t render_format;

	struct nx_buffer *buffer;
	// Buffer-space crop, zero for none.
	struct nx_fbox buffer_src_box;
	// Zero size means native size.
	struct nx_box buffer_dst_box;

	// Backends may reject tearing.
	bool tearing_page_flip;

	enum nx_output_state_mode_type mode_type;
	struct nx_output_mode *mode;
	struct {
		int32_t width, height;
		int32_t refresh; // mHz, may be zero
	} custom_mode;

	struct nx_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
	struct nx_drm_syncobj_timeline *signal_timeline;
	uint64_t signal_point;

	struct nx_color_transform *color_transform;

	struct nx_output_image_description *image_description;

	// wp_content_type_v1 value for ALLM.
	uint32_t content_type;
};

void nx_output_state_init(struct nx_output_state *state);
void nx_output_state_finish(struct nx_output_state *state);
void nx_output_state_set_enabled(struct nx_output_state *state,
	bool enabled);
void nx_output_state_set_mode(struct nx_output_state *state,
	struct nx_output_mode *mode);
// Zero refresh lets the backend pick.
void nx_output_state_set_custom_mode(struct nx_output_state *state,
	int32_t width, int32_t height, int32_t refresh);
void nx_output_state_set_scale(struct nx_output_state *state, float scale);
void nx_output_state_set_transform(struct nx_output_state *state,
	enum wl_output_transform transform);
void nx_output_state_set_adaptive_sync_enabled(struct nx_output_state *state,
	bool enabled);
// Format of the output swapchain.
void nx_output_state_set_render_format(struct nx_output_state *state,
	uint32_t format);
void nx_output_state_set_buffer(struct nx_output_state *state,
	struct nx_buffer *buffer);
void nx_output_state_set_damage(struct nx_output_state *state,
	const pixman_region32_t *damage);
// Requires a buffer and timeline support.
void nx_output_state_set_wait_timeline(struct nx_output_state *state,
	struct nx_drm_syncobj_timeline *timeline, uint64_t src_point);
// Requires a buffer and timeline support.
void nx_output_state_set_signal_timeline(struct nx_output_state *state,
	struct nx_drm_syncobj_timeline *timeline, uint64_t dst_point);
void nx_output_state_set_color_transform(struct nx_output_state *state,
	struct nx_color_transform *tr);
bool nx_output_state_set_image_description(struct nx_output_state *state,
	const struct nx_output_image_description *image_desc);
// wp_content_type_v1 value for ALLM.
void nx_output_state_set_content_type(struct nx_output_state *state,
	uint32_t content_type);
bool nx_output_state_copy(struct nx_output_state *dst,
	const struct nx_output_state *src);

#endif
