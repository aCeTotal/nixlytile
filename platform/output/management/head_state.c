#include "platform/log/log.h"
#include "platform/output/management/configuration.h"
#include "platform/output/management/head.h"
#include "wlr-output-management-unstable-v1-protocol.h"

static void send_adaptive_sync(struct nx_output_head_v1 *head,
		struct wl_resource *head_resource) {
	if (wl_resource_get_version(head_resource) <
			ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_SINCE_VERSION) {
		return;
	}
	zwlr_output_head_v1_send_adaptive_sync(head_resource,
		head->state.adaptive_sync_enabled ?
		ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_ENABLED :
		ZWLR_OUTPUT_HEAD_V1_ADAPTIVE_SYNC_STATE_DISABLED);
}

void output_head_send_state(struct nx_output_head_v1 *head,
		struct wl_resource *head_resource, uint32_t state) {
	// Enabling resends all state.
	if (state & HEAD_STATE_ENABLED) {
		zwlr_output_head_v1_send_enabled(head_resource, head->state.enabled);
		state = HEAD_STATE_ALL;
	}
	if (!head->state.enabled) {
		return;
	}

	if (state & HEAD_STATE_MODE) {
		output_head_send_current_mode(head, head_resource);
	}
	if (state & HEAD_STATE_POSITION) {
		zwlr_output_head_v1_send_position(head_resource,
			head->state.x, head->state.y);
	}
	if (state & HEAD_STATE_TRANSFORM) {
		zwlr_output_head_v1_send_transform(head_resource,
			head->state.transform);
	}
	if (state & HEAD_STATE_SCALE) {
		zwlr_output_head_v1_send_scale(head_resource,
			wl_fixed_from_double(head->state.scale));
	}
	if (state & HEAD_STATE_ADAPTIVE_SYNC) {
		send_adaptive_sync(head, head_resource);
	}
}

static uint32_t head_state_diff(const struct nx_output_head_v1_state *current,
		const struct nx_output_head_v1_state *next) {
	uint32_t state = 0;
	if (current->enabled != next->enabled) {
		state |= HEAD_STATE_ENABLED;
	}
	if (current->mode != next->mode ||
			current->custom_mode.width != next->custom_mode.width ||
			current->custom_mode.height != next->custom_mode.height ||
			current->custom_mode.refresh != next->custom_mode.refresh) {
		state |= HEAD_STATE_MODE;
	}
	if (current->x != next->x || current->y != next->y) {
		state |= HEAD_STATE_POSITION;
	}
	if (current->transform != next->transform) {
		state |= HEAD_STATE_TRANSFORM;
	}
	if (current->scale != next->scale) {
		state |= HEAD_STATE_SCALE;
	}
	if (current->adaptive_sync_enabled != next->adaptive_sync_enabled) {
		state |= HEAD_STATE_ADAPTIVE_SYNC;
	}
	return state;
}

// Returns whether the head changed.
static bool update_head(struct nx_output_head_v1 *head,
		const struct nx_output_head_v1_state *next) {
	uint32_t state = head_state_diff(&head->state, next);
	output_head_update_modes(head, next);
	if (state == 0) {
		return false;
	}

	head->state = *next;
	struct wl_resource *resource;
	wl_resource_for_each(resource, &head->resources) {
		output_head_send_state(head, resource, state);
	}
	return true;
}

static struct nx_output_configuration_head_v1 *configuration_get_head(
		struct nx_output_configuration_v1 *config, struct nx_output *output) {
	struct nx_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		if (head->state.output == output) {
			return head;
		}
	}
	return NULL;
}

// Heads left in the config are new.
static bool add_new_heads(struct nx_output_manager_v1 *manager,
		struct nx_output_configuration_v1 *config) {
	bool added = false;
	struct nx_output_configuration_head_v1 *config_head, *tmp;
	wl_list_for_each_safe(config_head, tmp, &config->heads, link) {
		struct nx_output_head_v1 *head =
			output_head_create(manager, config_head->state.output);
		if (head == NULL) {
			nx_log_errno(NX_ERROR, "Allocation failed");
			continue;
		}
		head->state = config_head->state;

		struct wl_resource *manager_resource;
		wl_resource_for_each(manager_resource, &manager->resources) {
			output_manager_send_head(head, manager_resource);
		}
		added = true;
	}
	return added;
}

void nx_output_manager_v1_set_configuration(
		struct nx_output_manager_v1 *manager,
		struct nx_output_configuration_v1 *config) {
	bool changed = manager->current_configuration_dirty;

	struct nx_output_head_v1 *head, *head_tmp;
	wl_list_for_each_safe(head, head_tmp, &manager->heads, link) {
		struct nx_output_configuration_head_v1 *updated =
			configuration_get_head(config, head->state.output);
		if (updated == NULL) {
			output_head_destroy(head);
			changed = true;
			continue;
		}
		changed |= update_head(head, &updated->state);
		output_config_head_destroy(updated);
	}

	changed |= add_new_heads(manager, config);
	nx_output_configuration_v1_destroy(config);
	if (!changed) {
		return;
	}

	manager->serial = wl_display_next_serial(manager->display);
	struct wl_resource *manager_resource;
	wl_resource_for_each(manager_resource, &manager->resources) {
		zwlr_output_manager_v1_send_done(manager_resource, manager->serial);
	}
	manager->current_configuration_dirty = false;
}
