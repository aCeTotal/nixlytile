#ifndef PLATFORM_INPUT_XCURSOR_CURSOR_H
#define PLATFORM_INPUT_XCURSOR_CURSOR_H

#include "platform/input/xcursor/file.h"
#include "platform/input/xcursor/xcursor.h"

// NULL if no image loads.
struct nx_xcursor *xcursor_create_from_images(
		const struct xcursor_images *images);
void xcursor_destroy(struct nx_xcursor *cursor);

#endif
