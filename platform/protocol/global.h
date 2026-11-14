#ifndef PLATFORM_PROTOCOL_GLOBAL_H
#define PLATFORM_PROTOCOL_GLOBAL_H

#include <wayland-server-core.h>

// Delays destruction past racing binds.
void nx_global_destroy_safe(struct wl_global *global);

#endif
