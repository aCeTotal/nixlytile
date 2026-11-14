#include <assert.h>
#include <stdlib.h>
#include "platform/log/log.h"
#include "platform/surface/compositor.h"
#include "platform/surface/tearing_control.h"
#include "tearing-control-v1-protocol.h"

#define TEARING_CONTROL_MANAGER_VERSION 1

struct nx_tearing_control_manager_v1 {
	struct wl_global *global;
	struct wl_listener display_destroy;
};

struct nx_tearing_control_v1 {
	struct wl_resource *resource;
	enum wp_tearing_control_v1_presentation_hint current, pending;
	struct nx_addon addon;
	struct nx_surface_synced synced;
};

static const struct wp_tearing_control_manager_v1_interface tearing_impl;
static const struct wp_tearing_control_v1_interface tearing_control_impl;

static struct nx_tearing_control_manager_v1 *tearing_manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_tearing_control_manager_v1_interface,
		&tearing_impl));
	return wl_resource_get_user_data(resource);
}

static struct nx_tearing_control_v1 *tearing_surface_hint_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wp_tearing_control_v1_interface,
		&tearing_control_impl));
	return wl_resource_get_user_data(resource);
}

static void destroy_tearing_hint(struct nx_tearing_control_v1 *hint) {
	if (hint == NULL) {
		return;
	}
	wl_resource_set_user_data(hint->resource, NULL);
	nx_addon_finish(&hint->addon);
	nx_surface_synced_finish(&hint->synced);
	free(hint);
}

static void surface_addon_destroy(struct nx_addon *addon) {
	struct nx_tearing_control_v1 *hint = wl_container_of(addon, hint, addon);
	destroy_tearing_hint(hint);
}

static const struct nx_addon_interface surface_addon_impl = {
	.name = "wp_tearing_control_v1",
	.destroy = surface_addon_destroy,
};

static void resource_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void destroy_tearing_resource_impl(struct wl_resource *resource) {
	destroy_tearing_hint(tearing_surface_hint_from_resource(resource));
}

static void tearing_control_handle_set_presentation_hint(struct wl_client *client,
		struct wl_resource *resource, uint32_t hint) {
	struct nx_tearing_control_v1 *surface_hint =
		tearing_surface_hint_from_resource(resource);
	surface_hint->pending = hint;
}

static const struct wp_tearing_control_v1_interface tearing_control_impl = {
	.destroy = resource_handle_destroy,
	.set_presentation_hint = tearing_control_handle_set_presentation_hint,
};

static const struct nx_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(enum wp_tearing_control_v1_presentation_hint),
};

static void tearing_control_manager_handle_get_tearing_control(
		struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct nx_tearing_control_manager_v1 *manager = tearing_manager_from_resource(resource);
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);

	if (nx_addon_find(&surface->addons, manager, &surface_addon_impl) != NULL) {
		wl_resource_post_error(resource,
			WP_TEARING_CONTROL_MANAGER_V1_ERROR_TEARING_CONTROL_EXISTS,
			"Tearing control object already exists!");
		return;
	}

	struct nx_tearing_control_v1 *hint = calloc(1, sizeof(*hint));
	if (!hint) {
		wl_client_post_no_memory(client);
		return;
	}

	if (!nx_surface_synced_init(&hint->synced, surface,
			&surface_synced_impl, &hint->pending, &hint->current)) {
		free(hint);
		wl_client_post_no_memory(client);
		return;
	}

	hint->resource = wl_resource_create(client, &wp_tearing_control_v1_interface,
		wl_resource_get_version(resource), id);
	if (hint->resource == NULL) {
		nx_surface_synced_finish(&hint->synced);
		free(hint);
		wl_resource_post_no_memory(resource);
		return;
	}
	wl_resource_set_implementation(hint->resource, &tearing_control_impl,
		hint, destroy_tearing_resource_impl);

	nx_addon_init(&hint->addon, &surface->addons, manager, &surface_addon_impl);
}

static const struct wp_tearing_control_manager_v1_interface tearing_impl = {
	.destroy = resource_handle_destroy,
	.get_tearing_control = tearing_control_manager_handle_get_tearing_control,
};

static void tearing_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_tearing_control_manager_v1 *manager = data;

	struct wl_resource *wl_resource = wl_resource_create(wl_client,
		&wp_tearing_control_manager_v1_interface, version, id);
	if (wl_resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}
	wl_resource_set_implementation(wl_resource, &tearing_impl, manager, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_tearing_control_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_tearing_control_manager_v1 *nx_tearing_control_manager_v1_create(
		struct wl_display *display, uint32_t version) {
	assert(version <= TEARING_CONTROL_MANAGER_VERSION);

	struct nx_tearing_control_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (!manager) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	manager->global = wl_global_create(display, &wp_tearing_control_manager_v1_interface,
		version, manager, tearing_bind);
	if (manager->global == NULL) {
		wl_list_remove(&manager->display_destroy.link);
		free(manager);
		return NULL;
	}
	return manager;
}

enum wp_tearing_control_v1_presentation_hint
nx_tearing_control_manager_v1_surface_hint_from_surface(struct nx_tearing_control_manager_v1 *manager,
		struct nx_surface *surface) {
	struct nx_addon *addon =
		nx_addon_find(&surface->addons, manager, &surface_addon_impl);
	if (addon == NULL) {
		return WP_TEARING_CONTROL_V1_PRESENTATION_HINT_VSYNC;
	}

	struct nx_tearing_control_v1 *hint = wl_container_of(addon, hint, addon);
	return hint->current;
}
