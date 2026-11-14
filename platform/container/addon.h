#ifndef PLATFORM_CONTAINER_ADDON_H
#define PLATFORM_CONTAINER_ADDON_H

#include <wayland-server-core.h>

struct nx_addon_set {
	struct {
		struct wl_list addons;
	};
};

struct nx_addon;

struct nx_addon_interface {
	const char *name;
	// Must call nx_addon_finish().
	void (*destroy)(struct nx_addon *addon);
};

// Extra state keyed by owner and interface.
struct nx_addon {
	const struct nx_addon_interface *impl;

	struct {
		const void *owner;
		struct wl_list link;
	};
};

void nx_addon_set_init(struct nx_addon_set *set);
// Destroys every remaining addon.
void nx_addon_set_finish(struct nx_addon_set *set);

void nx_addon_init(struct nx_addon *addon, struct nx_addon_set *set,
	const void *owner, const struct nx_addon_interface *impl);
void nx_addon_finish(struct nx_addon *addon);

struct nx_addon *nx_addon_find(struct nx_addon_set *set, const void *owner,
	const struct nx_addon_interface *impl);

#endif
