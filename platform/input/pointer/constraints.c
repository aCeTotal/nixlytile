#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/input/pointer/constraint.h"
#include "platform/log/log.h"
#include "pointer-constraints-unstable-v1-protocol.h"

static const struct zwp_pointer_constraints_v1_interface pointer_constraints_impl;

static struct nx_pointer_constraints_v1 *pointer_constraints_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwp_pointer_constraints_v1_interface, &pointer_constraints_impl));
	return wl_resource_get_user_data(resource);
}

static bool surface_constrained(struct nx_pointer_constraints_v1 *pointer_constraints,
		struct nx_surface *surface, struct nx_seat *seat) {
	struct nx_pointer_constraint_v1 *constraint;
	wl_list_for_each(constraint, &pointer_constraints->constraints, link) {
		if (constraint->surface == surface && constraint->seat == seat) {
			return true;
		}
	}
	return false;
}

static void pointer_constraint_create(struct wl_client *client,
		struct wl_resource *pointer_constraints_resource, uint32_t id,
		struct wl_resource *surface_resource,
		struct wl_resource *pointer_resource,
		struct wl_resource *region_resource,
		enum zwp_pointer_constraints_v1_lifetime lifetime,
		enum nx_pointer_constraint_v1_type type) {
	struct nx_pointer_constraints_v1 *pointer_constraints =
		pointer_constraints_from_resource(pointer_constraints_resource);
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);
	struct nx_seat_client *seat_client =
		nx_seat_client_from_pointer_resource(pointer_resource);

	struct wl_resource *resource = input_pointer_constraint_resource_create(
		pointer_constraints_resource, id, type);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	if (seat_client == NULL) {
		return;
	}

	struct nx_seat *seat = seat_client->seat;
	if (surface_constrained(pointer_constraints, surface, seat)) {
		wl_resource_destroy(resource);
		wl_resource_post_error(pointer_constraints_resource,
			ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
			"a pointer constraint with a wl_pointer of the same wl_seat"
			" is already on this surface");
		return;
	}

	struct nx_pointer_constraint_v1 *constraint =
		input_pointer_constraint_create(surface, seat, region_resource);
	if (constraint == NULL) {
		wl_resource_destroy(resource);
		wl_client_post_no_memory(client);
		return;
	}
	constraint->resource = resource;
	constraint->lifetime = lifetime;
	constraint->type = type;
	wl_resource_set_user_data(resource, constraint);

	nx_log(NX_DEBUG, "new %s_pointer %p (res %p)",
		type == NX_POINTER_CONSTRAINT_V1_LOCKED ? "locked" : "confined",
		(void *)constraint, (void *)constraint->resource);

	wl_list_insert(&pointer_constraints->constraints, &constraint->link);

	wl_signal_emit_mutable(&pointer_constraints->events.new_constraint,
		constraint);
}

static void pointer_constraints_lock_pointer(struct wl_client *client,
		struct wl_resource *cons_resource, uint32_t id,
		struct wl_resource *surface, struct wl_resource *pointer,
		struct wl_resource *region, enum zwp_pointer_constraints_v1_lifetime lifetime) {
	pointer_constraint_create(client, cons_resource, id, surface, pointer,
		region, lifetime, NX_POINTER_CONSTRAINT_V1_LOCKED);
}

static void pointer_constraints_confine_pointer(struct wl_client *client,
		struct wl_resource *cons_resource, uint32_t id,
		struct wl_resource *surface, struct wl_resource *pointer,
		struct wl_resource *region,
		enum zwp_pointer_constraints_v1_lifetime lifetime) {
	pointer_constraint_create(client, cons_resource, id, surface, pointer,
		region, lifetime, NX_POINTER_CONSTRAINT_V1_CONFINED);
}

static void pointer_constraints_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_pointer_constraints_v1_interface
		pointer_constraints_impl = {
	.destroy = pointer_constraints_destroy,
	.lock_pointer = pointer_constraints_lock_pointer,
	.confine_pointer = pointer_constraints_confine_pointer,
};

static void pointer_constraints_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct nx_pointer_constraints_v1 *pointer_constraints = data;

	struct wl_resource *resource = wl_resource_create(client,
		&zwp_pointer_constraints_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &pointer_constraints_impl,
		pointer_constraints, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_pointer_constraints_v1 *pointer_constraints =
		wl_container_of(listener, pointer_constraints, display_destroy);

	assert(wl_list_empty(&pointer_constraints->events.new_constraint.listener_list));

	wl_list_remove(&pointer_constraints->display_destroy.link);
	wl_global_destroy(pointer_constraints->global);
	free(pointer_constraints);
}

struct nx_pointer_constraints_v1 *nx_pointer_constraints_v1_create(
		struct wl_display *display) {
	struct nx_pointer_constraints_v1 *pointer_constraints =
		calloc(1, sizeof(*pointer_constraints));
	if (!pointer_constraints) {
		return NULL;
	}

	pointer_constraints->global = wl_global_create(display,
		&zwp_pointer_constraints_v1_interface, 1, pointer_constraints,
		pointer_constraints_bind);
	if (!pointer_constraints->global) {
		free(pointer_constraints);
		return NULL;
	}

	wl_list_init(&pointer_constraints->constraints);

	wl_signal_init(&pointer_constraints->events.new_constraint);

	pointer_constraints->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display,
		&pointer_constraints->display_destroy);

	return pointer_constraints;
}
