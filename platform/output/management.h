#ifndef PLATFORM_OUTPUT_MANAGEMENT_H
#define PLATFORM_OUTPUT_MANAGEMENT_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include "platform/output/output.h"

struct nx_output_manager_v1 {
	struct wl_display *display;
	struct wl_global *global;
	struct wl_list resources; // wl_resource_get_link()

	struct wl_list heads; // nx_output_head_v1.link
	uint32_t serial;
	bool current_configuration_dirty;

	// Listeners own and answer configs.
	struct {
		struct wl_signal apply; // struct nx_output_configuration_v1
		struct wl_signal test; // struct nx_output_configuration_v1
	} events;

	struct {
		struct wl_listener display_destroy;
	};
};

struct nx_output_head_v1_state {
	struct nx_output *output;

	bool enabled;
	struct nx_output_mode *mode;
	struct {
		int32_t width, height;
		int32_t refresh;
	} custom_mode;
	int32_t x, y;
	enum wl_output_transform transform;
	float scale;
	bool adaptive_sync_enabled;
};

struct nx_output_configuration_v1 {
	struct wl_list heads; // nx_output_configuration_head_v1.link

	struct nx_output_manager_v1 *manager;
	uint32_t serial;
	bool finalized; // client requested apply or test
	bool finished; // compositor sent feedback
	struct wl_resource *resource; // NULL once destroyed
};

struct nx_output_configuration_head_v1 {
	struct nx_output_head_v1_state state;
	struct wl_list link; // nx_output_configuration_v1.heads

	struct wl_resource *resource; // NULL if finalized or disabled

	struct {
		struct wl_listener output_destroy;
	};
};

struct nx_output_manager_v1 *nx_output_manager_v1_create(
	struct wl_display *display);
// Takes ownership of the configuration.
void nx_output_manager_v1_set_configuration(
	struct nx_output_manager_v1 *manager,
	struct nx_output_configuration_v1 *config);
struct nx_output_configuration_v1 *nx_output_configuration_v1_create(void);
void nx_output_configuration_v1_destroy(
	struct nx_output_configuration_v1 *config);
void nx_output_configuration_v1_send_succeeded(
	struct nx_output_configuration_v1 *config);
void nx_output_configuration_v1_send_failed(
	struct nx_output_configuration_v1 *config);
// Prefilled from the output's state.
struct nx_output_configuration_head_v1 *
	nx_output_configuration_head_v1_create(
	struct nx_output_configuration_v1 *config, struct nx_output *output);

#endif
