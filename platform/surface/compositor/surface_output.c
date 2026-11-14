#include <assert.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>
#include "platform/os/clock.h"
#include "platform/output/output.h"
#include "platform/surface/compositor/surface.h"

void surface_output_destroy(struct nx_surface_output *surface_output) {
	wl_list_remove(&surface_output->bind.link);
	wl_list_remove(&surface_output->destroy.link);
	wl_list_remove(&surface_output->link);

	free(surface_output);
}

static void surface_handle_output_bind(struct wl_listener *listener,
		void *data) {
	struct nx_output_event_bind *evt = data;
	struct nx_surface_output *surface_output =
		wl_container_of(listener, surface_output, bind);
	struct wl_client *client = wl_resource_get_client(
		surface_output->surface->resource);
	if (client == wl_resource_get_client(evt->resource)) {
		wl_surface_send_enter(surface_output->surface->resource, evt->resource);
	}
}

static void surface_handle_output_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_surface_output *surface_output =
		wl_container_of(listener, surface_output, destroy);
	surface_output_destroy(surface_output);
}

static struct nx_surface_output *find_surface_output(struct nx_surface *surface,
		struct nx_output *output) {
	struct nx_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		if (surface_output->output == output) {
			return surface_output;
		}
	}
	return NULL;
}

void nx_surface_send_enter(struct nx_surface *surface,
		struct nx_output *output) {
	if (find_surface_output(surface, output) != NULL) {
		return;
	}

	struct nx_surface_output *surface_output = calloc(1, sizeof(*surface_output));
	if (surface_output == NULL) {
		return;
	}
	surface_output->bind.notify = surface_handle_output_bind;
	surface_output->destroy.notify = surface_handle_output_destroy;

	wl_signal_add(&output->events.bind, &surface_output->bind);
	wl_signal_add(&output->events.destroy, &surface_output->destroy);

	surface_output->surface = surface;
	surface_output->output = output;
	wl_list_insert(&surface->current_outputs, &surface_output->link);

	struct wl_client *client = wl_resource_get_client(surface->resource);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &output->resources) {
		if (client == wl_resource_get_client(resource)) {
			wl_surface_send_enter(surface->resource, resource);
		}
	}
}

void nx_surface_send_leave(struct nx_surface *surface,
		struct nx_output *output) {
	struct nx_surface_output *surface_output = find_surface_output(surface, output);
	if (surface_output == NULL) {
		return;
	}
	surface_output_destroy(surface_output);

	struct wl_client *client = wl_resource_get_client(surface->resource);
	struct wl_resource *resource;
	wl_resource_for_each(resource, &output->resources) {
		if (client == wl_resource_get_client(resource)) {
			wl_surface_send_leave(surface->resource, resource);
		}
	}
}

void nx_surface_send_frame_done(struct nx_surface *surface,
		const struct timespec *when) {
	uint32_t time_ms = (uint32_t)timespec_to_msec(when);
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp,
			&surface->current.frame_callback_list) {
		wl_callback_send_done(resource, time_ms);
		wl_resource_destroy(resource);
	}
}

void nx_surface_set_preferred_buffer_scale(struct nx_surface *surface,
		int32_t scale) {
	assert(scale > 0);

	if (wl_resource_get_version(surface->resource) <
			WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION) {
		return;
	}

	if (surface->preferred_buffer_scale == scale) {
		return;
	}

	wl_surface_send_preferred_buffer_scale(surface->resource, scale);
	surface->preferred_buffer_scale = scale;
}

void nx_surface_set_preferred_buffer_transform(struct nx_surface *surface,
		enum wl_output_transform transform) {
	if (wl_resource_get_version(surface->resource) <
			WL_SURFACE_PREFERRED_BUFFER_TRANSFORM_SINCE_VERSION) {
		return;
	}

	if (surface->preferred_buffer_transform == transform &&
			surface->preferred_buffer_transform_sent) {
		return;
	}

	wl_surface_send_preferred_buffer_transform(surface->resource, transform);
	surface->preferred_buffer_transform_sent = true;
	surface->preferred_buffer_transform = transform;
}
