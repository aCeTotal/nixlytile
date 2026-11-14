#ifndef PLATFORM_SHELL_IDLE_IDLE_NOTIFY_H
#define PLATFORM_SHELL_IDLE_IDLE_NOTIFY_H

#include <wayland-server-core.h>

struct nx_seat;

struct nx_idle_notifier_v1 {
	struct wl_global *global;

	struct {
		bool inhibited;
		struct wl_list notifications; // idle_notification.link

		struct wl_listener display_destroy;
	};
};

struct nx_idle_notifier_v1 *nx_idle_notifier_v1_create(struct wl_display *display);

// True while inhibitors are visible.
void nx_idle_notifier_v1_set_inhibited(struct nx_idle_notifier_v1 *notifier,
	bool inhibited);

// Call on each input event.
void nx_idle_notifier_v1_notify_activity(struct nx_idle_notifier_v1 *notifier,
	struct nx_seat *seat);

#endif
