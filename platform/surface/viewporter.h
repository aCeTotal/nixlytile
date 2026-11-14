#ifndef PLATFORM_SURFACE_VIEWPORTER_H
#define PLATFORM_SURFACE_VIEWPORTER_H

#include <wayland-server-core.h>

struct nx_viewporter;

// Renderers crop to source box
struct nx_viewporter *nx_viewporter_create(struct wl_display *display);

#endif
