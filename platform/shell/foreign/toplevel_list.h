#ifndef PLATFORM_SHELL_FOREIGN_TOPLEVEL_LIST_H
#define PLATFORM_SHELL_FOREIGN_TOPLEVEL_LIST_H

#include <wayland-server-core.h>

struct nx_ext_foreign_toplevel_list_v1 {
	struct wl_global *global;
	struct wl_list resources; // wl_resource_get_link()
	struct wl_list toplevels; // nx_ext_foreign_toplevel_handle_v1.link

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_ext_foreign_toplevel_handle_v1;

struct nx_ext_foreign_toplevel_handle_v1_state {
	const char *title;
	const char *app_id;
};

struct nx_ext_foreign_toplevel_list_v1 *nx_ext_foreign_toplevel_list_v1_create(
	struct wl_display *display, uint32_t version);

struct nx_ext_foreign_toplevel_handle_v1 *nx_ext_foreign_toplevel_handle_v1_create(
	struct nx_ext_foreign_toplevel_list_v1 *list,
	const struct nx_ext_foreign_toplevel_handle_v1_state *state);

// Sends closed to every client.
void nx_ext_foreign_toplevel_handle_v1_destroy(
	struct nx_ext_foreign_toplevel_handle_v1 *toplevel);

void nx_ext_foreign_toplevel_handle_v1_update_state(
	struct nx_ext_foreign_toplevel_handle_v1 *toplevel,
	const struct nx_ext_foreign_toplevel_handle_v1_state *state);

#endif
