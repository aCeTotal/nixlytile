#include <stdlib.h>
#include "platform/container/mem.h"
#include "platform/render/renderer.h"
#include "platform/surface/color_management.h"

enum nx_color_transfer_function
nx_color_manager_v1_transfer_function_to_nx(enum wp_color_manager_v1_transfer_function tf) {
	switch (tf) {
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4:
		return NX_COLOR_TRANSFER_FUNCTION_SRGB;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
		return NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
		return NX_COLOR_TRANSFER_FUNCTION_EXT_LINEAR;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
		return NX_COLOR_TRANSFER_FUNCTION_GAMMA22;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
		return NX_COLOR_TRANSFER_FUNCTION_BT1886;
	default:
		abort();
	}
}

enum wp_color_manager_v1_transfer_function
nx_color_manager_v1_transfer_function_from_nx(enum nx_color_transfer_function tf) {
	switch (tf) {
	case NX_COLOR_TRANSFER_FUNCTION_SRGB:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4;
	case NX_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
	case NX_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR;
	case NX_COLOR_TRANSFER_FUNCTION_GAMMA22:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
	case NX_COLOR_TRANSFER_FUNCTION_BT1886:
		return WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886;
	}
	abort();
}

enum nx_color_named_primaries
nx_color_manager_v1_primaries_to_nx(enum wp_color_manager_v1_primaries primaries) {
	switch (primaries) {
	case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB:
		return NX_COLOR_NAMED_PRIMARIES_SRGB;
	case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020:
		return NX_COLOR_NAMED_PRIMARIES_BT2020;
	default:
		abort();
	}
}

enum wp_color_manager_v1_primaries
nx_color_manager_v1_primaries_from_nx(enum nx_color_named_primaries primaries) {
	switch (primaries) {
	case NX_COLOR_NAMED_PRIMARIES_SRGB:
		return WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
	case NX_COLOR_NAMED_PRIMARIES_BT2020:
		return WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
	}
	abort();
}

enum wp_color_manager_v1_transfer_function *
nx_color_manager_v1_transfer_function_list_from_renderer(struct nx_renderer *renderer, size_t *len) {
	*len = 0;
	if (!renderer->features.input_color_transform) {
		return NULL;
	}

	static const enum wp_color_manager_v1_transfer_function list[] = {
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR,
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886,
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4,
	};

	enum wp_color_manager_v1_transfer_function *out = NULL;
	if (!memdup(&out, list, sizeof(list))) {
		return NULL;
	}

	*len = sizeof(list) / sizeof(list[0]);
	return out;
}

enum wp_color_manager_v1_primaries *
nx_color_manager_v1_primaries_list_from_renderer(struct nx_renderer *renderer, size_t *len) {
	*len = 0;
	if (!renderer->features.input_color_transform) {
		return NULL;
	}

	static const enum wp_color_manager_v1_primaries list[] = {
		WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
		WP_COLOR_MANAGER_V1_PRIMARIES_BT2020,
	};

	enum wp_color_manager_v1_primaries *out = NULL;
	if (!memdup(&out, list, sizeof(list))) {
		return NULL;
	}

	*len = sizeof(list) / sizeof(list[0]);
	return out;
}
