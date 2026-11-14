#ifndef NX_XWAYLAND_SURFACE_H
#define NX_XWAYLAND_SURFACE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#include "platform/container/addon.h"

struct nx_surface;
struct nx_xwm;

// ICCCM input focus model.
enum nx_xwayland_icccm_input_model {
	NX_ICCCM_INPUT_MODEL_NONE = 0,
	NX_ICCCM_INPUT_MODEL_PASSIVE = 1,
	NX_ICCCM_INPUT_MODEL_LOCAL = 2,
	NX_ICCCM_INPUT_MODEL_GLOBAL = 3,
};

// _NET_WM_WINDOW_TYPE values the compositor queries.
enum nx_xwayland_net_wm_window_type {
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR,
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY,
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH,
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG,
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP,
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION,
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,
	NX_XWAYLAND_NET_WM_WINDOW_TYPE_DND,
};

// X11 window; surface valid when associated.
struct nx_xwayland_surface {
	xcb_window_t window_id;
	struct nx_xwm *xwm;
	uint32_t surface_id;
	uint64_t serial;

	struct wl_list link;
	struct wl_list stack_link;
	struct wl_list unpaired_link;

	struct nx_surface *surface;
	struct nx_addon surface_addon;

	int16_t x, y;
	uint16_t width, height;
	bool override_redirect;

	char *title;
	char *class;
	pid_t pid;

	struct wl_list children; // nx_xwayland_surface.parent_link
	struct nx_xwayland_surface *parent;
	struct wl_list parent_link; // nx_xwayland_surface.children

	xcb_atom_t *window_type;
	size_t window_type_len;

	xcb_atom_t *protocols;
	size_t protocols_len;

	xcb_icccm_wm_hints_t *hints;
	xcb_size_hints_t *size_hints;

	bool pinging;
	struct wl_event_source *ping_timer;

	bool withdrawn;
	// _NET_WM_STATE
	bool modal;
	bool fullscreen;
	bool maximized_vert, maximized_horz;
	bool minimized;
	bool sticky;
	bool shaded;
	bool skip_taskbar;
	bool skip_pager;
	bool above;
	bool below;
	bool demands_attention;

	struct {
		struct wl_signal destroy;
		struct wl_signal request_configure; // struct nx_xwayland_surface_configure_event
		struct wl_signal request_minimize; // struct nx_xwayland_minimize_event
		struct wl_signal request_fullscreen;
		struct wl_signal request_activate;
		struct wl_signal associate;
		struct wl_signal dissociate;
		struct wl_signal set_title;
		struct wl_signal set_hints;
		struct wl_signal set_override_redirect;
		struct wl_signal ping_timeout;
	} events;

	void *data;

	char *wm_name, *net_wm_name;
	struct wl_listener surface_commit;
	struct wl_listener surface_map;
	struct wl_listener surface_unmap;
};

struct nx_xwayland_surface_configure_event {
	struct nx_xwayland_surface *surface;
	int16_t x, y;
	uint16_t width, height;
	uint16_t mask; // xcb_config_window_t
};

struct nx_xwayland_minimize_event {
	struct nx_xwayland_surface *surface;
	bool minimize;
};

// NULL for non-X11 surfaces.
struct nx_xwayland_surface *nx_xwayland_surface_try_from_nx_surface(
	struct nx_surface *surface);

void nx_xwayland_surface_activate(struct nx_xwayland_surface *surface,
	bool activated);

// NULL sibling: top or bottom.
void nx_xwayland_surface_restack(struct nx_xwayland_surface *surface,
	struct nx_xwayland_surface *sibling, enum xcb_stack_mode_t mode);

void nx_xwayland_surface_configure(struct nx_xwayland_surface *surface,
	int16_t x, int16_t y, uint16_t width, uint16_t height);

void nx_xwayland_surface_close(struct nx_xwayland_surface *surface);

void nx_xwayland_surface_set_minimized(struct nx_xwayland_surface *surface,
	bool minimized);

void nx_xwayland_surface_set_maximized(struct nx_xwayland_surface *surface,
	bool maximized_horz, bool maximized_vert);

void nx_xwayland_surface_set_fullscreen(struct nx_xwayland_surface *surface,
	bool fullscreen);

void nx_xwayland_surface_ping(struct nx_xwayland_surface *surface);

bool nx_xwayland_surface_has_window_type(
	const struct nx_xwayland_surface *xsurface,
	enum nx_xwayland_net_wm_window_type window_type);

// Override-redirect focus heuristic.
bool nx_xwayland_surface_override_redirect_wants_focus(
	const struct nx_xwayland_surface *xsurface);

enum nx_xwayland_icccm_input_model nx_xwayland_surface_icccm_input_model(
	const struct nx_xwayland_surface *xsurface);

#endif
