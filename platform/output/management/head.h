#ifndef PLATFORM_OUTPUT_MANAGEMENT_HEAD_H
#define PLATFORM_OUTPUT_MANAGEMENT_HEAD_H

#include "platform/output/management.h"

enum output_head_state_field {
	HEAD_STATE_ENABLED = 1 << 0,
	HEAD_STATE_MODE = 1 << 1,
	HEAD_STATE_POSITION = 1 << 2,
	HEAD_STATE_TRANSFORM = 1 << 3,
	HEAD_STATE_SCALE = 1 << 4,
	HEAD_STATE_ADAPTIVE_SYNC = 1 << 5,
	HEAD_STATE_ALL = HEAD_STATE_ENABLED | HEAD_STATE_MODE |
		HEAD_STATE_POSITION | HEAD_STATE_TRANSFORM | HEAD_STATE_SCALE |
		HEAD_STATE_ADAPTIVE_SYNC,
};

struct nx_output_head_v1 {
	struct nx_output_head_v1_state state;
	struct nx_output_manager_v1 *manager;
	struct wl_list link; // nx_output_manager_v1.heads

	struct wl_list resources; // wl_resource_get_link()
	struct wl_list mode_resources; // wl_resource_get_link()

	struct wl_listener output_destroy;
};

// NULL once the head is inert.
struct nx_output_head_v1 *output_head_from_resource(struct wl_resource *resource);
struct nx_output_head_v1 *output_head_create(struct nx_output_manager_v1 *manager,
	struct nx_output *output);
void output_head_destroy(struct nx_output_head_v1 *head);
void output_head_send_state(struct nx_output_head_v1 *head,
	struct wl_resource *head_resource, uint32_t state);
void output_manager_send_head(struct nx_output_head_v1 *head,
	struct wl_resource *manager_resource);

// NULL for custom or inert modes.
struct nx_output_mode *output_mode_from_resource(struct wl_resource *resource);
void output_head_send_mode(struct nx_output_head_v1 *head,
	struct wl_resource *head_resource, struct nx_output_mode *mode);
// Advertises new and custom modes.
void output_head_update_modes(struct nx_output_head_v1 *head,
	const struct nx_output_head_v1_state *next);
void output_head_send_current_mode(struct nx_output_head_v1 *head,
	struct wl_resource *head_resource);

#endif
