#ifndef PLATFORM_INPUT_XCURSOR_MANAGER_H
#define PLATFORM_INPUT_XCURSOR_MANAGER_H

#include <wayland-server-core.h>
#include "platform/input/xcursor/xcursor.h"

struct nx_xcursor_manager_theme {
	float scale;
	struct nx_xcursor_theme *theme;
	struct wl_list link;
};

// One theme per output scale.
struct nx_xcursor_manager {
	char *name;
	uint32_t size; // at scale 1
	struct wl_list scaled_themes; // nx_xcursor_manager_theme.link
};

struct nx_xcursor_manager *nx_xcursor_manager_create(const char *name,
		uint32_t size);
void nx_xcursor_manager_destroy(struct nx_xcursor_manager *manager);

// Loads each scale once.
bool nx_xcursor_manager_load(struct nx_xcursor_manager *manager, float scale);
// NULL unless `scale` was loaded.
struct nx_xcursor *nx_xcursor_manager_get_xcursor(
		struct nx_xcursor_manager *manager, const char *name, float scale);

#endif
