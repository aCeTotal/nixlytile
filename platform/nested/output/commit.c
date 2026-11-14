#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include "platform/log/log.h"
#include "platform/nested/output/output.h"
#include "platform/output/output_interface.h"
#include "platform/output/commit/commit.h"

#include "xdg-shell-client-protocol.h"

static const char *const APP_ID = "nixlytile";

static const uint32_t SUPPORTED_OUTPUT_STATE =
	NX_OUTPUT_STATE_BACKEND_OPTIONAL |
	NX_OUTPUT_STATE_BUFFER |
	NX_OUTPUT_STATE_ENABLED |
	NX_OUTPUT_STATE_MODE |
	NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED |
	NX_OUTPUT_STATE_WAIT_TIMELINE |
	NX_OUTPUT_STATE_SIGNAL_TIMELINE;

// The host neither scales nor crops.
static bool buffer_fits_output(struct nx_output *nx_output,
		const struct nx_output_state *state) {
	int pending_width, pending_height;
	output_pending_resolution(nx_output, state, &pending_width, &pending_height);
	if (state->buffer->width != pending_width ||
			state->buffer->height != pending_height) {
		nx_log(NX_DEBUG, "Primary buffer size mismatch");
		return false;
	}

	struct nx_fbox src_box;
	output_state_get_buffer_src_box(state, &src_box);
	if (src_box.x != 0.0 || src_box.y != 0.0 ||
			src_box.width != (double)state->buffer->width ||
			src_box.height != (double)state->buffer->height) {
		nx_log(NX_DEBUG, "Source crop not supported in wayland output");
		return false;
	}
	return true;
}

bool nested_output_test(struct nx_output *nx_output,
		const struct nx_output_state *state) {
	struct nx_wl_output *output = nested_output_from_output(nx_output);
	uint32_t committed = state->committed;

	if ((committed & NX_OUTPUT_STATE_BUFFER) && !buffer_fits_output(nx_output, state)) {
		return false;
	}

	uint32_t unsupported = committed & ~SUPPORTED_OUTPUT_STATE;
	if (unsupported != 0) {
		nx_log(NX_DEBUG, "Unsupported output state fields: 0x%"PRIx32,
			unsupported);
		return false;
	}

	assert(nx_output->adaptive_sync_status == NX_OUTPUT_ADAPTIVE_SYNC_ENABLED);
	if ((committed & NX_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED) &&
			!state->adaptive_sync_enabled) {
		nx_log(NX_DEBUG, "Disabling adaptive sync is not supported");
		return false;
	}

	assert(!(committed & NX_OUTPUT_STATE_MODE) ||
		state->mode_type == NX_OUTPUT_STATE_MODE_CUSTOM);
	if ((committed & NX_OUTPUT_STATE_MODE) && state->custom_mode.refresh != 0) {
		nx_log(NX_DEBUG, "Refresh rates are not supported");
		return false;
	}

	if ((committed & NX_OUTPUT_STATE_BUFFER) &&
			!nested_buffer_test(output->backend, state->buffer)) {
		nx_log(NX_DEBUG, "Unsupported buffer format");
		return false;
	}

	if ((committed & NX_OUTPUT_STATE_SIGNAL_TIMELINE) &&
			!(committed & NX_OUTPUT_STATE_WAIT_TIMELINE)) {
		nx_log(NX_DEBUG, "Signal timeline requires a wait timeline");
		return false;
	}

	struct nx_dmabuf_attributes dmabuf;
	if ((committed & (NX_OUTPUT_STATE_WAIT_TIMELINE | NX_OUTPUT_STATE_SIGNAL_TIMELINE)) &&
			!nx_buffer_get_dmabuf(state->buffer, &dmabuf)) {
		nx_log(NX_DEBUG, "Wait/signal timelines require DMA-BUFs");
		return false;
	}
	return true;
}

static void unmap_callback_handle_done(void *data, struct wl_callback *callback,
		uint32_t cb_data) {
	struct nx_wl_output *output = data;
	output->unmap_callback = NULL;
	wl_callback_destroy(callback);
}

static const struct wl_callback_listener unmap_callback_listener = {
	.done = unmap_callback_handle_done,
};

static bool output_unmap(struct nx_wl_output *output) {
	output->unmap_callback = wl_display_sync(output->backend->remote_display);
	if (output->unmap_callback == NULL) {
		return false;
	}
	wl_callback_add_listener(output->unmap_callback, &unmap_callback_listener, output);

	wl_surface_attach(output->surface, NULL, 0, 0);
	wl_surface_commit(output->surface);

	output->initialized = false;
	output->configured = false;
	output->has_configure_serial = false;
	output->requested_width = output->requested_height = 0;
	return true;
}

static void dispatch_until_configured(struct nx_wl_output *output) {
	struct nx_wl_backend *wl = output->backend;
	struct wl_event_queue *display_queue =
		wl_proxy_get_queue((struct wl_proxy *)wl->remote_display);
	wl_proxy_set_queue((struct wl_proxy *)output->xdg_surface, wl->busy_loop_queue);
	wl_proxy_set_queue((struct wl_proxy *)output->xdg_toplevel, wl->busy_loop_queue);

	wl_display_flush(wl->remote_display);
	while (!output->configured) {
		if (wl_display_dispatch_queue(wl->remote_display, wl->busy_loop_queue) == -1) {
			nx_log(NX_ERROR, "wl_display_dispatch_queue() failed");
			break;
		}
	}

	wl_proxy_set_queue((struct wl_proxy *)output->xdg_surface, display_queue);
	wl_proxy_set_queue((struct wl_proxy *)output->xdg_toplevel, display_queue);
}

static bool output_map(struct nx_wl_output *output) {
	char title[64];
	snprintf(title, sizeof(title), "nixlytile - %s", output->nx_output.name);
	xdg_toplevel_set_title(output->xdg_toplevel, title);
	xdg_toplevel_set_app_id(output->xdg_toplevel, APP_ID);
	wl_surface_commit(output->surface);
	output->initialized = true;

	dispatch_until_configured(output);
	return output->configured;
}

static bool update_mapping(struct nx_wl_output *output,
		const struct nx_output_state *state) {
	bool pending_enabled = output_pending_enabled(&output->nx_output, state);
	if (output->nx_output.enabled && !pending_enabled) {
		return output_unmap(output);
	}
	if (pending_enabled && !output->initialized) {
		return output_map(output);
	}
	return true;
}

static void damage_surface(struct wl_surface *surface,
		const pixman_region32_t *damage) {
	if (damage == NULL) {
		wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
		return;
	}

	int rects_len;
	const pixman_box32_t *rects = pixman_region32_rectangles(damage, &rects_len);
	for (int i = 0; i < rects_len; i++) {
		const pixman_box32_t *r = &rects[i];
		wl_surface_damage_buffer(surface, r->x1, r->y1,
			r->x2 - r->x1, r->y2 - r->y1);
	}
}

static struct nx_wl_buffer *attach_buffer(struct nx_wl_output *output,
		const struct nx_output_state *state) {
	struct nx_wl_buffer *buffer =
		nested_buffer_get_or_create(output->backend, state->buffer);
	if (buffer == NULL) {
		return NULL;
	}
	wl_surface_attach(output->surface, buffer->wl_buffer, 0, 0);
	damage_surface(output->surface,
		(state->committed & NX_OUTPUT_STATE_DAMAGE) ? &state->damage : NULL);
	return buffer;
}

static bool finish_commit(struct nx_wl_output *output,
		const struct nx_output_state *state, struct nx_wl_buffer *buffer) {
	if (!nested_sync_apply(output, state, buffer)) {
		return false;
	}
	if (output_pending_enabled(&output->nx_output, state) &&
			!nested_output_submit(output)) {
		return false;
	}
	wl_display_flush(output->backend->remote_display);
	return true;
}

bool nested_output_commit(struct nx_output *nx_output,
		const struct nx_output_state *state) {
	struct nx_wl_output *output = nested_output_from_output(nx_output);
	if (!nested_output_test(nx_output, state) || !update_mapping(output, state)) {
		return false;
	}
	if (!(state->committed & NX_OUTPUT_STATE_BUFFER)) {
		return finish_commit(output, state, NULL);
	}

	struct nx_wl_buffer *buffer = attach_buffer(output, state);
	if (buffer == NULL) {
		return false;
	}
	return finish_commit(output, state, buffer);
}
