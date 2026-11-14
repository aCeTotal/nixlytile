#ifndef PLATFORM_SURFACE_COLOR_MANAGEMENT_H
#define PLATFORM_SURFACE_COLOR_MANAGEMENT_H

#include <wayland-server-core.h>
#include <wayland-protocols/color-management-v1-enum.h>
#include "platform/render/color.h"

struct nx_renderer;
struct nx_surface;

struct nx_image_description_v1_data {
	uint32_t tf_named; // enum wp_color_manager_v1_transfer_function, zero if unset
	uint32_t primaries_named; // enum wp_color_manager_v1_primaries, zero if unset

	bool has_primaries;
	struct nx_color_primaries primaries;

	bool has_luminances;
	struct nx_color_luminances luminances; // cd/m²

	bool has_mastering_display_primaries;
	struct nx_color_primaries mastering_display_primaries;

	bool has_mastering_luminance;
	struct {
		float min, max; // cd/m²
	} mastering_luminance;

	uint32_t max_cll, max_fall; // cd/m², zero if unset
};

struct nx_color_manager_v1_features {
	bool icc_v2_v4;
	bool parametric;
	bool set_primaries;
	bool set_luminances;
	bool set_mastering_display_primaries;
};

struct nx_color_manager_v1_options {
	struct nx_color_manager_v1_features features;

	const enum wp_color_manager_v1_render_intent *render_intents;
	size_t render_intents_len;

	const enum wp_color_manager_v1_transfer_function *transfer_functions;
	size_t transfer_functions_len;

	const enum wp_color_manager_v1_primaries *primaries;
	size_t primaries_len;
};

struct nx_color_manager_v1 {
	struct wl_global *global;

	struct {
		struct wl_signal destroy;
	} events;

	struct {
		struct nx_color_manager_v1_features features;

		enum wp_color_manager_v1_render_intent *render_intents;
		size_t render_intents_len;

		enum wp_color_manager_v1_transfer_function *transfer_functions;
		size_t transfer_functions_len;

		enum wp_color_manager_v1_primaries *primaries;
		size_t primaries_len;

		struct wl_list surface_feedbacks; // color_feedback.link

		uint64_t last_image_desc_identity;

		struct wl_listener display_destroy;
	};
};

struct nx_color_manager_v1 *nx_color_manager_v1_create(struct wl_display *display,
	uint32_t version, const struct nx_color_manager_v1_options *options);

const struct nx_image_description_v1_data *
nx_surface_get_image_description_v1_data(struct nx_surface *surface);

void nx_color_manager_v1_set_surface_preferred_image_description(
	struct nx_color_manager_v1 *manager, struct nx_surface *surface,
	const struct nx_image_description_v1_data *data);

// Aborts without a platform match
enum nx_color_transfer_function
nx_color_manager_v1_transfer_function_to_nx(enum wp_color_manager_v1_transfer_function tf);
enum wp_color_manager_v1_transfer_function
nx_color_manager_v1_transfer_function_from_nx(enum nx_color_transfer_function tf);

// Aborts without a platform match
enum nx_color_named_primaries
nx_color_manager_v1_primaries_to_nx(enum wp_color_manager_v1_primaries primaries);
enum wp_color_manager_v1_primaries
nx_color_manager_v1_primaries_from_nx(enum nx_color_named_primaries primaries);

// Caller frees the returned list
enum wp_color_manager_v1_transfer_function *
nx_color_manager_v1_transfer_function_list_from_renderer(struct nx_renderer *renderer, size_t *len);
enum wp_color_manager_v1_primaries *
nx_color_manager_v1_primaries_list_from_renderer(struct nx_renderer *renderer, size_t *len);

#endif
