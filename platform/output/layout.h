#ifndef PLATFORM_OUTPUT_LAYOUT_H
#define PLATFORM_OUTPUT_LAYOUT_H

#include <stdbool.h>
#include <wayland-util.h>
#include "platform/output/output.h"
#include "platform/container/addon.h"

struct nx_box;

// Outputs arranged in layout space.
struct nx_output_layout {
	struct wl_list outputs;
	struct wl_display *display;

	struct {
		struct wl_signal add; // struct nx_output_layout_output
		struct wl_signal change;
		struct wl_signal destroy;
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_output_layout_output {
	struct nx_output_layout *layout;

	struct nx_output *output;

	int x, y;
	struct wl_list link;

	bool auto_configured;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct nx_addon addon;

		struct wl_listener commit;
	};
};

enum nx_direction {
	NX_DIRECTION_UP = 1 << 0,
	NX_DIRECTION_DOWN = 1 << 1,
	NX_DIRECTION_LEFT = 1 << 2,
	NX_DIRECTION_RIGHT = 1 << 3,
};

struct nx_output_layout *nx_output_layout_create(struct wl_display *display);
// NULL for outputs not added.
struct nx_output_layout_output *nx_output_layout_get(
	struct nx_output_layout *layout, struct nx_output *reference);
struct nx_output *nx_output_layout_output_at(
	struct nx_output_layout *layout, double lx, double ly);
// Re-adding switches to manual placement.
struct nx_output_layout_output *nx_output_layout_add(struct nx_output_layout *layout,
	struct nx_output *output, int lx, int ly);
struct nx_output_layout_output *nx_output_layout_add_auto(struct nx_output_layout *layout,
	struct nx_output *output);
void nx_output_layout_remove(struct nx_output_layout *layout,
	struct nx_output *output);
// Converts layout to output-local coordinates.
void nx_output_layout_output_coords(struct nx_output_layout *layout,
	struct nx_output *reference, double *lx, double *ly);
bool nx_output_layout_contains_point(struct nx_output_layout *layout,
	struct nx_output *reference, int lx, int ly);
// NULL reference: whole layout.
void nx_output_layout_closest_point(struct nx_output_layout *layout,
	struct nx_output *reference, double lx, double ly,
	double *dest_lx, double *dest_ly);
// NULL reference: whole layout.
void nx_output_layout_get_box(struct nx_output_layout *layout,
	struct nx_output *reference, struct nx_box *dest_box);
struct nx_output *nx_output_layout_adjacent_output(
	struct nx_output_layout *layout, enum nx_direction direction,
	struct nx_output *reference, double ref_lx, double ref_ly);
struct nx_output *nx_output_layout_farthest_output(
	struct nx_output_layout *layout, enum nx_direction direction,
	struct nx_output *reference, double ref_lx, double ref_ly);

#endif
