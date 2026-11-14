#ifndef PLATFORM_OUTPUT_PRESENTATION_H
#define PLATFORM_OUTPUT_PRESENTATION_H

#include <wayland-server-core.h>

struct nx_backend;
struct nx_output;
struct nx_presentation;
struct nx_surface;

struct nx_presentation *nx_presentation_create(struct wl_display *display,
	struct nx_backend *backend, uint32_t version);
// Call before committing the output.
void nx_presentation_surface_textured_on_output(struct nx_surface *surface,
	struct nx_output *output);
void nx_presentation_surface_scanned_out_on_output(struct nx_surface *surface,
	struct nx_output *output);

#endif
