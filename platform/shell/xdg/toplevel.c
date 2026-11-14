#include <assert.h>
#include <stdlib.h>
#include "platform/shell/xdg/role.h"

#define ALL_WM_CAPABILITIES (NX_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU | \
	NX_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE | \
	NX_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN | \
	NX_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE)

struct nx_xdg_toplevel *nx_xdg_toplevel_try_from_nx_surface(struct nx_surface *surface) {
	struct nx_xdg_surface *xdg_surface = nx_xdg_surface_try_from_nx_surface(surface);
	if (xdg_surface == NULL || xdg_surface->role != NX_XDG_SURFACE_ROLE_TOPLEVEL) {
		return NULL;
	}
	return xdg_surface->toplevel;
}

static void handle_parent_unmap(struct wl_listener *listener, void *data) {
	struct nx_xdg_toplevel *toplevel =
		wl_container_of(listener, toplevel, parent_unmap);
	if (!nx_xdg_toplevel_set_parent(toplevel, toplevel->parent->parent)) {
		assert(0 && "not reached");
	}
}

bool nx_xdg_toplevel_set_parent(struct nx_xdg_toplevel *toplevel,
		struct nx_xdg_toplevel *parent) {
	for (struct nx_xdg_toplevel *iter = parent; iter != NULL; iter = iter->parent) {
		if (iter == toplevel) {
			return false;
		}
	}

	if (toplevel->parent != NULL) {
		wl_list_remove(&toplevel->parent_unmap.link);
	}

	toplevel->parent = NULL;
	if (parent != NULL && parent->base->surface->mapped) {
		toplevel->parent = parent;
		toplevel->parent_unmap.notify = handle_parent_unmap;
		wl_signal_add(&parent->base->surface->events.unmap,
			&toplevel->parent_unmap);
	}

	wl_signal_emit_mutable(&toplevel->events.set_parent, NULL);
	return true;
}

static const struct nx_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct nx_xdg_toplevel_state),
};

void shell_toplevel_create(struct nx_xdg_surface *surface, uint32_t id) {
	if (!shell_xdg_surface_set_role(surface, NX_XDG_SURFACE_ROLE_TOPLEVEL)) {
		return;
	}

	assert(surface->toplevel == NULL);
	struct nx_xdg_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (toplevel == NULL) {
		wl_resource_post_no_memory(surface->resource);
		return;
	}
	surface->toplevel = toplevel;
	toplevel->base = surface;

	wl_signal_init(&toplevel->events.destroy);
	wl_signal_init(&toplevel->events.request_maximize);
	wl_signal_init(&toplevel->events.request_fullscreen);
	wl_signal_init(&toplevel->events.set_parent);
	wl_signal_init(&toplevel->events.set_title);

	if (!nx_surface_synced_init(&toplevel->synced, surface->surface,
			&surface_synced_impl, &toplevel->pending, &toplevel->current)) {
		goto error_toplevel;
	}

	toplevel->resource = wl_resource_create(surface->client->client,
		&xdg_toplevel_interface, wl_resource_get_version(surface->resource), id);
	if (toplevel->resource == NULL) {
		goto error_synced;
	}
	wl_resource_set_implementation(toplevel->resource,
		&shell_toplevel_impl, toplevel, NULL);

	shell_xdg_surface_set_role_object(surface, toplevel->resource);

	// The first configure carries capabilities.
	toplevel->scheduled.wm_capabilities = ALL_WM_CAPABILITIES;
	toplevel->scheduled.fields |= NX_XDG_TOPLEVEL_CONFIGURE_WM_CAPABILITIES;

	wl_signal_emit_mutable(&surface->client->shell->events.new_toplevel, toplevel);
	return;

error_synced:
	nx_surface_synced_finish(&toplevel->synced);
error_toplevel:
	free(toplevel);
	surface->toplevel = NULL;
	wl_resource_post_no_memory(surface->resource);
}

void shell_toplevel_reset(struct nx_xdg_toplevel *toplevel) {
	if (toplevel->parent) {
		wl_list_remove(&toplevel->parent_unmap.link);
		toplevel->parent = NULL;
	}
	free(toplevel->title);
	toplevel->title = NULL;
	free(toplevel->app_id);
	toplevel->app_id = NULL;

	toplevel->requested.fullscreen = false;
}

void shell_toplevel_destroy(struct nx_xdg_toplevel *toplevel) {
	nx_surface_unmap(toplevel->base->surface);
	shell_toplevel_reset(toplevel);

	wl_signal_emit_mutable(&toplevel->events.destroy, NULL);

	assert(wl_list_empty(&toplevel->events.destroy.listener_list));
	assert(wl_list_empty(&toplevel->events.request_maximize.listener_list));
	assert(wl_list_empty(&toplevel->events.request_fullscreen.listener_list));
	assert(wl_list_empty(&toplevel->events.set_parent.listener_list));
	assert(wl_list_empty(&toplevel->events.set_title.listener_list));

	nx_surface_synced_finish(&toplevel->synced);
	toplevel->base->toplevel = NULL;
	wl_resource_set_user_data(toplevel->resource, NULL);
	free(toplevel);
}

void shell_toplevel_client_commit(struct nx_xdg_toplevel *toplevel) {
	const struct nx_xdg_toplevel_state *pending = &toplevel->pending;
	// Max 0 means unbounded.
	if (pending->min_width < 0 || pending->min_height < 0 ||
			pending->max_width < 0 || pending->max_height < 0 ||
			(pending->max_width != 0 && pending->max_width < pending->min_width) ||
			(pending->max_height != 0 && pending->max_height < pending->min_height)) {
		nx_surface_reject_pending(toplevel->base->surface, toplevel->resource,
			XDG_TOPLEVEL_ERROR_INVALID_SIZE, "client provided an invalid min or max size");
	}
}

void nx_xdg_toplevel_send_close(struct nx_xdg_toplevel *toplevel) {
	xdg_toplevel_send_close(toplevel->resource);
}
