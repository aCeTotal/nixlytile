#include <stdlib.h>
#include "platform/shell/foreign/foreign_v2.h"
#include "xdg-foreign-unstable-v2-protocol.h"

#define FOREIGN_V2_VERSION 1

struct nx_xdg_toplevel *shell_foreign_toplevel_get(struct wl_resource *resource,
		struct nx_surface *surface) {
	struct nx_xdg_toplevel *toplevel = nx_xdg_toplevel_try_from_nx_surface(surface);
	if (toplevel != NULL) {
		return toplevel;
	}
	// Same code on the importer.
	wl_resource_post_error(resource, ZXDG_EXPORTER_V2_ERROR_INVALID_SURFACE,
		"surface must be an xdg_toplevel");
	return NULL;
}

static void foreign_destroy(struct nx_xdg_foreign_v2 *foreign) {
	wl_list_remove(&foreign->foreign_registry_destroy.link);
	wl_list_remove(&foreign->display_destroy.link);

	wl_global_destroy(foreign->exporter_global);
	wl_global_destroy(foreign->importer_global);
	free(foreign);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_xdg_foreign_v2 *foreign =
		wl_container_of(listener, foreign, display_destroy);
	foreign_destroy(foreign);
}

static void handle_foreign_registry_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_xdg_foreign_v2 *foreign =
		wl_container_of(listener, foreign, foreign_registry_destroy);
	foreign_destroy(foreign);
}

struct nx_xdg_foreign_v2 *nx_xdg_foreign_v2_create(
		struct wl_display *display, struct nx_xdg_foreign_registry *registry) {
	struct nx_xdg_foreign_v2 *foreign = calloc(1, sizeof(*foreign));
	if (!foreign) {
		return NULL;
	}

	foreign->exporter_global = wl_global_create(display,
		&zxdg_exporter_v2_interface, FOREIGN_V2_VERSION, foreign,
		shell_foreign_exporter_bind);
	if (!foreign->exporter_global) {
		free(foreign);
		return NULL;
	}

	foreign->importer_global = wl_global_create(display,
		&zxdg_importer_v2_interface, FOREIGN_V2_VERSION, foreign,
		shell_foreign_importer_bind);
	if (!foreign->importer_global) {
		wl_global_destroy(foreign->exporter_global);
		free(foreign);
		return NULL;
	}

	foreign->registry = registry;

	foreign->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &foreign->display_destroy);

	foreign->foreign_registry_destroy.notify = handle_foreign_registry_destroy;
	wl_signal_add(&registry->events.destroy, &foreign->foreign_registry_destroy);

	return foreign;
}
