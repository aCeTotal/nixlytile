#include <assert.h>
#include <stdlib.h>
#include <wayland-server-protocol.h>
#include "platform/render/renderer.h"
#include "platform/surface/compositor/region.h"
#include "platform/surface/compositor/surface.h"

#define COMPOSITOR_VERSION 6

static const struct wl_compositor_interface compositor_impl;

static struct nx_compositor *compositor_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_compositor_interface,
		&compositor_impl));
	return wl_resource_get_user_data(resource);
}

static void compositor_create_surface(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	struct nx_compositor *compositor = compositor_from_resource(resource);

	struct nx_surface *surface = surface_create(client,
		wl_resource_get_version(resource), id, compositor);
	if (surface == NULL) {
		return;
	}

	wl_signal_emit_mutable(&compositor->events.new_surface, surface);
}

static void compositor_create_region(struct wl_client *client,
		struct wl_resource *resource, uint32_t id) {
	region_create(client, wl_resource_get_version(resource), id);
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = compositor_create_surface,
	.create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_compositor *compositor = data;

	struct wl_resource *resource =
		wl_resource_create(wl_client, &wl_compositor_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(resource, &compositor_impl, compositor, NULL);
}

static void compositor_handle_display_destroy(
		struct wl_listener *listener, void *data) {
	struct nx_compositor *compositor =
		wl_container_of(listener, compositor, display_destroy);
	wl_signal_emit_mutable(&compositor->events.destroy, NULL);

	assert(wl_list_empty(&compositor->events.new_surface.listener_list));
	assert(wl_list_empty(&compositor->events.destroy.listener_list));

	wl_list_remove(&compositor->display_destroy.link);
	wl_list_remove(&compositor->renderer_destroy.link);
	wl_global_destroy(compositor->global);
	free(compositor);
}

static void compositor_handle_renderer_destroy(
		struct wl_listener *listener, void *data) {
	struct nx_compositor *compositor =
		wl_container_of(listener, compositor, renderer_destroy);
	nx_compositor_set_renderer(compositor, NULL);
}

struct nx_compositor *nx_compositor_create(struct wl_display *display,
		uint32_t version, struct nx_renderer *renderer) {
	assert(version <= COMPOSITOR_VERSION);

	struct nx_compositor *compositor = calloc(1, sizeof(*compositor));
	if (!compositor) {
		return NULL;
	}

	compositor->global = wl_global_create(display, &wl_compositor_interface,
		version, compositor, compositor_bind);
	if (!compositor->global) {
		free(compositor);
		return NULL;
	}

	wl_signal_init(&compositor->events.new_surface);
	wl_signal_init(&compositor->events.destroy);

	wl_list_init(&compositor->renderer_destroy.link);

	compositor->display_destroy.notify = compositor_handle_display_destroy;
	wl_display_add_destroy_listener(display, &compositor->display_destroy);

	nx_compositor_set_renderer(compositor, renderer);

	return compositor;
}

void nx_compositor_set_renderer(struct nx_compositor *compositor,
		struct nx_renderer *renderer) {
	wl_list_remove(&compositor->renderer_destroy.link);
	compositor->renderer = renderer;

	if (renderer == NULL) {
		wl_list_init(&compositor->renderer_destroy.link);
		return;
	}
	compositor->renderer_destroy.notify = compositor_handle_renderer_destroy;
	wl_signal_add(&renderer->events.destroy, &compositor->renderer_destroy);
}
