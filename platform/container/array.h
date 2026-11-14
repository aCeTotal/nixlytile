#ifndef PLATFORM_CONTAINER_ARRAY_H
#define PLATFORM_CONTAINER_ARRAY_H

#include <stdlib.h>
#include <stdbool.h>
#include <wayland-util.h>

void array_remove_at(struct wl_array *arr, size_t offset, size_t size);
// Fits the allocation to size.
bool array_realloc(struct wl_array *arr, size_t size);

#endif
