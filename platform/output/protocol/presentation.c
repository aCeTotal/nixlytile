#include <assert.h>
#include <stdlib.h>
#include "platform/container/addon.h"
#include "platform/output/protocol/presentation_feedback.h"
#include "presentation-time-protocol.h"

#define PRESENTATION_VERSION 2

struct nx_presentation {
	struct wl_global *global;
	struct wl_listener display_destroy;
};

struct nx_presentation_surface_state {
	struct nx_presentation_feedback *feedback;
};

struct nx_presentation_surface {
	struct nx_presentation_surface_state current, pending;

	struct nx_addon addon; // nx_surface.addons
	struct nx_surface_synced synced;
};

static void feedback_handle_resource_destroy(struct wl_resource *resource) {
	wl_list_remove(wl_resource_get_link(resource));
}

static void presentation_surface_addon_destroy(struct nx_addon *addon) {
	struct nx_presentation_surface *p_surface =
		wl_container_of(addon, p_surface, addon);

	nx_addon_finish(addon);
	nx_surface_synced_finish(&p_surface->synced);
	free(p_surface);
}

static const struct nx_addon_interface presentation_surface_addon_impl = {
	.name = "nx_presentation_surface",
	.destroy = presentation_surface_addon_destroy,
};

static void surface_synced_finish_state(void *_state) {
	struct nx_presentation_surface_state *state = _state;
	output_presentation_feedback_destroy(state->feedback);
}

static void surface_synced_move_state(void *_dst, void *_src) {
	struct nx_presentation_surface_state *dst = _dst, *src = _src;
	surface_synced_finish_state(dst);
	dst->feedback = src->feedback;
	src->feedback = NULL;
}

static const struct nx_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct nx_presentation_surface_state),
	.finish_state = surface_synced_finish_state,
	.move_state = surface_synced_move_state,
};

static struct nx_presentation_surface *presentation_surface_from_surface(
		struct nx_surface *surface) {
	struct nx_addon *addon =
		nx_addon_find(&surface->addons, NULL, &presentation_surface_addon_impl);
	if (addon == NULL) {
		return NULL;
	}
	struct nx_presentation_surface *p_surface =
		wl_container_of(addon, p_surface, addon);
	return p_surface;
}

static struct nx_presentation_surface *presentation_surface_create(
		struct nx_surface *surface) {
	struct nx_presentation_surface *p_surface = calloc(1, sizeof(*p_surface));
	if (p_surface == NULL) {
		return NULL;
	}
	if (!nx_surface_synced_init(&p_surface->synced, surface,
			&surface_synced_impl, &p_surface->pending, &p_surface->current)) {
		free(p_surface);
		return NULL;
	}
	nx_addon_init(&p_surface->addon, &surface->addons,
		NULL, &presentation_surface_addon_impl);
	return p_surface;
}

static struct nx_presentation_feedback *pending_feedback(
		struct nx_presentation_surface *p_surface) {
	if (p_surface->pending.feedback != NULL) {
		return p_surface->pending.feedback;
	}
	struct nx_presentation_feedback *feedback = calloc(1, sizeof(*feedback));
	if (feedback == NULL) {
		return NULL;
	}
	wl_list_init(&feedback->resources);
	p_surface->pending.feedback = feedback;
	return feedback;
}

static void presentation_handle_feedback(struct wl_client *client,
		struct wl_resource *presentation_resource,
		struct wl_resource *surface_resource, uint32_t id) {
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);
	struct nx_presentation_surface *p_surface =
		presentation_surface_from_surface(surface);
	if (p_surface == NULL) {
		p_surface = presentation_surface_create(surface);
	}
	struct nx_presentation_feedback *feedback =
		p_surface != NULL ? pending_feedback(p_surface) : NULL;
	if (feedback == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	uint32_t version = wl_resource_get_version(presentation_resource);
	struct wl_resource *resource = wl_resource_create(client,
		&wp_presentation_feedback_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, NULL, feedback,
		feedback_handle_resource_destroy);
	wl_list_insert(&feedback->resources, wl_resource_get_link(resource));
}

static void presentation_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_presentation_interface presentation_impl = {
	.feedback = presentation_handle_feedback,
	.destroy = presentation_handle_destroy,
};

static void presentation_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&wp_presentation_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &presentation_impl, NULL, NULL);

	wp_presentation_send_clock_id(resource, CLOCK_MONOTONIC);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_presentation *presentation =
		wl_container_of(listener, presentation, display_destroy);
	wl_list_remove(&presentation->display_destroy.link);
	wl_global_destroy(presentation->global);
	free(presentation);
}

struct nx_presentation *nx_presentation_create(struct wl_display *display,
		struct nx_backend *backend, uint32_t version) {
	assert(version <= PRESENTATION_VERSION);

	struct nx_presentation *presentation = calloc(1, sizeof(*presentation));
	if (presentation == NULL) {
		return NULL;
	}

	presentation->global = wl_global_create(display, &wp_presentation_interface,
		version, NULL, presentation_bind);
	if (presentation->global == NULL) {
		free(presentation);
		return NULL;
	}

	presentation->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &presentation->display_destroy);
	return presentation;
}

struct nx_presentation_feedback *output_presentation_surface_sampled(
		struct nx_surface *surface) {
	struct nx_presentation_surface *p_surface =
		presentation_surface_from_surface(surface);
	if (p_surface == NULL) {
		return NULL;
	}
	struct nx_presentation_feedback *sampled = p_surface->current.feedback;
	p_surface->current.feedback = NULL;
	return sampled;
}
