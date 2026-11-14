#ifndef PLATFORM_SHELL_POPUP_POSITIONER_H
#define PLATFORM_SHELL_POPUP_POSITIONER_H

#include "platform/shell/xdg/xdg_shell.h"

struct nx_xdg_positioner {
	struct wl_resource *resource;
	struct nx_xdg_positioner_rules rules;
};

void shell_positioner_create(struct nx_xdg_client *client, uint32_t id);
struct nx_xdg_positioner *shell_positioner_from_resource(
	struct wl_resource *resource);
bool shell_positioner_is_complete(const struct nx_xdg_positioner *positioner);

// Placement before constraint adjustment.
void shell_positioner_rules_get_geometry(
	const struct nx_xdg_positioner_rules *rules, struct nx_box *box);
// Fits the box inside constraint.
void shell_positioner_rules_unconstrain_box(
	const struct nx_xdg_positioner_rules *rules,
	const struct nx_box *constraint, struct nx_box *box);

#endif
