#include <assert.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/surface/compositor/surface.h"

static void pending_buffer_resource_handle_destroy(struct wl_listener *listener, void *data) {
	struct nx_surface *surface =
		wl_container_of(listener, surface, pending_buffer_resource_destroy);
	surface_set_pending_buffer_resource(surface, NULL);
}

void surface_set_pending_buffer_resource(struct nx_surface *surface,
		struct wl_resource *resource) {
	wl_list_remove(&surface->pending_buffer_resource_destroy.link);
	surface->pending_buffer_resource = resource;
	if (resource == NULL) {
		wl_list_init(&surface->pending_buffer_resource_destroy.link);
		return;
	}
	wl_resource_add_destroy_listener(resource, &surface->pending_buffer_resource_destroy);
}

struct nx_surface *nx_surface_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_surface_interface,
		&surface_implementation));
	return wl_resource_get_user_data(resource);
}

static void assert_no_listeners(struct nx_surface *surface) {
	assert(wl_list_empty(&surface->events.client_commit.listener_list));
	assert(wl_list_empty(&surface->events.commit.listener_list));
	assert(wl_list_empty(&surface->events.map.listener_list));
	assert(wl_list_empty(&surface->events.unmap.listener_list));
	assert(wl_list_empty(&surface->events.destroy.listener_list));
	assert(wl_list_empty(&surface->events.new_subsurface.listener_list));
	assert(wl_list_empty(&surface->synced));
}

static void surface_handle_resource_destroy(struct wl_resource *resource) {
	struct nx_surface *surface = nx_surface_from_resource(resource);

	surface_destroy_role_object(surface);

	wl_signal_emit_mutable(&surface->events.destroy, surface);
	nx_addon_set_finish(&surface->addons);
	assert_no_listeners(surface);

	struct nx_surface_state *cached, *cached_tmp;
	wl_list_for_each_safe(cached, cached_tmp, &surface->cached, cached_state_link) {
		surface_state_destroy_cached(cached, surface);
	}

	wl_list_remove(&surface->role_resource_destroy.link);
	wl_list_remove(&surface->pending_buffer_resource_destroy.link);

	surface_state_finish(&surface->pending);
	surface_state_finish(&surface->current);
	pixman_region32_fini(&surface->buffer_damage);
	pixman_region32_fini(&surface->opaque_region);
	pixman_region32_fini(&surface->input_region);
	if (surface->buffer != NULL) {
		nx_buffer_unlock(&surface->buffer->base);
	}

	struct nx_surface_output *surface_output, *surface_output_tmp;
	wl_list_for_each_safe(surface_output, surface_output_tmp,
			&surface->current_outputs, link) {
		surface_output_destroy(surface_output);
	}

	free(surface);
}

static void surface_init(struct nx_surface *surface, struct nx_compositor *compositor) {
	surface->compositor = compositor;

	surface_state_init(&surface->current, surface);
	surface_state_init(&surface->pending, surface);
	surface->pending.seq = 1;

	wl_signal_init(&surface->events.client_commit);
	wl_signal_init(&surface->events.commit);
	wl_signal_init(&surface->events.map);
	wl_signal_init(&surface->events.unmap);
	wl_signal_init(&surface->events.destroy);
	wl_signal_init(&surface->events.new_subsurface);

	wl_list_init(&surface->current_outputs);
	wl_list_init(&surface->cached);
	pixman_region32_init(&surface->buffer_damage);
	pixman_region32_init(&surface->opaque_region);
	pixman_region32_init(&surface->input_region);
	nx_addon_set_init(&surface->addons);
	wl_list_init(&surface->synced);

	wl_list_init(&surface->role_resource_destroy.link);

	surface->pending_buffer_resource_destroy.notify = pending_buffer_resource_handle_destroy;
	wl_list_init(&surface->pending_buffer_resource_destroy.link);
}

struct nx_surface *surface_create(struct wl_client *client,
		uint32_t version, uint32_t id, struct nx_compositor *compositor) {
	struct nx_surface *surface = calloc(1, sizeof(*surface));
	if (!surface) {
		wl_client_post_no_memory(client);
		return NULL;
	}
	surface->resource = wl_resource_create(client, &wl_surface_interface,
		version, id);
	if (surface->resource == NULL) {
		free(surface);
		wl_client_post_no_memory(client);
		return NULL;
	}
	wl_resource_set_implementation(surface->resource, &surface_implementation,
		surface, surface_handle_resource_destroy);

	nx_log(NX_DEBUG, "New nx_surface %p (res %p)",
		(void *)surface, (void *)surface->resource);

	surface_init(surface, compositor);
	return surface;
}

struct nx_texture *nx_surface_get_texture(struct nx_surface *surface) {
	if (surface->buffer == NULL) {
		return NULL;
	}
	return surface->buffer->texture;
}

bool nx_surface_has_buffer(struct nx_surface *surface) {
	return nx_surface_state_has_buffer(&surface->current);
}

bool nx_surface_state_has_buffer(const struct nx_surface_state *state) {
	return state->buffer_width > 0 && state->buffer_height > 0;
}
