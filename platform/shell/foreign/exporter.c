#include <assert.h>
#include <stdlib.h>
#include "platform/shell/foreign/foreign_v2.h"
#include "xdg-foreign-unstable-v2-protocol.h"

struct exported_toplevel {
	struct nx_xdg_foreign_exported base;
	struct wl_resource *resource;
	struct wl_listener xdg_surface_destroy;
};

static const struct zxdg_exported_v2_interface exported_impl;
static const struct zxdg_exporter_v2_interface exporter_impl;

static struct exported_toplevel *exported_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zxdg_exported_v2_interface,
		&exported_impl));
	return wl_resource_get_user_data(resource);
}

static void exported_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zxdg_exported_v2_interface exported_impl = {
	.destroy = exported_handle_destroy,
};

static void exported_destroy(struct exported_toplevel *exported) {
	nx_xdg_foreign_exported_finish(&exported->base);

	wl_list_remove(&exported->xdg_surface_destroy.link);
	wl_resource_set_user_data(exported->resource, NULL);
	free(exported);
}

static void exported_handle_resource_destroy(struct wl_resource *resource) {
	struct exported_toplevel *exported = exported_from_resource(resource);
	if (exported) {
		exported_destroy(exported);
	}
}

static void handle_xdg_surface_destroy(struct wl_listener *listener, void *data) {
	struct exported_toplevel *exported =
		wl_container_of(listener, exported, xdg_surface_destroy);
	exported_destroy(exported);
}

static struct nx_xdg_foreign_v2 *foreign_from_exporter_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &zxdg_exporter_v2_interface,
		&exporter_impl));
	return wl_resource_get_user_data(resource);
}

static void exporter_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void exporter_handle_export(struct wl_client *wl_client,
		struct wl_resource *client_resource, uint32_t id,
		struct wl_resource *surface_resource) {
	struct nx_xdg_foreign_v2 *foreign =
		foreign_from_exporter_resource(client_resource);
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);

	struct nx_xdg_toplevel *xdg_toplevel =
		shell_foreign_toplevel_get(client_resource, surface);
	if (!xdg_toplevel) {
		return;
	}

	struct exported_toplevel *exported = calloc(1, sizeof(*exported));
	if (exported == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	if (!nx_xdg_foreign_exported_init(&exported->base, foreign->registry)) {
		wl_client_post_no_memory(wl_client);
		free(exported);
		return;
	}

	exported->base.toplevel = xdg_toplevel;
	exported->resource = wl_resource_create(wl_client, &zxdg_exported_v2_interface,
		wl_resource_get_version(client_resource), id);
	if (exported->resource == NULL) {
		nx_xdg_foreign_exported_finish(&exported->base);
		wl_client_post_no_memory(wl_client);
		free(exported);
		return;
	}

	wl_resource_set_implementation(exported->resource, &exported_impl,
		exported, exported_handle_resource_destroy);

	zxdg_exported_v2_send_handle(exported->resource, exported->base.handle);

	exported->xdg_surface_destroy.notify = handle_xdg_surface_destroy;
	wl_signal_add(&xdg_toplevel->base->events.destroy, &exported->xdg_surface_destroy);
}

static const struct zxdg_exporter_v2_interface exporter_impl = {
	.destroy = exporter_handle_destroy,
	.export_toplevel = exporter_handle_export,
};

void shell_foreign_exporter_bind(struct wl_client *wl_client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *exporter_resource =
		wl_resource_create(wl_client, &zxdg_exporter_v2_interface, version, id);
	if (exporter_resource == NULL) {
		wl_client_post_no_memory(wl_client);
		return;
	}

	wl_resource_set_implementation(exporter_resource, &exporter_impl,
		data, NULL);
}
