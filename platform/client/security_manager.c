#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "platform/client/context.h"
#include "platform/log/log.h"
#include "security-context-v1-protocol.h"

#define SECURITY_CONTEXT_MANAGER_V1_VERSION 1

static void post_invalid_listen_fd(struct wl_resource *manager_resource,
		const char *message) {
	wl_resource_post_error(manager_resource,
		WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_INVALID_LISTEN_FD, "%s", message);
}

// Posts error unless listening socket.
static bool validate_listen_fd(struct wl_resource *manager_resource,
		int listen_fd) {
	struct stat stat_buf = {0};
	if (fstat(listen_fd, &stat_buf) != 0) {
		nx_log_errno(NX_ERROR, "fstat failed on listen FD");
		post_invalid_listen_fd(manager_resource, "Invalid listen_fd");
		return false;
	}
	if (!S_ISSOCK(stat_buf.st_mode)) {
		post_invalid_listen_fd(manager_resource, "listen_fd is not a socket");
		return false;
	}

	int accept_conn = 0;
	socklen_t accept_conn_size = sizeof(accept_conn);
	if (getsockopt(listen_fd, SOL_SOCKET, SO_ACCEPTCONN, &accept_conn,
			&accept_conn_size) != 0) {
		nx_log_errno(NX_ERROR, "getsockopt failed on listen FD");
		post_invalid_listen_fd(manager_resource, "Invalid listen_fd");
		return false;
	}
	if (accept_conn == 0) {
		post_invalid_listen_fd(manager_resource,
			"listen_fd is not a listening socket");
		return false;
	}
	return true;
}

static void manager_handle_create_listener(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id,
		int listen_fd, int close_fd) {
	if (validate_listen_fd(manager_resource, listen_fd)) {
		security_context_create(manager_resource, id, listen_fd, close_fd);
	}
}

static void manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_security_context_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.create_listener = manager_handle_create_listener,
};

static void manager_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&wp_security_context_manager_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_security_context_manager_v1 *manager =
		wl_container_of(listener, manager, display_destroy);
	security_context_destroy_all(manager);
	wl_global_destroy(manager->global);
	wl_list_remove(&manager->display_destroy.link);
	free(manager);
}

struct nx_security_context_manager_v1 *nx_security_context_manager_v1_create(
		struct wl_display *display) {
	struct nx_security_context_manager_v1 *manager = calloc(1, sizeof(*manager));
	if (manager == NULL) {
		return NULL;
	}
	manager->global = wl_global_create(display,
		&wp_security_context_manager_v1_interface,
		SECURITY_CONTEXT_MANAGER_V1_VERSION, manager, manager_bind);
	if (manager->global == NULL) {
		free(manager);
		return NULL;
	}

	wl_list_init(&manager->contexts);
	manager->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &manager->display_destroy);
	return manager;
}
