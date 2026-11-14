#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include "platform/output/output.h"
#include "platform/shell/lock/lock_surface.h"
#include "ext-session-lock-v1-protocol.h"

struct lock_configure {
	struct wl_list link; // nx_session_lock_surface_v1.configure_list
	uint32_t serial;
	uint32_t width, height;
};

static const struct ext_session_lock_surface_v1_interface lock_surface_implementation;
static const struct nx_surface_role lock_surface_role;

void shell_lock_surface_destroy(struct nx_session_lock_surface_v1 *lock_surface) {
	nx_surface_unmap(lock_surface->surface);

	wl_signal_emit_mutable(&lock_surface->events.destroy, NULL);

	wl_list_remove(&lock_surface->link);

	struct lock_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &lock_surface->configure_list, link) {
		wl_list_remove(&configure->link);
		free(configure);
	}

	assert(wl_list_empty(&lock_surface->events.destroy.listener_list));

	wl_list_remove(&lock_surface->output_destroy.link);
	wl_resource_set_user_data(lock_surface->resource, NULL);
	free(lock_surface);
}

static struct nx_session_lock_surface_v1 *lock_surface_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_session_lock_surface_v1_interface, &lock_surface_implementation));
	return wl_resource_get_user_data(resource);
}

static struct nx_session_lock_surface_v1 *lock_surface_try_from_nx_surface(
		struct nx_surface *surface) {
	if (surface->role != &lock_surface_role || surface->role_resource == NULL) {
		return NULL;
	}
	return lock_surface_from_resource(surface->role_resource);
}

uint32_t nx_session_lock_surface_v1_configure(
		struct nx_session_lock_surface_v1 *lock_surface,
		uint32_t width, uint32_t height) {
	struct lock_configure *configure = calloc(1, sizeof(*configure));
	if (configure == NULL) {
		wl_resource_post_no_memory(lock_surface->resource);
		return lock_surface->acked.configure_serial;
	}

	struct wl_display *display =
		wl_client_get_display(wl_resource_get_client(lock_surface->resource));

	configure->width = width;
	configure->height = height;
	configure->serial = wl_display_next_serial(display);

	wl_list_insert(lock_surface->configure_list.prev, &configure->link);

	ext_session_lock_surface_v1_send_configure(lock_surface->resource,
		configure->serial, configure->width, configure->height);

	return configure->serial;
}

static struct lock_configure *find_configure(
		struct nx_session_lock_surface_v1 *lock_surface, uint32_t serial) {
	struct lock_configure *configure;
	wl_list_for_each(configure, &lock_surface->configure_list, link) {
		if (configure->serial == serial) {
			return configure;
		}
	}
	return NULL;
}

static void lock_surface_handle_ack_configure(struct wl_client *client,
		struct wl_resource *resource, uint32_t serial) {
	struct nx_session_lock_surface_v1 *lock_surface =
		lock_surface_from_resource(resource);
	if (lock_surface == NULL) {
		return;
	}

	struct lock_configure *acked = find_configure(lock_surface, serial);
	if (acked == NULL) {
		wl_resource_post_error(resource,
			EXT_SESSION_LOCK_SURFACE_V1_ERROR_INVALID_SERIAL,
			"ack_configure serial %" PRIu32
			" does not match any configure serial", serial);
		return;
	}

	// Older configures are implicitly acked.
	struct lock_configure *configure, *tmp;
	wl_list_for_each_safe(configure, tmp, &lock_surface->configure_list, link) {
		if (configure == acked) {
			break;
		}
		wl_list_remove(&configure->link);
		free(configure);
	}

	lock_surface->acked = (struct nx_session_lock_surface_v1_state){
		.width = acked->width,
		.height = acked->height,
		.configure_serial = acked->serial,
	};
	lock_surface->configured = true;

	wl_list_remove(&acked->link);
	free(acked);
}

static void lock_surface_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct ext_session_lock_surface_v1_interface lock_surface_implementation = {
	.destroy = lock_surface_handle_destroy,
	.ack_configure = lock_surface_handle_ack_configure,
};

static void lock_surface_role_client_commit(struct nx_surface *surface) {
	struct nx_session_lock_surface_v1 *lock_surface =
		lock_surface_try_from_nx_surface(surface);
	if (lock_surface == NULL) {
		return;
	}

	if (!nx_surface_state_has_buffer(&surface->pending)) {
		nx_surface_reject_pending(surface, lock_surface->resource,
			EXT_SESSION_LOCK_SURFACE_V1_ERROR_NULL_BUFFER,
			"session lock surface is committed with a null buffer");
		return;
	}

	if (!lock_surface->configured) {
		nx_surface_reject_pending(surface, lock_surface->resource,
			EXT_SESSION_LOCK_SURFACE_V1_ERROR_COMMIT_BEFORE_FIRST_ACK,
			"session lock surface has never been configured");
		return;
	}

	if ((uint32_t)surface->pending.width != lock_surface->acked.width ||
			(uint32_t)surface->pending.height != lock_surface->acked.height) {
		nx_surface_reject_pending(surface, lock_surface->resource,
			EXT_SESSION_LOCK_SURFACE_V1_ERROR_DIMENSIONS_MISMATCH,
			"committed surface dimensions do not match last acked configure");
	}
}

static void lock_surface_role_commit(struct nx_surface *surface) {
	if (lock_surface_try_from_nx_surface(surface) == NULL) {
		return;
	}
	nx_surface_map(surface);
}

static void lock_surface_role_destroy(struct nx_surface *surface) {
	struct nx_session_lock_surface_v1 *lock_surface =
		lock_surface_try_from_nx_surface(surface);
	if (lock_surface == NULL) {
		return;
	}
	shell_lock_surface_destroy(lock_surface);
}

static const struct nx_surface_role lock_surface_role = {
	.name = "ext_session_lock_surface_v1",
	.client_commit = lock_surface_role_client_commit,
	.commit = lock_surface_role_commit,
	.destroy = lock_surface_role_destroy,
};

static void lock_surface_handle_output_destroy(struct wl_listener *listener,
		void *data) {
	struct nx_session_lock_surface_v1 *lock_surface =
		wl_container_of(listener, lock_surface, output_destroy);
	shell_lock_surface_destroy(lock_surface);
}

static bool has_surface_on(struct nx_session_lock_v1 *lock,
		struct nx_output *output) {
	struct nx_session_lock_surface_v1 *other;
	wl_list_for_each(other, &lock->surfaces, link) {
		if (other->output == output) {
			return true;
		}
	}
	return false;
}

static void lock_surface_init(struct nx_session_lock_surface_v1 *lock_surface,
		struct nx_session_lock_v1 *lock) {
	wl_resource_set_user_data(lock_surface->resource, lock_surface);
	nx_surface_set_role_object(lock_surface->surface, lock_surface->resource);

	wl_list_insert(&lock->surfaces, &lock_surface->link);
	wl_list_init(&lock_surface->configure_list);
	wl_signal_init(&lock_surface->events.destroy);

	wl_signal_add(&lock_surface->output->events.destroy,
		&lock_surface->output_destroy);
	lock_surface->output_destroy.notify = lock_surface_handle_output_destroy;

	wl_signal_emit_mutable(&lock->events.new_surface, lock_surface);
}

void shell_lock_handle_get_lock_surface(struct wl_client *client,
		struct wl_resource *lock_resource, uint32_t id,
		struct wl_resource *surface_resource,
		struct wl_resource *output_resource) {
	// Always create, keeping client sync.
	struct wl_resource *lock_surface_resource = wl_resource_create(
		client, &ext_session_lock_surface_v1_interface,
		wl_resource_get_version(lock_resource), id);
	if (lock_surface_resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(lock_surface_resource,
		&lock_surface_implementation, NULL, NULL);

	struct nx_session_lock_v1 *lock = wl_resource_get_user_data(lock_resource);
	struct nx_output *output = nx_output_from_resource(output_resource);
	if (lock == NULL || output == NULL) {
		return;
	}

	if (has_surface_on(lock, output)) {
		wl_resource_post_error(lock_resource,
			EXT_SESSION_LOCK_V1_ERROR_DUPLICATE_OUTPUT,
			"session lock surface already created for the given output");
		return;
	}

	struct nx_surface *surface = nx_surface_from_resource(surface_resource);
	if (nx_surface_has_buffer(surface)) {
		wl_resource_post_error(lock_resource,
			EXT_SESSION_LOCK_V1_ERROR_ALREADY_CONSTRUCTED,
			"surface already has a buffer attached");
		return;
	}

	struct nx_session_lock_surface_v1 *lock_surface = calloc(1, sizeof(*lock_surface));
	if (lock_surface == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	if (!nx_surface_set_role(surface, &lock_surface_role,
			lock_resource, EXT_SESSION_LOCK_V1_ERROR_ROLE)) {
		free(lock_surface);
		return;
	}

	lock_surface->resource = lock_surface_resource;
	lock_surface->output = output;
	lock_surface->surface = surface;
	lock_surface_init(lock_surface, lock);
}
