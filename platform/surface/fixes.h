#ifndef PLATFORM_SURFACE_FIXES_H
#define PLATFORM_SURFACE_FIXES_H

#include <wayland-server-core.h>

struct nx_fixes;

struct nx_fixes *nx_fixes_create(struct wl_display *display, uint32_t version);

#endif
