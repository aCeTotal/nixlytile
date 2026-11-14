#include <math.h>
#include "platform/geometry/transform.h"
#include "platform/surface/compositor/surface.h"
#include "platform/surface/subsurface/subsurface.h"

struct nx_surface *nx_surface_get_root_surface(struct nx_surface *surface) {
	struct nx_subsurface *subsurface;
	while ((subsurface = subsurface_try_from_surface(surface))) {
		surface = subsurface->parent;
	}
	return surface;
}

bool nx_surface_point_accepts_input(struct nx_surface *surface,
		double sx, double sy) {
	return sx >= 0 && sx < surface->current.width &&
		sy >= 0 && sy < surface->current.height &&
		pixman_region32_contains_point(&surface->input_region,
			(int)floor(sx), (int)floor(sy), NULL);
}

struct surface_iteration {
	nx_surface_iterator_func_t iterator;
	void *user_data;
};

static void iterate_surface(struct nx_surface *surface, int x, int y,
	const struct surface_iteration *iteration);

static void iterate_subsurfaces(struct wl_list *subsurfaces, int x, int y,
		const struct surface_iteration *iteration) {
	struct nx_subsurface *subsurface;
	wl_list_for_each(subsurface, subsurfaces, current.link) {
		if (!subsurface->surface->mapped) {
			continue;
		}
		iterate_surface(subsurface->surface, x + subsurface->current.x,
			y + subsurface->current.y, iteration);
	}
}

static void iterate_surface(struct nx_surface *surface, int x, int y,
		const struct surface_iteration *iteration) {
	iterate_subsurfaces(&surface->current.subsurfaces_below, x, y, iteration);
	iteration->iterator(surface, x, y, iteration->user_data);
	iterate_subsurfaces(&surface->current.subsurfaces_above, x, y, iteration);
}

void nx_surface_for_each_surface(struct nx_surface *surface,
		nx_surface_iterator_func_t iterator, void *user_data) {
	const struct surface_iteration iteration = {
		.iterator = iterator,
		.user_data = user_data,
	};
	iterate_surface(surface, 0, 0, &iteration);
}

struct bound_acc {
	int32_t min_x, min_y;
	int32_t max_x, max_y;
};

static int min(int a, int b) {
	return a < b ? a : b;
}

static int max(int a, int b) {
	return a > b ? a : b;
}

static void handle_bounding_box_surface(struct nx_surface *surface,
		int x, int y, void *data) {
	struct bound_acc *acc = data;

	acc->min_x = min(x, acc->min_x);
	acc->min_y = min(y, acc->min_y);

	acc->max_x = max(x + surface->current.width, acc->max_x);
	acc->max_y = max(y + surface->current.height, acc->max_y);
}

void nx_surface_get_extents(struct nx_surface *surface, struct nx_box *box) {
	struct bound_acc acc = {
		.min_x = 0,
		.min_y = 0,
		.max_x = surface->current.width,
		.max_y = surface->current.height,
	};

	nx_surface_for_each_surface(surface, handle_bounding_box_surface, &acc);

	box->x = acc.min_x;
	box->y = acc.min_y;
	box->width = acc.max_x - acc.min_x;
	box->height = acc.max_y - acc.min_y;
}

void nx_surface_get_buffer_source_box(struct nx_surface *surface,
		struct nx_fbox *box) {
	const struct nx_surface_state *current = &surface->current;
	box->x = box->y = 0;
	box->width = current->buffer_width;
	box->height = current->buffer_height;

	if (!current->viewport.has_src) {
		return;
	}

	box->x = current->viewport.src.x * current->scale;
	box->y = current->viewport.src.y * current->scale;
	box->width = current->viewport.src.width * current->scale;
	box->height = current->viewport.src.height * current->scale;

	int width, height;
	surface_state_transformed_buffer_size(current, &width, &height);
	nx_fbox_transform(box, box, nx_output_transform_invert(current->transform),
		width, height);
}
