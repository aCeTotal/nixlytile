#ifndef PLATFORM_NESTED_OUTPUT_OUTPUT_H
#define PLATFORM_NESTED_OUTPUT_OUTPUT_H

#include "platform/nested/backend.h"
#include "platform/output/output.h"
#include "platform/render/drm_syncobj.h"

struct nx_wl_pointer;

// Host-side wl_buffer for a local buffer.
struct nx_wl_buffer {
	struct nx_buffer *buffer;
	struct wl_buffer *wl_buffer;
	bool released;
	struct wl_list link; // nx_wl_backend.buffers
	struct wl_listener buffer_destroy;

	bool has_drm_syncobj_waiter;
	struct nx_drm_syncobj_timeline_waiter drm_syncobj_waiter;

	struct nx_drm_syncobj_timeline *fallback_signal_timeline;
	uint64_t fallback_signal_point;
};

// An xdg_toplevel on the host.
struct nx_wl_output {
	struct nx_output nx_output;

	struct nx_wl_backend *backend;
	struct wl_list link; // nx_wl_backend.outputs

	struct wl_surface *surface;
	struct wl_callback *frame_callback;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1;
	struct wp_linux_drm_syncobj_surface_v1 *drm_syncobj_surface_v1;
	struct wl_list presentation_feedbacks;

	// Zero when not requested.
	int32_t requested_width, requested_height;

	uint32_t configure_serial;
	bool has_configure_serial;
	bool configured;
	bool initialized;

	// Set until the host acks unmapping.
	struct wl_callback *unmap_callback;

	uint32_t enter_serial;

	struct {
		struct nx_wl_pointer *pointer;
		struct wl_surface *surface;
		int32_t hotspot_x, hotspot_y;
	} cursor;
};

struct nx_wl_output *nested_output_from_output(struct nx_output *output);
// NULL for foreign surfaces.
struct nx_wl_output *nested_output_from_surface(struct nx_wl_backend *wl,
	struct wl_surface *surface);

bool nested_output_test(struct nx_output *output,
	const struct nx_output_state *state);
bool nested_output_commit(struct nx_output *output,
	const struct nx_output_state *state);
// Requests frame timing and commits.
bool nested_output_submit(struct nx_wl_output *output);
void nested_output_drop_feedbacks(struct nx_wl_output *output);

bool nested_output_set_cursor(struct nx_output *output,
	struct nx_buffer *buffer, int hotspot_x, int hotspot_y);
bool nested_output_move_cursor(struct nx_output *output, int x, int y);
void nested_output_update_cursor(struct nx_wl_output *output);

bool nested_buffer_test(struct nx_wl_backend *wl, struct nx_buffer *buffer);
// Reuses buffers the host released.
struct nx_wl_buffer *nested_buffer_get_or_create(struct nx_wl_backend *wl,
	struct nx_buffer *buffer);
void nested_buffer_sync_ready(struct nx_drm_syncobj_timeline_waiter *waiter);
void nested_buffers_destroy(struct nx_wl_backend *wl);

// Sets acquire and release points.
bool nested_sync_apply(struct nx_wl_output *output,
	const struct nx_output_state *state, struct nx_wl_buffer *buffer);
void nested_timelines_destroy(struct nx_wl_backend *wl);

#endif
