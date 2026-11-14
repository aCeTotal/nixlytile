#include <assert.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/input/pointer/constraint.h"
#include "platform/log/log.h"
#include "platform/surface/compositor.h"
#include "pointer-constraints-unstable-v1-protocol.h"

static const struct zwp_locked_pointer_v1_interface locked_pointer_impl;
static const struct zwp_confined_pointer_v1_interface confined_pointer_impl;

// NULL for inert constraints.
static struct nx_pointer_constraint_v1 *pointer_constraint_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
			&zwp_confined_pointer_v1_interface, &confined_pointer_impl) ||
		wl_resource_instance_of(resource,
			&zwp_locked_pointer_v1_interface, &locked_pointer_impl));
	return wl_resource_get_user_data(resource);
}

static void resource_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void pointer_constraint_destroy(struct nx_pointer_constraint_v1 *constraint) {
	// Oneshot deactivation re-enters here.
	if (constraint == NULL || constraint->destroying) {
		return;
	}
	constraint->destroying = true;

	nx_log(NX_DEBUG, "destroying constraint %p", (void *)constraint);

	wl_signal_emit_mutable(&constraint->events.destroy, constraint);

	assert(wl_list_empty(&constraint->events.destroy.listener_list));

	wl_resource_set_user_data(constraint->resource, NULL);
	nx_surface_synced_finish(&constraint->synced);
	wl_list_remove(&constraint->link);
	wl_list_remove(&constraint->surface_destroy.link);
	wl_list_remove(&constraint->seat_destroy.link);
	pixman_region32_fini(&constraint->region);
	free(constraint);
}

static void pointer_constraint_destroy_resource(struct wl_resource *resource) {
	pointer_constraint_destroy(pointer_constraint_from_resource(resource));
}

static void pointer_constraint_handle_set_region(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *region_resource) {
	struct nx_pointer_constraint_v1 *constraint =
		pointer_constraint_from_resource(resource);
	if (constraint == NULL) {
		return;
	}

	pixman_region32_clear(&constraint->pending.region);
	if (region_resource) {
		pixman_region32_copy(&constraint->pending.region,
			nx_region_from_resource(region_resource));
	}

	constraint->pending.committed |= NX_POINTER_CONSTRAINT_V1_STATE_REGION;
}

static void pointer_constraint_set_cursor_position_hint(struct wl_client *client,
		struct wl_resource *resource, wl_fixed_t x, wl_fixed_t y) {
	struct nx_pointer_constraint_v1 *constraint =
		pointer_constraint_from_resource(resource);
	if (constraint == NULL) {
		return;
	}

	constraint->pending.cursor_hint.enabled = true;
	constraint->pending.cursor_hint.x = wl_fixed_to_double(x);
	constraint->pending.cursor_hint.y = wl_fixed_to_double(y);
	constraint->pending.committed |= NX_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT;
}

static void update_region(struct nx_pointer_constraint_v1 *constraint) {
	pixman_region32_t region;
	pixman_region32_init(&region);

	if (pixman_region32_empty(&constraint->current.region)) {
		pixman_region32_copy(&region, &constraint->surface->input_region);
	} else {
		pixman_region32_intersect(&region,
			&constraint->surface->input_region, &constraint->current.region);
	}

	if (pixman_region32_equal(&region, &constraint->region)) {
		pixman_region32_fini(&region);
		return;
	}

	pixman_region32_fini(&constraint->region);
	constraint->region = region;
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct nx_pointer_constraint_v1 *constraint =
		wl_container_of(listener, constraint, surface_destroy);
	pointer_constraint_destroy(constraint);
}

static void handle_seat_destroy(struct wl_listener *listener, void *data) {
	struct nx_pointer_constraint_v1 *constraint =
		wl_container_of(listener, constraint, seat_destroy);
	pointer_constraint_destroy(constraint);
}

static const struct zwp_confined_pointer_v1_interface confined_pointer_impl = {
	.destroy = resource_destroy,
	.set_region = pointer_constraint_handle_set_region,
};

static const struct zwp_locked_pointer_v1_interface locked_pointer_impl = {
	.destroy = resource_destroy,
	.set_region = pointer_constraint_handle_set_region,
	.set_cursor_position_hint = pointer_constraint_set_cursor_position_hint,
};

static void surface_synced_init_state(void *_state) {
	struct nx_pointer_constraint_v1_state *state = _state;
	pixman_region32_init(&state->region);
}

static void surface_synced_finish_state(void *_state) {
	struct nx_pointer_constraint_v1_state *state = _state;
	pixman_region32_fini(&state->region);
}

static void surface_synced_move_state(void *_dst, void *_src) {
	struct nx_pointer_constraint_v1_state *dst = _dst, *src = _src;

	if (src->committed & NX_POINTER_CONSTRAINT_V1_STATE_REGION) {
		pixman_region32_copy(&dst->region, &src->region);
	}
	if (src->committed & NX_POINTER_CONSTRAINT_V1_STATE_CURSOR_HINT) {
		dst->cursor_hint = src->cursor_hint;
	}

	dst->committed = src->committed;
	src->committed = 0;
}

static void surface_synced_commit(struct nx_surface_synced *synced) {
	struct nx_pointer_constraint_v1 *constraint =
		wl_container_of(synced, constraint, synced);
	update_region(constraint);
}

static const struct nx_surface_synced_impl surface_synced_impl = {
	.state_size = sizeof(struct nx_pointer_constraint_v1_state),
	.init_state = surface_synced_init_state,
	.finish_state = surface_synced_finish_state,
	.move_state = surface_synced_move_state,
	.commit = surface_synced_commit,
};

struct wl_resource *input_pointer_constraint_resource_create(
		struct wl_resource *manager_resource, uint32_t id,
		enum nx_pointer_constraint_v1_type type) {
	const struct wl_interface *interface = &zwp_confined_pointer_v1_interface;
	const void *impl = &confined_pointer_impl;
	if (type == NX_POINTER_CONSTRAINT_V1_LOCKED) {
		interface = &zwp_locked_pointer_v1_interface;
		impl = &locked_pointer_impl;
	}

	struct wl_resource *resource = wl_resource_create(
		wl_resource_get_client(manager_resource), interface,
		wl_resource_get_version(manager_resource), id);
	if (resource == NULL) {
		return NULL;
	}
	wl_resource_set_implementation(resource, impl, NULL,
		pointer_constraint_destroy_resource);
	return resource;
}

struct nx_pointer_constraint_v1 *input_pointer_constraint_create(
		struct nx_surface *surface, struct nx_seat *seat,
		struct wl_resource *region_resource) {
	struct nx_pointer_constraint_v1 *constraint = calloc(1, sizeof(*constraint));
	if (constraint == NULL) {
		return NULL;
	}

	if (!nx_surface_synced_init(&constraint->synced, surface,
			&surface_synced_impl, &constraint->pending, &constraint->current)) {
		free(constraint);
		return NULL;
	}

	constraint->surface = surface;
	constraint->seat = seat;

	wl_signal_init(&constraint->events.destroy);

	pixman_region32_init(&constraint->region);

	if (region_resource) {
		pixman_region32_copy(&constraint->current.region,
			nx_region_from_resource(region_resource));
		update_region(constraint);
	}

	constraint->surface_destroy.notify = handle_surface_destroy;
	wl_signal_add(&surface->events.destroy, &constraint->surface_destroy);

	constraint->seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&seat->events.destroy, &constraint->seat_destroy);
	return constraint;
}

void nx_pointer_constraint_v1_send_activated(
		struct nx_pointer_constraint_v1 *constraint) {
	nx_log(NX_DEBUG, "constrained %p", (void *)constraint);
	if (constraint->type == NX_POINTER_CONSTRAINT_V1_LOCKED) {
		zwp_locked_pointer_v1_send_locked(constraint->resource);
		return;
	}
	zwp_confined_pointer_v1_send_confined(constraint->resource);
}

void nx_pointer_constraint_v1_send_deactivated(
		struct nx_pointer_constraint_v1 *constraint) {
	nx_log(NX_DEBUG, "unconstrained %p", (void *)constraint);
	if (constraint->type == NX_POINTER_CONSTRAINT_V1_LOCKED) {
		zwp_locked_pointer_v1_send_unlocked(constraint->resource);
	} else {
		zwp_confined_pointer_v1_send_unconfined(constraint->resource);
	}

	if (constraint->lifetime == ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT) {
		pointer_constraint_destroy(constraint);
	}
}
