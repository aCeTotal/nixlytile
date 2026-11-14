#include <assert.h>
#include <stdlib.h>
#include "platform/shell/lock/lock_surface.h"
#include "ext-session-lock-v1-protocol.h"

#define SESSION_LOCK_VERSION 1

static const struct ext_session_lock_manager_v1_interface lock_manager_implementation;
static const struct ext_session_lock_v1_interface lock_implementation;

static struct nx_session_lock_manager_v1 *lock_manager_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_session_lock_manager_v1_interface, &lock_manager_implementation));
	struct nx_session_lock_manager_v1 *lock_manager =
		wl_resource_get_user_data(resource);
	assert(lock_manager != NULL);
	return lock_manager;
}

static struct nx_session_lock_v1 *lock_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&ext_session_lock_v1_interface, &lock_implementation));
	return wl_resource_get_user_data(resource);
}

static void lock_handle_unlock_and_destroy(struct wl_client *client,
		struct wl_resource *lock_resource) {
	struct nx_session_lock_v1 *lock = lock_from_resource(lock_resource);
	// Already finished by the compositor.
	if (lock == NULL) {
		wl_resource_destroy(lock_resource);
		return;
	}

	if (!lock->locked_sent) {
		wl_resource_post_error(lock_resource,
			EXT_SESSION_LOCK_V1_ERROR_INVALID_UNLOCK,
			"the locked event was never sent");
		return;
	}

	wl_signal_emit_mutable(&lock->events.unlock, NULL);
	wl_resource_destroy(lock_resource);
}

static void lock_handle_destroy(struct wl_client *client,
		struct wl_resource *lock_resource) {
	struct nx_session_lock_v1 *lock = lock_from_resource(lock_resource);
	if (lock == NULL) {
		wl_resource_destroy(lock_resource);
		return;
	}

	wl_resource_post_error(lock_resource,
		EXT_SESSION_LOCK_V1_ERROR_INVALID_DESTROY,
		lock->locked_sent ?
			"the session lock may not be destroyed while locked" :
			"the finished event was never sent");
}

static const struct ext_session_lock_v1_interface lock_implementation = {
	.destroy = lock_handle_destroy,
	.get_lock_surface = shell_lock_handle_get_lock_surface,
	.unlock_and_destroy = lock_handle_unlock_and_destroy,
};

void nx_session_lock_v1_send_locked(struct nx_session_lock_v1 *lock) {
	assert(!lock->locked_sent);
	lock->locked_sent = true;
	ext_session_lock_v1_send_locked(lock->resource);
}

static void lock_destroy(struct nx_session_lock_v1 *lock) {
	struct nx_session_lock_surface_v1 *lock_surface, *tmp;
	wl_list_for_each_safe(lock_surface, tmp, &lock->surfaces, link) {
		shell_lock_surface_destroy(lock_surface);
	}
	assert(wl_list_empty(&lock->surfaces));

	wl_signal_emit_mutable(&lock->events.destroy, NULL);

	assert(wl_list_empty(&lock->events.new_surface.listener_list));
	assert(wl_list_empty(&lock->events.unlock.listener_list));
	assert(wl_list_empty(&lock->events.destroy.listener_list));

	wl_resource_set_user_data(lock->resource, NULL);
	free(lock);
}

// Also inerts its lock surfaces.
void nx_session_lock_v1_destroy(struct nx_session_lock_v1 *lock) {
	ext_session_lock_v1_send_finished(lock->resource);
	lock_destroy(lock);
}

static void lock_resource_destroy(struct wl_resource *lock_resource) {
	struct nx_session_lock_v1 *lock = lock_from_resource(lock_resource);
	if (lock != NULL) {
		lock_destroy(lock);
	}
}

static void lock_manager_handle_lock(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	struct nx_session_lock_manager_v1 *lock_manager =
		lock_manager_from_resource(manager_resource);

	struct nx_session_lock_v1 *lock = calloc(1, sizeof(*lock));
	if (lock == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	lock->resource = wl_resource_create(client, &ext_session_lock_v1_interface,
		wl_resource_get_version(manager_resource), id);
	if (lock->resource == NULL) {
		free(lock);
		wl_client_post_no_memory(client);
		return;
	}

	wl_list_init(&lock->surfaces);

	wl_signal_init(&lock->events.new_surface);
	wl_signal_init(&lock->events.unlock);
	wl_signal_init(&lock->events.destroy);

	wl_resource_set_implementation(lock->resource, &lock_implementation,
		lock, lock_resource_destroy);

	wl_signal_emit_mutable(&lock_manager->events.new_lock, lock);
}

static void lock_manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct ext_session_lock_manager_v1_interface lock_manager_implementation = {
	.destroy = lock_manager_handle_destroy,
	.lock = lock_manager_handle_lock,
};

static void lock_manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(
		client, &ext_session_lock_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &lock_manager_implementation,
		data, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_session_lock_manager_v1 *lock_manager =
		wl_container_of(listener, lock_manager, display_destroy);
	wl_list_remove(&lock_manager->display_destroy.link);

	wl_global_destroy(lock_manager->global);

	assert(wl_list_empty(&lock_manager->events.new_lock.listener_list));

	free(lock_manager);
}

struct nx_session_lock_manager_v1 *nx_session_lock_manager_v1_create(struct wl_display *display) {
	struct nx_session_lock_manager_v1 *lock_manager = calloc(1, sizeof(*lock_manager));
	if (lock_manager == NULL) {
		return NULL;
	}

	lock_manager->global = wl_global_create(display,
		&ext_session_lock_manager_v1_interface, SESSION_LOCK_VERSION,
		lock_manager, lock_manager_bind);
	if (lock_manager->global == NULL) {
		free(lock_manager);
		return NULL;
	}

	wl_signal_init(&lock_manager->events.new_lock);

	lock_manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &lock_manager->display_destroy);

	return lock_manager;
}
