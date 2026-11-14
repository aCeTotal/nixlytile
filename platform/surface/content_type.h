#ifndef PLATFORM_SURFACE_CONTENT_TYPE_H
#define PLATFORM_SURFACE_CONTENT_TYPE_H

#include <wayland-server-core.h>
#include <wayland-protocols/content-type-v1-enum.h>

struct nx_surface;
struct nx_content_type_manager_v1;

struct nx_content_type_manager_v1 *nx_content_type_manager_v1_create(
	struct wl_display *display, uint32_t version);
enum wp_content_type_v1_type nx_surface_get_content_type_v1(
	struct nx_content_type_manager_v1 *manager, struct nx_surface *surface);

#endif
