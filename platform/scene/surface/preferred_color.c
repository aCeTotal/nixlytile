#include <assert.h>
#include <stdlib.h>
#include "platform/output/output.h"
#include "platform/surface/color_management.h"
#include "platform/surface/compositor.h"
#include "platform/scene/surface/scene_surface.h"

static const struct nx_output_image_description hdr_description = {
	.transfer_function = NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
	.primaries = NX_COLOR_NAMED_PRIMARIES_BT2020,
};

static int get_tf_preference(enum nx_color_transfer_function tf) {
	switch (tf) {
	case NX_COLOR_TRANSFER_FUNCTION_GAMMA22:
		return 0;
	case NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		return 1;
	case NX_COLOR_TRANSFER_FUNCTION_BT1886:
	case NX_COLOR_TRANSFER_FUNCTION_SRGB:
	case NX_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		return -1;
	}
	abort(); // unreachable
}

static int get_primaries_preference(enum nx_color_named_primaries primaries) {
	switch (primaries) {
	case NX_COLOR_NAMED_PRIMARIES_SRGB:
		return 0;
	case NX_COLOR_NAMED_PRIMARIES_BT2020:
		return 1;
	}
	abort(); // unreachable
}

static bool color_manager_supports_pq_bt2020(const struct nx_color_manager_v1 *manager) {
	bool has_pq = false, has_bt2020 = false;
	for (size_t i = 0; i < manager->transfer_functions_len; i++) {
		has_pq |= manager->transfer_functions[i] ==
			WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
	}
	for (size_t i = 0; i < manager->primaries_len; i++) {
		has_bt2020 |= manager->primaries[i] == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
	}
	return has_pq && has_bt2020;
}

// Unset HDR outputs still hint PQ.
static const struct nx_output_image_description *output_image_description(
		const struct nx_output *output, const struct nx_color_manager_v1 *manager) {
	if (output->image_description != NULL) {
		return output->image_description;
	}

	// Only if clients can create PQ.
	if (!color_manager_supports_pq_bt2020(manager) ||
			!(output->supported_transfer_functions & NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ) ||
			!(output->supported_primaries & NX_COLOR_NAMED_PRIMARIES_BT2020)) {
		return NULL;
	}

	return &hdr_description;
}

void scene_preferred_image_description(struct nx_surface *surface,
		struct nx_color_manager_v1 *manager,
		struct nx_image_description_v1_data *out) {
	struct nx_output_image_description preferred = {
		.transfer_function = NX_COLOR_TRANSFER_FUNCTION_GAMMA22,
		.primaries = NX_COLOR_NAMED_PRIMARIES_SRGB,
	};

	struct nx_surface_output *surface_output;
	wl_list_for_each(surface_output, &surface->current_outputs, link) {
		const struct nx_output_image_description *img_desc =
			output_image_description(surface_output->output, manager);
		if (img_desc == NULL) {
			continue;
		}
		if (get_tf_preference(preferred.transfer_function) < get_tf_preference(img_desc->transfer_function)) {
			preferred.transfer_function = img_desc->transfer_function;
		}
		if (get_primaries_preference(preferred.primaries) < get_primaries_preference(img_desc->primaries)) {
			preferred.primaries = img_desc->primaries;
		}
	}

	*out = (struct nx_image_description_v1_data){
		.tf_named = nx_color_manager_v1_transfer_function_from_nx(preferred.transfer_function),
		.primaries_named = nx_color_manager_v1_primaries_from_nx(preferred.primaries),
	};
}

static void scene_handle_color_manager_v1_destroy(struct wl_listener *listener, void *data) {
	struct nx_scene *scene = wl_container_of(listener, scene, color_manager_v1_destroy);
	wl_list_remove(&scene->color_manager_v1_destroy.link);
	wl_list_init(&scene->color_manager_v1_destroy.link);
	scene->color_manager_v1 = NULL;
}

void nx_scene_set_color_manager_v1(struct nx_scene *scene, struct nx_color_manager_v1 *manager) {
	assert(scene->color_manager_v1 == NULL);
	scene->color_manager_v1 = manager;

	scene->color_manager_v1_destroy.notify = scene_handle_color_manager_v1_destroy;
	wl_signal_add(&manager->events.destroy, &scene->color_manager_v1_destroy);
}
