#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "platform/client/context.h"
#include "platform/log/log.h"
#include "platform/os/fd.h"

// Client connected through a context.
struct nx_security_context_v1_client {
	struct nx_security_context_v1_state state;
	struct wl_listener destroy;
};

static bool copy_state_field(char **dst, const char *src) {
	if (src == NULL) {
		return true;
	}
	*dst = strdup(src);
	return *dst != NULL;
}

// Caller frees partial copies.
static bool state_copy(struct nx_security_context_v1_state *dst,
		const struct nx_security_context_v1_state *src) {
	return copy_state_field(&dst->app_id, src->app_id) &&
		copy_state_field(&dst->sandbox_engine, src->sandbox_engine) &&
		copy_state_field(&dst->instance_id, src->instance_id);
}

static void client_destroy(struct nx_security_context_v1_client *sc_client) {
	wl_list_remove(&sc_client->destroy.link);
	security_context_state_finish(&sc_client->state);
	free(sc_client);
}

// Its identity marks sandboxed clients.
static void client_handle_destroy(struct wl_listener *listener, void *data) {
	struct nx_security_context_v1_client *sc_client =
		wl_container_of(listener, sc_client, destroy);
	client_destroy(sc_client);
}

const struct nx_security_context_v1_state *nx_security_context_manager_v1_lookup_client(
		struct nx_security_context_manager_v1 *manager,
		const struct wl_client *client) {
	struct wl_listener *listener = wl_client_get_destroy_listener(
		(struct wl_client *)client, client_handle_destroy);
	if (listener == NULL) {
		return NULL;
	}
	struct nx_security_context_v1_client *sc_client =
		wl_container_of(listener, sc_client, destroy);
	return &sc_client->state;
}

void security_context_accept_client(struct wl_display *display, int listen_fd,
		const struct nx_security_context_v1_state *state) {
	int client_fd = accept(listen_fd, NULL, NULL);
	if (client_fd < 0) {
		nx_log_errno(NX_ERROR, "accept failed");
		return;
	}
	if (!set_cloexec(client_fd, true)) {
		close(client_fd);
		return;
	}

	struct nx_security_context_v1_client *sc_client =
		calloc(1, sizeof(*sc_client));
	if (sc_client == NULL) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		close(client_fd);
		return;
	}
	struct wl_client *client = wl_client_create(display, client_fd);
	if (client == NULL) {
		nx_log(NX_ERROR, "wl_client_create failed");
		close(client_fd);
		free(sc_client);
		return;
	}

	sc_client->destroy.notify = client_handle_destroy;
	wl_client_add_destroy_listener(client, &sc_client->destroy);
	if (!state_copy(&sc_client->state, state)) {
		client_destroy(sc_client);
		wl_client_post_no_memory(client);
	}
}
