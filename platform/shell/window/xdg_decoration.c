#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include "platform/shell/window/xdg_decoration.h"
#include "platform/log/log.h"
#include "xdg-decoration-unstable-v1-protocol.h"

#define DECORATION_MANAGER_VERSION 1

struct decoration_configure {
	struct wl_list link; // nx_xdg_toplevel_decoration_v1.configure_list
	struct nx_xdg_surface_configure *surface_configure;
	enum nx_xdg_toplevel_decoration_v1_mode mode;
};

static const struct zxdg_toplevel_decoration_v1_interface
	toplevel_decoration_impl;

static struct nx_xdg_toplevel_decoration_v1 *toplevel_decoration_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zxdg_toplevel_decoration_v1_interface, &toplevel_decoration_impl));
	return wl_resource_get_user_data(resource);
}

static void toplevel_decoration_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void request_mode(struct wl_resource *resource) {
	struct nx_xdg_toplevel_decoration_v1 *decoration =
		toplevel_decoration_from_resource(resource);
	wl_signal_emit_mutable(&decoration->events.request_mode, decoration);
}

static void toplevel_decoration_handle_set_mode(struct wl_client *client,
		struct wl_resource *resource,
		enum zxdg_toplevel_decoration_v1_mode mode) {
	request_mode(resource);
}

static void toplevel_decoration_handle_unset_mode(struct wl_client *client,
		struct wl_resource *resource) {
	request_mode(resource);
}

static const struct zxdg_toplevel_decoration_v1_interface
		toplevel_decoration_impl = {
	.destroy = toplevel_decoration_handle_destroy,
	.set_mode = toplevel_decoration_handle_set_mode,
	.unset_mode = toplevel_decoration_handle_unset_mode,
};

uint32_t nx_xdg_toplevel_decoration_v1_set_mode(
		struct nx_xdg_toplevel_decoration_v1 *decoration,
		enum nx_xdg_toplevel_decoration_v1_mode mode) {
	assert(mode != NX_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE);
	decoration->scheduled_mode = mode;
	return nx_xdg_surface_schedule_configure(decoration->toplevel->base);
}

static void toplevel_decoration_handle_resource_destroy(
		struct wl_resource *resource) {
	struct nx_xdg_toplevel_decoration_v1 *decoration =
		toplevel_decoration_from_resource(resource);
	wl_signal_emit_mutable(&decoration->events.destroy, decoration);

	assert(wl_list_empty(&decoration->events.destroy.listener_list));
	assert(wl_list_empty(&decoration->events.request_mode.listener_list));

	wl_list_remove(&decoration->toplevel_destroy.link);
	wl_list_remove(&decoration->surface_configure.link);
	wl_list_remove(&decoration->surface_ack_configure.link);
	struct decoration_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &decoration->configure_list, link) {
		free(configure);
	}
	wl_list_remove(&decoration->link);
	free(decoration);
}

static void toplevel_decoration_handle_toplevel_destroy(
		struct wl_listener *listener, void *data) {
	struct nx_xdg_toplevel_decoration_v1 *decoration =
		wl_container_of(listener, decoration, toplevel_destroy);

	wl_resource_post_error(decoration->resource,
		ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ORPHANED,
		"xdg_toplevel destroyed before xdg_toplevel_decoration");

	wl_resource_destroy(decoration->resource);
}

static void toplevel_decoration_handle_surface_configure(
		struct wl_listener *listener, void *data) {
	struct nx_xdg_toplevel_decoration_v1 *decoration =
		wl_container_of(listener, decoration, surface_configure);
	if (decoration->acked_mode == decoration->scheduled_mode) {
		return;
	}

	struct decoration_configure *configure = calloc(1, sizeof(*configure));
	if (configure == NULL) {
		return;
	}
	configure->surface_configure = data;
	configure->mode = decoration->scheduled_mode;
	wl_list_insert(decoration->configure_list.prev, &configure->link);

	zxdg_toplevel_decoration_v1_send_configure(decoration->resource,
		configure->mode);
}

static struct decoration_configure *find_configure(
		struct nx_xdg_toplevel_decoration_v1 *decoration,
		const struct nx_xdg_surface_configure *surface_configure) {
	struct decoration_configure *configure;
	wl_list_for_each(configure, &decoration->configure_list, link) {
		if (configure->surface_configure == surface_configure) {
			return configure;
		}
	}
	return NULL;
}

static void toplevel_decoration_handle_surface_ack_configure(
		struct wl_listener *listener, void *data) {
	struct nx_xdg_toplevel_decoration_v1 *decoration =
		wl_container_of(listener, decoration, surface_ack_configure);
	struct decoration_configure *acked = find_configure(decoration, data);
	if (acked == NULL) {
		return;
	}

	// Older configures are implicitly acked.
	struct decoration_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &decoration->configure_list, link) {
		if (configure == acked) {
			break;
		}
		wl_list_remove(&configure->link);
		free(configure);
	}

	decoration->acked_mode = acked->mode;

	wl_list_remove(&acked->link);
	free(acked);
}

static const struct zxdg_decoration_manager_v1_interface decoration_manager_impl;

static struct nx_xdg_decoration_manager_v1 *
		decoration_manager_from_resource(struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zxdg_decoration_manager_v1_interface,
		&decoration_manager_impl));
	return wl_resource_get_user_data(resource);
}

static void decoration_manager_handle_destroy(
		struct wl_client *client, struct wl_resource *manager_resource) {
	wl_resource_destroy(manager_resource);
}

static bool has_decoration(struct nx_xdg_decoration_manager_v1 *manager,
		struct nx_xdg_toplevel *toplevel) {
	struct nx_xdg_toplevel_decoration_v1 *existing;
	wl_list_for_each(existing, &manager->decorations, link) {
		if (existing->toplevel == toplevel) {
			return true;
		}
	}
	return false;
}

static void decoration_listen(struct nx_xdg_toplevel_decoration_v1 *decoration) {
	struct nx_xdg_toplevel *toplevel = decoration->toplevel;
	wl_signal_add(&toplevel->events.destroy, &decoration->toplevel_destroy);
	decoration->toplevel_destroy.notify = toplevel_decoration_handle_toplevel_destroy;
	wl_signal_add(&toplevel->base->events.configure, &decoration->surface_configure);
	decoration->surface_configure.notify = toplevel_decoration_handle_surface_configure;
	wl_signal_add(&toplevel->base->events.ack_configure, &decoration->surface_ack_configure);
	decoration->surface_ack_configure.notify = toplevel_decoration_handle_surface_ack_configure;
}

static void decoration_manager_handle_get_toplevel_decoration(
		struct wl_client *client, struct wl_resource *manager_resource,
		uint32_t id, struct wl_resource *toplevel_resource) {
	struct nx_xdg_decoration_manager_v1 *manager =
		decoration_manager_from_resource(manager_resource);
	struct nx_xdg_toplevel *toplevel =
		nx_xdg_toplevel_from_resource(toplevel_resource);

	// Tolerate SDL3 attaching buffers early.
	if (nx_surface_has_buffer(toplevel->base->surface)) {
		nx_log(NX_DEBUG, "xdg_toplevel_decoration created on a toplevel "
			"that already has a buffer; tolerating (spec violation by client)");
	}

	if (has_decoration(manager, toplevel)) {
		wl_resource_post_error(manager_resource,
			ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
			"xdg_toplevel already has a decoration object");
		return;
	}

	struct nx_xdg_toplevel_decoration_v1 *decoration = calloc(1, sizeof(*decoration));
	if (decoration == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	decoration->toplevel = toplevel;

	decoration->resource = wl_resource_create(client,
		&zxdg_toplevel_decoration_v1_interface,
		wl_resource_get_version(manager_resource), id);
	if (decoration->resource == NULL) {
		free(decoration);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(decoration->resource,
		&toplevel_decoration_impl, decoration,
		toplevel_decoration_handle_resource_destroy);

	nx_log(NX_DEBUG, "new xdg_toplevel_decoration %p (res %p)", (void *)decoration,
		(void *)decoration->resource);

	wl_list_init(&decoration->configure_list);
	wl_signal_init(&decoration->events.destroy);
	wl_signal_init(&decoration->events.request_mode);
	decoration_listen(decoration);

	wl_list_insert(&manager->decorations, &decoration->link);

	wl_signal_emit_mutable(&manager->events.new_toplevel_decoration, decoration);
}

static const struct zxdg_decoration_manager_v1_interface
		decoration_manager_impl = {
	.destroy = decoration_manager_handle_destroy,
	.get_toplevel_decoration = decoration_manager_handle_get_toplevel_decoration,
};

static void decoration_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&zxdg_decoration_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &decoration_manager_impl,
		data, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_xdg_decoration_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);

	assert(wl_list_empty(&manager->events.new_toplevel_decoration.listener_list));

	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_xdg_decoration_manager_v1 *
		nx_xdg_decoration_manager_v1_create(struct wl_display *display) {
	struct nx_xdg_decoration_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	manager->global = wl_global_create(display,
		&zxdg_decoration_manager_v1_interface, DECORATION_MANAGER_VERSION,
		manager, decoration_manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}
	wl_list_init(&manager->decorations);

	wl_signal_init(&manager->events.new_toplevel_decoration);

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
