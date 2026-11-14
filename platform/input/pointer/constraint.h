#ifndef PLATFORM_INPUT_POINTER_CONSTRAINT_H
#define PLATFORM_INPUT_POINTER_CONSTRAINT_H

#include "platform/input/pointer/constraints.h"

// Locked or confined pointer resource.
struct wl_resource *input_pointer_constraint_resource_create(
	struct wl_resource *manager_resource, uint32_t id,
	enum nx_pointer_constraint_v1_type type);
// NULL on allocation failure.
struct nx_pointer_constraint_v1 *input_pointer_constraint_create(
	struct nx_surface *surface, struct nx_seat *seat,
	struct wl_resource *region_resource);

#endif
