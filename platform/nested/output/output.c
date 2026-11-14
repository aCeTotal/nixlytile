#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "platform/log/log.h"
#include "platform/nested/output/output.h"
#include "platform/nested/seat/seat.h"
#include "platform/output/output_interface.h"

#include "linux-drm-syncobj-v1-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static const int32_t DEFAULT_WIDTH = 1280;
static const int32_t DEFAULT_HEIGHT = 720;

static size_t last_output_num = 0;

// Proxy tag marking our surfaces.
static const char *surface_tag = "nx_wl_output";

static const struct nx_output_impl output_impl;

static bool output_is_nested(struct nx_output *nx_output) {
	return nx_output->impl == &output_impl;
}

struct nx_wl_output *nested_output_from_output(struct nx_output *nx_output) {
	assert(output_is_nested(nx_output));
	struct nx_wl_output *output = wl_container_of(nx_output, output, nx_output);
	return output;
}

struct nx_wl_output *nested_output_from_surface(struct nx_wl_backend *wl,
		struct wl_surface *surface) {
	if (wl_proxy_get_tag((struct wl_proxy *)surface) != &surface_tag) {
		return NULL;
	}
	struct nx_wl_output *output = wl_surface_get_user_data(surface);
	assert(output != NULL);
	if (output->backend != wl) {
		return NULL;
	}
	return output;
}

static const struct nx_drm_format_set *output_get_formats(
		struct nx_output *nx_output, uint32_t buffer_caps) {
	struct nx_wl_output *output = nested_output_from_output(nx_output);
	if (buffer_caps & NX_BUFFER_CAP_DMABUF) {
		return &output->backend->linux_dmabuf_v1_formats;
	}
	if (buffer_caps & NX_BUFFER_CAP_SHM) {
		return &output->backend->shm_formats;
	}
	return NULL;
}

static void output_destroy(struct nx_output *nx_output) {
	struct nx_wl_output *output = nested_output_from_output(nx_output);

	nx_output_finish(nx_output);
	wl_list_remove(&output->link);

	if (output->cursor.surface) {
		wl_surface_destroy(output->cursor.surface);
	}
	if (output->frame_callback) {
		wl_callback_destroy(output->frame_callback);
	}
	nested_output_drop_feedbacks(output);
	if (output->unmap_callback) {
		wl_callback_destroy(output->unmap_callback);
	}
	if (output->drm_syncobj_surface_v1) {
		wp_linux_drm_syncobj_surface_v1_destroy(output->drm_syncobj_surface_v1);
	}
	if (output->zxdg_toplevel_decoration_v1) {
		zxdg_toplevel_decoration_v1_destroy(output->zxdg_toplevel_decoration_v1);
	}
	if (output->xdg_toplevel) {
		xdg_toplevel_destroy(output->xdg_toplevel);
	}
	if (output->xdg_surface) {
		xdg_surface_destroy(output->xdg_surface);
	}
	wl_surface_destroy(output->surface);
	wl_display_flush(output->backend->remote_display);

	free(output);
}

static const struct nx_output_impl output_impl = {
	.destroy = output_destroy,
	.test = nested_output_test,
	.commit = nested_output_commit,
	.set_cursor = nested_output_set_cursor,
	.move_cursor = nested_output_move_cursor,
	.get_cursor_formats = output_get_formats,
	.get_primary_formats = output_get_formats,
};

static void xdg_surface_handle_configure(void *data,
		struct xdg_surface *xdg_surface, uint32_t serial) {
	struct nx_wl_output *output = data;
	assert(output && output->xdg_surface == xdg_surface);

	int32_t req_width = output->nx_output.width;
	int32_t req_height = output->nx_output.height;
	if (output->requested_width > 0) {
		req_width = output->requested_width;
		output->requested_width = 0;
	}
	if (output->requested_height > 0) {
		req_height = output->requested_height;
		output->requested_height = 0;
	}

	if (output->unmap_callback != NULL) {
		return;
	}

	output->configured = true;
	output->has_configure_serial = true;
	output->configure_serial = serial;

	// Enabling commits wait for this.
	if (!output->nx_output.enabled) {
		return;
	}

	struct nx_output_state state;
	nx_output_state_init(&state);
	nx_output_state_set_custom_mode(&state, req_width, req_height, 0);
	nx_output_send_request_state(&output->nx_output, &state);
	nx_output_state_finish(&state);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_handle_configure,
};

static void xdg_toplevel_handle_configure(void *data,
		struct xdg_toplevel *xdg_toplevel,
		int32_t width, int32_t height, struct wl_array *states) {
	struct nx_wl_output *output = data;
	assert(output && output->xdg_toplevel == xdg_toplevel);

	if (width > 0) {
		output->requested_width = width;
	}
	if (height > 0) {
		output->requested_height = height;
	}
}

static void xdg_toplevel_handle_close(void *data,
		struct xdg_toplevel *xdg_toplevel) {
	struct nx_wl_output *output = data;
	assert(output && output->xdg_toplevel == xdg_toplevel);

	nx_output_destroy(&output->nx_output);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_handle_configure,
	.close = xdg_toplevel_handle_close,
};

static struct nx_wl_output *output_create(struct nx_wl_backend *backend,
		struct wl_surface *surface) {
	struct nx_wl_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		nx_log(NX_ERROR, "Failed to allocate nx_wl_output");
		return NULL;
	}
	struct nx_output *nx_output = &output->nx_output;

	struct nx_output_state state;
	nx_output_state_init(&state);
	nx_output_state_set_custom_mode(&state, DEFAULT_WIDTH, DEFAULT_HEIGHT, 0);
	nx_output_init(nx_output, &backend->backend, &output_impl,
		backend->event_loop, &state);
	nx_output_state_finish(&state);

	// The host decides adaptive sync.
	nx_output->adaptive_sync_status = NX_OUTPUT_ADAPTIVE_SYNC_ENABLED;

	size_t output_num = ++last_output_num;

	char name[64];
	snprintf(name, sizeof(name), "WL-%zu", output_num);
	nx_output_set_name(nx_output, name);

	char description[128];
	snprintf(description, sizeof(description), "Wayland output %zu", output_num);
	nx_output_set_description(nx_output, description);

	output->surface = surface;
	output->backend = backend;
	wl_list_init(&output->presentation_feedbacks);

	wl_proxy_set_tag((struct wl_proxy *)output->surface, &surface_tag);
	wl_surface_set_user_data(output->surface, output);

	wl_list_insert(&backend->outputs, &output->link);
	return output;
}

static bool request_server_decoration(struct nx_wl_output *output) {
	struct zxdg_decoration_manager_v1 *manager =
		output->backend->zxdg_decoration_manager_v1;
	if (manager == NULL) {
		return true;
	}

	output->zxdg_toplevel_decoration_v1 =
		zxdg_decoration_manager_v1_get_toplevel_decoration(manager,
			output->xdg_toplevel);
	if (!output->zxdg_toplevel_decoration_v1) {
		nx_log_errno(NX_ERROR, "Could not get xdg toplevel decoration");
		return false;
	}
	zxdg_toplevel_decoration_v1_set_mode(output->zxdg_toplevel_decoration_v1,
		ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	return true;
}

static bool create_toplevel(struct nx_wl_output *output) {
	output->xdg_surface = xdg_wm_base_get_xdg_surface(
		output->backend->xdg_wm_base, output->surface);
	if (!output->xdg_surface) {
		nx_log_errno(NX_ERROR, "Could not get xdg surface");
		return false;
	}

	output->xdg_toplevel = xdg_surface_get_toplevel(output->xdg_surface);
	if (!output->xdg_toplevel) {
		nx_log_errno(NX_ERROR, "Could not get xdg toplevel");
		return false;
	}

	if (!request_server_decoration(output)) {
		return false;
	}

	xdg_surface_add_listener(output->xdg_surface, &xdg_surface_listener, output);
	xdg_toplevel_add_listener(output->xdg_toplevel, &xdg_toplevel_listener, output);
	return true;
}

static void output_start(struct nx_wl_output *output) {
	struct nx_wl_backend *backend = output->backend;

	wl_signal_emit_mutable(&backend->backend.events.new_output, &output->nx_output);

	struct nx_wl_seat *seat;
	wl_list_for_each(seat, &backend->seats, link) {
		if (seat->wl_pointer) {
			nested_pointer_create(seat, output);
		}
	}
}

struct nx_output *nx_wl_output_create(struct nx_backend *nx_backend) {
	struct nx_wl_backend *backend = nested_backend_from_backend(nx_backend);
	if (!backend->started) {
		++backend->requested_outputs;
		return NULL;
	}

	struct wl_surface *surface = wl_compositor_create_surface(backend->compositor);
	if (surface == NULL) {
		nx_log(NX_ERROR, "Could not create output surface");
		return NULL;
	}

	struct nx_wl_output *output = output_create(backend, surface);
	if (output == NULL) {
		wl_surface_destroy(surface);
		return NULL;
	}
	if (!create_toplevel(output)) {
		nx_output_destroy(&output->nx_output);
		return NULL;
	}

	wl_display_flush(backend->remote_display);
	output_start(output);

	if (backend->activation_v1 && backend->activation_token) {
		xdg_activation_v1_activate(backend->activation_v1,
			backend->activation_token, output->surface);
	}
	return &output->nx_output;
}
