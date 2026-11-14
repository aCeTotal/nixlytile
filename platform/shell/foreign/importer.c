#include <assert.h>
#include <stdlib.h>
#include "platform/shell/foreign/foreign_v2.h"
#include "xdg-foreign-unstable-v2-protocol.h"

struct imported_toplevel {
	struct nx_xdg_foreign_exported *exported;
	struct wl_resource *resource;
	struct wl_list children; // imported_child.link
	struct wl_listener exported_destroy;
};

// Toplevel parented to an import.
struct imported_child {
	struct nx_xdg_toplevel *toplevel;
	struct wl_list link; // imported_toplevel.children
	struct wl_listener xdg_toplevel_destroy;
	struct wl_listener xdg_toplevel_set_parent;
};

static const struct zxdg_imported_v2_interface imported_impl;
static const struct zxdg_importer_v2_interface importer_impl;

static struct imported_toplevel *imported_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zxdg_imported_v2_interface,
		&imported_impl));
	return wl_resource_get_user_data(resource);
}

static void imported_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void child_destroy(struct imported_child *child) {
	wl_list_remove(&child->xdg_toplevel_set_parent.link);
	wl_list_remove(&child->xdg_toplevel_destroy.link);
	wl_list_remove(&child->link);
	free(child);
}

static void handle_child_xdg_toplevel_destroy(struct wl_listener *listener,
		void *data) {
	struct imported_child *child =
		wl_container_of(listener, child, xdg_toplevel_destroy);
	child_destroy(child);
}

static void handle_child_xdg_toplevel_set_parent(struct wl_listener *listener,
		void *data) {
	struct imported_child *child =
		wl_container_of(listener, child, xdg_toplevel_set_parent);
	child_destroy(child);
}

static bool has_child(struct imported_toplevel *imported,
		struct nx_xdg_toplevel *toplevel) {
	struct imported_child *child;
	wl_list_for_each(child, &imported->children, link) {
		if (child->toplevel == toplevel) {
			return true;
		}
	}
	return false;
}

static void imported_handle_set_parent_of(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *child_resource) {
	struct imported_toplevel *imported = imported_from_resource(resource);
	if (imported == NULL) {
		return;
	}

	struct nx_xdg_toplevel *toplevel = imported->exported->toplevel;
	struct nx_surface *child_surface = nx_surface_from_resource(child_resource);
	struct nx_xdg_toplevel *child_toplevel =
		shell_foreign_toplevel_get(resource, child_surface);
	if (!child_toplevel) {
		return;
	}

	if (!toplevel->base->surface->mapped) {
		nx_xdg_toplevel_set_parent(child_toplevel, NULL);
		return;
	}

	if (has_child(imported, child_toplevel)) {
		return;
	}

	struct imported_child *child = calloc(1, sizeof(*child));
	if (child == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	child->toplevel = child_toplevel;
	child->xdg_toplevel_destroy.notify = handle_child_xdg_toplevel_destroy;
	child->xdg_toplevel_set_parent.notify = handle_child_xdg_toplevel_set_parent;

	if (!nx_xdg_toplevel_set_parent(child_toplevel, toplevel)) {
		wl_resource_post_error(toplevel->resource,
			XDG_TOPLEVEL_ERROR_INVALID_PARENT,
			"a toplevel cannot be a parent of itself or its ancestor");
		free(child);
		return;
	}

	wl_signal_add(&child_toplevel->events.destroy, &child->xdg_toplevel_destroy);
	wl_signal_add(&child_toplevel->events.set_parent, &child->xdg_toplevel_set_parent);

	wl_list_insert(&imported->children, &child->link);
}

static const struct zxdg_imported_v2_interface imported_impl = {
	.destroy = imported_handle_destroy,
	.set_parent_of = imported_handle_set_parent_of,
};

static void imported_destroy(struct imported_toplevel *imported) {
	// Unparenting destroys each child.
	struct imported_child *child, *child_tmp;
	wl_list_for_each_safe(child, child_tmp, &imported->children, link) {
		nx_xdg_toplevel_set_parent(child->toplevel, NULL);
	}

	wl_list_remove(&imported->exported_destroy.link);
	wl_resource_set_user_data(imported->resource, NULL);
	free(imported);
}

static void imported_handle_resource_destroy(struct wl_resource *resource) {
	struct imported_toplevel *imported = imported_from_resource(resource);
	if (imported) {
		imported_destroy(imported);
	}
}

static void imported_handle_exported_destroy(struct wl_listener *listener,
		void *data) {
	struct imported_toplevel *imported =
		wl_container_of(listener, imported, exported_destroy);
	zxdg_imported_v2_send_destroyed(imported->resource);
	imported_destroy(imported);
}

static struct nx_xdg_foreign_v2 *foreign_from_importer_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zxdg_importer_v2_interface,
		&importer_impl));
	return wl_resource_get_user_data(resource);
}

static void importer_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void importer_handle_import(struct wl_client *wl_client,
		struct wl_resource *client_resource, uint32_t id, const char *handle) {
	struct nx_xdg_foreign_v2 *foreign =
		foreign_from_importer_resource(client_resource);

	struct imported_toplevel *imported = calloc(1, sizeof(*imported));
	if (imported == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	imported->exported = nx_xdg_foreign_registry_find_by_handle(
		foreign->registry, handle);
	imported->resource = wl_resource_create(wl_client, &zxdg_imported_v2_interface,
		wl_resource_get_version(client_resource), id);
	if (imported->resource == NULL) {
		wl_client_post_no_memory(wl_client);
		free(imported);
		return;
	}

	wl_resource_set_implementation(imported->resource, &imported_impl,
		imported, imported_handle_resource_destroy);

	if (imported->exported == NULL) {
		wl_resource_set_user_data(imported->resource, NULL);
		zxdg_imported_v2_send_destroyed(imported->resource);
		free(imported);
		return;
	}

	wl_list_init(&imported->children);

	imported->exported_destroy.notify = imported_handle_exported_destroy;
	wl_signal_add(&imported->exported->events.destroy, &imported->exported_destroy);
}

static const struct zxdg_importer_v2_interface importer_impl = {
	.destroy = importer_handle_destroy,
	.import_toplevel = importer_handle_import,
};

void shell_foreign_importer_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *importer_resource =
		wl_resource_create(wl_client, &zxdg_importer_v2_interface, version, id);
	if (importer_resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_resource_set_implementation(importer_resource, &importer_impl,
		data, NULL);
}
