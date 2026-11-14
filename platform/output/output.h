#ifndef PLATFORM_OUTPUT_OUTPUT_H
#define PLATFORM_OUTPUT_OUTPUT_H

#include <pixman.h>
#include <stdbool.h>
#include <time.h>
#include <wayland-server-protocol.h>
#include <wayland-util.h>
#include "platform/render/color.h"
#include "platform/render/renderer.h"
#include "platform/buffer/buffer.h"
#include "platform/container/addon.h"
#include "platform/geometry/box.h"
#include "platform/output/commit/state.h"

enum nx_output_mode_aspect_ratio {
	NX_OUTPUT_MODE_ASPECT_RATIO_NONE,
	NX_OUTPUT_MODE_ASPECT_RATIO_4_3,
	NX_OUTPUT_MODE_ASPECT_RATIO_16_9,
	NX_OUTPUT_MODE_ASPECT_RATIO_64_27,
	NX_OUTPUT_MODE_ASPECT_RATIO_256_135,
};

struct nx_output_mode {
	int32_t width, height;
	int32_t refresh; // mHz
	bool preferred;
	enum nx_output_mode_aspect_ratio picture_aspect_ratio;
	struct wl_list link;
};

struct nx_output_cursor {
	struct nx_output *output;
	double x, y;
	bool enabled;
	bool visible;
	uint32_t width, height;
	struct nx_fbox src_box;
	enum wl_output_transform transform;
	int32_t hotspot_x, hotspot_y;
	struct nx_texture *texture;
	bool own_texture;
	struct nx_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
	struct wl_list link;

	struct {
		struct wl_listener renderer_destroy;
		struct nx_color_transform *color_transform;
		struct nx_buffer *source_buffer; // for direct plane passthrough
	};
};

enum nx_output_adaptive_sync_status {
	NX_OUTPUT_ADAPTIVE_SYNC_DISABLED,
	NX_OUTPUT_ADAPTIVE_SYNC_ENABLED,
};

struct nx_output_impl;
struct nx_render_pass;

struct nx_output {
	const struct nx_output_impl *impl;
	struct nx_backend *backend;
	struct wl_event_loop *event_loop;

	struct wl_global *global;
	struct wl_list resources;

	char *name;
	char *description; // may be NULL
	char *make, *model, *serial; // may be NULL
	int32_t phys_width, phys_height; // mm
	const struct nx_color_primaries *default_primaries; // may be NULL

	struct wl_list modes; // nx_output_mode.link, may be empty
	struct nx_output_mode *current_mode;
	int32_t width, height;
	int32_t refresh; // mHz, may be zero

	uint32_t supported_primaries; // bitfield of enum nx_color_named_primaries
	uint32_t supported_transfer_functions; // bitfield of enum nx_color_transfer_function

	bool enabled;
	float scale;
	enum wl_output_subpixel subpixel;
	enum wl_output_transform transform;
	enum nx_output_adaptive_sync_status adaptive_sync_status;
	uint32_t render_format;
	const struct nx_output_image_description *image_description;

	// Else adaptive sync changes fail.
	bool adaptive_sync_supported;

	bool needs_frame;
	bool frame_pending;

	bool non_desktop;

	uint32_t commit_seq; // may overflow

	struct {
		struct wl_signal frame;
		struct wl_signal damage; // struct nx_output_event_damage
		struct wl_signal needs_frame;
		struct wl_signal precommit; // struct nx_output_event_precommit
		struct wl_signal commit; // struct nx_output_event_commit
		struct wl_signal present; // struct nx_output_event_present
		struct wl_signal bind; // struct nx_output_event_bind
		struct wl_signal description;
		struct wl_signal request_state; // struct nx_output_event_request_state
		struct wl_signal destroy;
	} events;

	struct wl_event_source *idle_frame;
	struct wl_event_source *idle_done;

	int attach_render_locks; // forces rendering over scan-out

	struct wl_list cursors; // nx_output_cursor.link
	struct nx_output_cursor *hardware_cursor;
	struct nx_swapchain *cursor_swapchain;
	bool cursor_format_failed; // sticky cursor format failure
	struct nx_buffer *cursor_front_buffer;
	int software_cursor_locks; // forces software cursors

	struct nx_allocator *allocator;
	struct nx_renderer *renderer;
	struct nx_swapchain *swapchain;

	struct nx_addon_set addons;

	void *data;

	struct {
		struct wl_listener display_destroy;
		struct nx_output_image_description image_description_value;
		struct nx_color_transform *color_transform;
		struct nx_color_primaries default_primaries_value;
	};
};

struct nx_output_event_damage {
	const pixman_region32_t *damage; // output-buffer-local coordinates
};

struct nx_output_event_precommit {
	const struct nx_output_state *state;
};

struct nx_output_event_commit {
	struct timespec when;
	const struct nx_output_state *state;
};

enum nx_output_present_flag {
	NX_OUTPUT_PRESENT_VSYNC = 0x1,
	NX_OUTPUT_PRESENT_HW_CLOCK = 0x2,
	NX_OUTPUT_PRESENT_HW_COMPLETION = 0x4,
	NX_OUTPUT_PRESENT_ZERO_COPY = 0x8,
};

struct nx_output_event_present {
	struct nx_output *output;
	uint32_t commit_seq; // see nx_output.commit_seq
	bool presented;
	struct timespec when; // first light of the update
	unsigned seq; // retrace counter, may be zero
	int refresh; // nsec, zero if unknown
	uint32_t flags; // enum nx_output_present_flag
};

struct nx_output_event_bind {
	struct wl_resource *resource;
};

struct nx_output_event_request_state {
	struct nx_output *output;
	const struct nx_output_state *state;
};

void nx_output_create_global(struct nx_output *output, struct wl_display *display);
void nx_output_destroy_global(struct nx_output *output);
// Call again to switch renderer.
bool nx_output_init_render(struct nx_output *output,
	struct nx_allocator *allocator, struct nx_renderer *renderer);
// NULL for modeless outputs.
struct nx_output_mode *nx_output_preferred_mode(struct nx_output *output);
// Unique, fixed once advertised.
void nx_output_set_name(struct nx_output *output, const char *name);
void nx_output_set_description(struct nx_output *output, const char *desc);
// Batches wl_output.done for add-ons.
void nx_output_schedule_done(struct nx_output *output);
void nx_output_destroy(struct nx_output *output);
void nx_output_transformed_resolution(struct nx_output *output,
	int *width, int *height);
// Transformed and scaled resolution.
void nx_output_effective_resolution(struct nx_output *output,
	int *width, int *height);
bool nx_output_test_state(struct nx_output *output,
	const struct nx_output_state *state);
// Atomic: nothing applies on failure.
bool nx_output_commit_state(struct nx_output *output,
	const struct nx_output_state *state);
void nx_output_schedule_frame(struct nx_output *output);
// Zero means no gamma LUT.
size_t nx_output_get_gamma_size(struct nx_output *output);
// NULL for inert resources.
struct nx_output *nx_output_from_resource(struct wl_resource *resource);
// Counted lock against direct scan-out.
void nx_output_lock_attach_render(struct nx_output *output, bool lock);
// Counted lock against hardware cursors.
void nx_output_lock_software_cursors(struct nx_output *output, bool lock);
// Damage is buffer-local.
void nx_output_add_software_cursors_to_render_pass(struct nx_output *output,
	struct nx_render_pass *render_pass, const pixman_region32_t *damage);
// NULL means any format.
const struct nx_drm_format_set *nx_output_get_primary_formats(
	struct nx_output *output, uint32_t buffer_caps);
bool nx_output_is_direct_scanout_allowed(struct nx_output *output);

struct nx_output_cursor *nx_output_cursor_create(struct nx_output *output);
bool nx_output_cursor_set_buffer(struct nx_output_cursor *cursor,
	struct nx_buffer *buffer, int32_t hotspot_x, int32_t hotspot_y);
bool nx_output_cursor_move(struct nx_output_cursor *cursor,
	double x, double y);
void nx_output_cursor_destroy(struct nx_output_cursor *cursor);

// Reuses a matching swapchain.
bool nx_output_configure_primary_swapchain(struct nx_output *output,
	const struct nx_output_state *state, struct nx_swapchain **swapchain);

#endif
