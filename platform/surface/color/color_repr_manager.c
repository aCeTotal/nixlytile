#include <assert.h>
#include <stdlib.h>
#include "color-representation-v1-protocol.h"
#include "platform/render/renderer.h"
#include "platform/surface/color/representation_manager.h"

#define WP_COLOR_REPRESENTATION_VERSION 1

static const enum wp_color_representation_surface_v1_coefficients coefficients[] = {
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_FCC,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_SMPTE240,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020_CL,
	WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_ICTCP,
};

#define COEFFICIENTS_LEN (sizeof(coefficients) / sizeof(coefficients[0]))

static const enum wp_color_representation_surface_v1_range ranges[] = {
	WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED,
	WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL,
};

#define RANGES_LEN (sizeof(ranges) / sizeof(ranges[0]))

struct coeffs_and_range {
	enum wp_color_representation_surface_v1_coefficients coeffs;
	enum wp_color_representation_surface_v1_range range;
};

struct nx_color_representation_manager_v1 {
	struct wl_global *global;

	// Identity full, then renderer encodings
	struct coeffs_and_range supported[1 + COEFFICIENTS_LEN * RANGES_LEN];
	size_t supported_len;

	struct wl_listener display_destroy;
};

enum nx_color_encoding nx_color_representation_v1_color_encoding_to_nx(
		enum wp_color_representation_surface_v1_coefficients wp_val) {
	switch (wp_val) {
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY:
		return NX_COLOR_ENCODING_IDENTITY;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT709:
		return NX_COLOR_ENCODING_BT709;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_FCC:
		return NX_COLOR_ENCODING_FCC;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT601:
		return NX_COLOR_ENCODING_BT601;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_SMPTE240:
		return NX_COLOR_ENCODING_SMPTE240;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020:
		return NX_COLOR_ENCODING_BT2020;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_BT2020_CL:
		return NX_COLOR_ENCODING_BT2020_CL;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_ICTCP:
		return NX_COLOR_ENCODING_ICTCP;
	}
	abort();
}

enum nx_color_range nx_color_representation_v1_color_range_to_nx(
		enum wp_color_representation_surface_v1_range wp_val) {
	switch (wp_val) {
	case WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_LIMITED:
		return NX_COLOR_RANGE_LIMITED;
	case WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL:
		return NX_COLOR_RANGE_FULL;
	}
	abort();
}

bool color_repr_manager_supports(const struct nx_color_representation_manager_v1 *manager,
		uint32_t coeffs, uint32_t range) {
	for (size_t i = 0; i < manager->supported_len; i++) {
		const struct coeffs_and_range *supported = &manager->supported[i];
		if (supported->coeffs == coeffs && supported->range == range) {
			return true;
		}
	}
	return false;
}

static void add_supported(struct nx_color_representation_manager_v1 *manager,
		enum wp_color_representation_surface_v1_coefficients coeffs,
		enum wp_color_representation_surface_v1_range range) {
	manager->supported[manager->supported_len++] = (struct coeffs_and_range){
		.coeffs = coeffs,
		.range = range,
	};
}

static void add_renderer_encodings(struct nx_color_representation_manager_v1 *manager,
		const struct nx_renderer *renderer) {
	for (size_t i = 0; i < COEFFICIENTS_LEN; i++) {
		enum nx_color_encoding enc =
			nx_color_representation_v1_color_encoding_to_nx(coefficients[i]);
		if (!(renderer->color_encodings & enc)) {
			continue;
		}
		for (size_t j = 0; j < RANGES_LEN; j++) {
			add_supported(manager, coefficients[i], ranges[j]);
		}
	}
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_color_representation_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_surface = color_repr_manager_get_surface,
};

struct nx_color_representation_manager_v1 *color_repr_manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_color_representation_manager_v1_interface, &manager_impl));
	return wl_resource_get_user_data(resource);
}

static void send_supported(struct nx_color_representation_manager_v1 *manager,
		struct wl_resource *resource) {
	wp_color_representation_manager_v1_send_supported_alpha_mode(resource,
		COLOR_REPR_ALPHA_MODE);

	for (size_t i = 0; i < manager->supported_len; i++) {
		wp_color_representation_manager_v1_send_supported_coefficients_and_ranges(
			resource, manager->supported[i].coeffs, manager->supported[i].range);
	}

	wp_color_representation_manager_v1_send_done(resource);
}

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_color_representation_manager_v1 *manager = data;

	struct wl_resource *resource = wl_resource_create(client,
		&wp_color_representation_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, manager, NULL);

	send_supported(manager, resource);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_color_representation_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	wl_list_remove(&manager->display_destroy.link);
	wl_global_destroy(manager->global);
	free(manager);
}

struct nx_color_representation_manager_v1 *nx_color_representation_manager_v1_create_with_renderer(
		struct wl_display *display, uint32_t version, struct nx_renderer *renderer) {
	assert(version <= WP_COLOR_REPRESENTATION_VERSION);

	struct nx_color_representation_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}

	add_supported(manager, WP_COLOR_REPRESENTATION_SURFACE_V1_COEFFICIENTS_IDENTITY,
		WP_COLOR_REPRESENTATION_SURFACE_V1_RANGE_FULL);
	add_renderer_encodings(manager, renderer);

	manager->global = wl_global_create(display,
		&wp_color_representation_manager_v1_interface,
		version, manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);

	return manager;
}
