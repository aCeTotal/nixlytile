#include <stdlib.h>
#include "platform/buffer/linux_dmabuf/feedback.h"
#include "platform/buffer/linux_dmabuf/requests.h"
#include "platform/surface/compositor.h"

struct nx_linux_dmabuf_v1_surface {
	struct nx_surface *surface;
	struct nx_linux_dmabuf_v1 *linux_dmabuf;
	struct wl_list link; // nx_linux_dmabuf_v1.surfaces

	struct nx_addon addon;
	struct nx_linux_dmabuf_feedback_v1_compiled *feedback;

	struct wl_list feedback_resources; // wl_resource_get_link
};

static void surface_destroy(struct nx_linux_dmabuf_v1_surface *surface) {
	struct wl_resource *resource, *resource_tmp;
	wl_resource_for_each_safe(resource, resource_tmp, &surface->feedback_resources) {
		struct wl_list *link = wl_resource_get_link(resource);
		wl_list_remove(link);
		wl_list_init(link);
	}

	nx_linux_dmabuf_feedback_destroy(surface->feedback);

	nx_addon_finish(&surface->addon);
	wl_list_remove(&surface->link);
	free(surface);
}

void nx_linux_dmabuf_surfaces_destroy(struct nx_linux_dmabuf_v1 *linux_dmabuf) {
	struct nx_linux_dmabuf_v1_surface *surface, *surface_tmp;
	wl_list_for_each_safe(surface, surface_tmp, &linux_dmabuf->surfaces, link) {
		surface_destroy(surface);
	}
}

static void surface_addon_destroy(struct nx_addon *addon) {
	struct nx_linux_dmabuf_v1_surface *surface =
		wl_container_of(addon, surface, addon);
	surface_destroy(surface);
}

static const struct nx_addon_interface surface_addon_impl = {
	.name = "nx_linux_dmabuf_v1_surface",
	.destroy = surface_addon_destroy,
};

static struct nx_linux_dmabuf_v1_surface *surface_get_or_create(
		struct nx_linux_dmabuf_v1 *linux_dmabuf,
		struct nx_surface *nx_surface) {
	struct nx_addon *addon =
		nx_addon_find(&nx_surface->addons, linux_dmabuf, &surface_addon_impl);
	if (addon != NULL) {
		struct nx_linux_dmabuf_v1_surface *surface =
			wl_container_of(addon, surface, addon);
		return surface;
	}

	struct nx_linux_dmabuf_v1_surface *surface = calloc(1, sizeof(*surface));
	if (surface == NULL) {
		return NULL;
	}

	surface->surface = nx_surface;
	surface->linux_dmabuf = linux_dmabuf;
	wl_list_init(&surface->feedback_resources);
	nx_addon_init(&surface->addon, &nx_surface->addons, linux_dmabuf,
		&surface_addon_impl);
	wl_list_insert(&linux_dmabuf->surfaces, &surface->link);

	return surface;
}

static const struct nx_linux_dmabuf_feedback_v1_compiled *surface_get_feedback(
		struct nx_linux_dmabuf_v1_surface *surface) {
	if (surface->feedback != NULL) {
		return surface->feedback;
	}
	return surface->linux_dmabuf->default_feedback;
}

static void surface_feedback_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

void nx_linux_dmabuf_get_surface_feedback(struct wl_client *client,
		struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct nx_linux_dmabuf_v1 *linux_dmabuf =
		nx_linux_dmabuf_from_resource(resource);
	struct nx_surface *nx_surface = nx_surface_from_resource(surface_resource);

	struct nx_linux_dmabuf_v1_surface *surface =
		surface_get_or_create(linux_dmabuf, nx_surface);
	if (surface == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	struct wl_resource *feedback_resource = nx_linux_dmabuf_feedback_resource_create(
		resource, id, surface_feedback_handle_resource_destroy);
	if (feedback_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_insert(&surface->feedback_resources, wl_resource_get_link(feedback_resource));

	nx_linux_dmabuf_feedback_send(surface_get_feedback(surface), feedback_resource);
}

bool nx_linux_dmabuf_v1_set_surface_feedback(
		struct nx_linux_dmabuf_v1 *linux_dmabuf,
		struct nx_surface *nx_surface,
		const struct nx_linux_dmabuf_feedback_v1 *feedback) {
	struct nx_linux_dmabuf_v1_surface *surface =
		surface_get_or_create(linux_dmabuf, nx_surface);
	if (surface == NULL) {
		return false;
	}

	struct nx_linux_dmabuf_feedback_v1_compiled *compiled =
		feedback != NULL ? nx_linux_dmabuf_feedback_compile(feedback) : NULL;
	if (feedback != NULL && compiled == NULL) {
		return false;
	}

	nx_linux_dmabuf_feedback_destroy(surface->feedback);
	surface->feedback = compiled;

	struct wl_resource *resource;
	wl_resource_for_each(resource, &surface->feedback_resources) {
		nx_linux_dmabuf_feedback_send(surface_get_feedback(surface), resource);
	}

	return true;
}
