#ifndef PLATFORM_CLIPBOARD_MIME_TYPES_H
#define PLATFORM_CLIPBOARD_MIME_TYPES_H

#include <wayland-server-core.h>

// Ignores duplicates, posts OOM.
void mime_types_add(struct wl_array *mime_types, const char *mime_type,
	struct wl_resource *resource);
void mime_types_release(struct wl_array *mime_types);

#endif
