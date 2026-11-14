#ifndef PLATFORM_SHELL_XDG_XDG_SHELL_H
#define PLATFORM_SHELL_XDG_XDG_SHELL_H

#include <wayland-server-core.h>
#include <wayland-protocols/xdg-shell-enum.h>
#include "platform/surface/compositor.h"
#include "platform/input/seat/seat.h"
#include "platform/geometry/box.h"

struct nx_xdg_shell {
	struct wl_global *global;
	struct wl_list popup_grabs; // nx_xdg_popup_grab.link
	uint32_t ping_timeout;

	struct {
		struct wl_signal new_toplevel; // struct nx_xdg_toplevel
		struct wl_signal new_popup; // struct nx_xdg_popup
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_xdg_client {
	struct nx_xdg_shell *shell;
	struct wl_resource *resource;
	struct wl_client *client;
	struct wl_list surfaces; // nx_xdg_surface.link

	uint32_t ping_serial;
	struct wl_event_source *ping_timer;
};

struct nx_xdg_positioner_rules {
	struct nx_box anchor_rect;
	enum xdg_positioner_anchor anchor;
	enum xdg_positioner_gravity gravity;
	enum xdg_positioner_constraint_adjustment constraint_adjustment;

	struct {
		int32_t width, height;
	} size;

	struct {
		int32_t x, y;
	} offset;
};

struct nx_xdg_popup_state {
	struct nx_box geometry; // relative to parent geometry
};

enum nx_xdg_popup_configure_field {
	NX_XDG_POPUP_CONFIGURE_REPOSITION_TOKEN = 1 << 0,
};

struct nx_xdg_popup_configure {
	uint32_t fields; // enum nx_xdg_popup_configure_field
	struct nx_box geometry;
	struct nx_xdg_positioner_rules rules;
	uint32_t reposition_token;
};

struct nx_xdg_popup {
	struct nx_xdg_surface *base;
	struct wl_list link; // parent's popups list

	struct wl_resource *resource;
	struct nx_surface *parent;
	struct nx_seat *seat;

	struct nx_xdg_popup_configure scheduled;

	struct nx_xdg_popup_state current, pending;

	struct wl_list grab_link; // nx_xdg_popup_grab.popups

	struct {
		struct nx_surface_synced synced;
	};
};

// One grab per seat.
struct nx_xdg_popup_grab {
	struct wl_client *client;
	struct nx_seat_pointer_grab pointer_grab;
	struct nx_seat_keyboard_grab keyboard_grab;
	struct nx_seat *seat;
	struct wl_list popups; // nx_xdg_popup.grab_link
	struct wl_list link; // nx_xdg_shell.popup_grabs

	struct {
		struct wl_listener seat_destroy;
	};
};

enum nx_xdg_surface_role {
	NX_XDG_SURFACE_ROLE_NONE,
	NX_XDG_SURFACE_ROLE_TOPLEVEL,
	NX_XDG_SURFACE_ROLE_POPUP,
};

struct nx_xdg_toplevel_state {
	int32_t width, height;
	int32_t max_width, max_height;
	int32_t min_width, min_height;
};

enum nx_xdg_toplevel_wm_capabilities {
	NX_XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU = 1 << 0,
	NX_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE = 1 << 1,
	NX_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN = 1 << 2,
	NX_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE = 1 << 3,
};

enum nx_xdg_toplevel_configure_field {
	NX_XDG_TOPLEVEL_CONFIGURE_BOUNDS = 1 << 0,
	NX_XDG_TOPLEVEL_CONFIGURE_WM_CAPABILITIES = 1 << 1,
};

struct nx_xdg_toplevel_configure {
	uint32_t fields; // enum nx_xdg_toplevel_configure_field

	bool maximized, fullscreen, resizing, activated, suspended;
	uint32_t tiled; // enum nx_edges
	int32_t width, height;

	struct {
		int32_t width, height;
	} bounds;
	uint32_t wm_capabilities; // enum nx_xdg_toplevel_wm_capabilities
};

struct nx_xdg_toplevel {
	struct wl_resource *resource;
	struct nx_xdg_surface *base;

	struct nx_xdg_toplevel *parent;

	struct nx_xdg_toplevel_state current, pending;

	// Sent with the next configure.
	struct nx_xdg_toplevel_configure scheduled;

	struct {
		bool fullscreen;
	} requested;

	char *title;
	char *app_id;

	struct {
		struct wl_signal destroy;
		// Listeners must send a configure.
		struct wl_signal request_maximize;
		struct wl_signal request_fullscreen;
		struct wl_signal set_parent;
		struct wl_signal set_title;
	} events;

	struct {
		struct nx_surface_synced synced;

		struct wl_listener parent_unmap;
	};
};

struct nx_xdg_surface_configure {
	struct wl_list link; // nx_xdg_surface.configure_list
	uint32_t serial;

	union {
		struct nx_xdg_toplevel_configure *toplevel_configure;
		struct nx_xdg_popup_configure *popup_configure;
	};
};

enum nx_xdg_surface_state_field {
	NX_XDG_SURFACE_STATE_WINDOW_GEOMETRY = 1 << 0,
};

struct nx_xdg_surface_state {
	uint32_t committed; // enum nx_xdg_surface_state_field

	struct nx_box geometry;

	uint32_t configure_serial;
};

struct nx_xdg_surface {
	struct nx_xdg_client *client;
	struct wl_resource *resource;
	struct nx_surface *surface;
	struct wl_list link; // nx_xdg_client.surfaces

	// Outlives the role object.
	enum nx_xdg_surface_role role;
	struct wl_resource *role_resource;

	// NULL for an inert role.
	union {
		struct nx_xdg_toplevel *toplevel;
		struct nx_xdg_popup *popup;
	};

	struct wl_list popups; // nx_xdg_popup.link

	bool configured;
	struct wl_event_source *configure_idle;
	uint32_t scheduled_serial;
	struct wl_list configure_list;

	struct nx_xdg_surface_state current, pending;

	// Ready to receive configure events.
	bool initialized;
	bool initial_commit;

	struct nx_box geometry;

	struct {
		struct wl_signal destroy;
		struct wl_signal ping_timeout;
		struct wl_signal configure; // struct nx_xdg_surface_configure
		struct wl_signal ack_configure; // struct nx_xdg_surface_configure
	} events;

	void *data;

	struct {
		struct nx_surface_synced synced;

		struct wl_listener role_resource_destroy;
	};
};

struct nx_xdg_shell *nx_xdg_shell_create(struct wl_display *display,
	uint32_t version);

// NULL for inert resources.
struct nx_xdg_popup *nx_xdg_popup_from_resource(
	struct wl_resource *resource);
struct nx_xdg_toplevel *nx_xdg_toplevel_from_resource(
	struct wl_resource *resource);

// Emits ping_timeout unless ponged.
void nx_xdg_surface_ping(struct nx_xdg_surface *surface);

// Setters return the configure serial.
uint32_t nx_xdg_toplevel_set_size(struct nx_xdg_toplevel *toplevel,
	int32_t width, int32_t height);
uint32_t nx_xdg_toplevel_set_activated(struct nx_xdg_toplevel *toplevel,
	bool activated);
uint32_t nx_xdg_toplevel_set_maximized(struct nx_xdg_toplevel *toplevel,
	bool maximized);
uint32_t nx_xdg_toplevel_set_fullscreen(struct nx_xdg_toplevel *toplevel,
	bool fullscreen);
uint32_t nx_xdg_toplevel_set_resizing(struct nx_xdg_toplevel *toplevel,
	bool resizing);
uint32_t nx_xdg_toplevel_set_tiled(struct nx_xdg_toplevel *toplevel,
	uint32_t tiled_edges);
uint32_t nx_xdg_toplevel_set_bounds(struct nx_xdg_toplevel *toplevel,
	int32_t width, int32_t height);
uint32_t nx_xdg_toplevel_set_wm_capabilities(struct nx_xdg_toplevel *toplevel,
	uint32_t caps);
uint32_t nx_xdg_toplevel_set_suspended(struct nx_xdg_toplevel *toplevel,
	bool suspended);

void nx_xdg_toplevel_send_close(struct nx_xdg_toplevel *toplevel);

// Fails on parent loops.
bool nx_xdg_toplevel_set_parent(struct nx_xdg_toplevel *toplevel,
	struct nx_xdg_toplevel *parent);

// Dismisses the popup.
void nx_xdg_popup_destroy(struct nx_xdg_popup *popup);

// Box in root toplevel coordinates.
void nx_xdg_popup_unconstrain_from_box(struct nx_xdg_popup *popup,
	const struct nx_box *toplevel_space_box);

struct nx_xdg_surface *nx_xdg_surface_try_from_nx_surface(struct nx_surface *surface);
struct nx_xdg_toplevel *nx_xdg_toplevel_try_from_nx_surface(struct nx_surface *surface);
struct nx_xdg_popup *nx_xdg_popup_try_from_nx_surface(struct nx_surface *surface);

uint32_t nx_xdg_surface_schedule_configure(struct nx_xdg_surface *surface);

#endif
