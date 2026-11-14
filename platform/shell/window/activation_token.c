#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include "platform/input/seat/seat.h"
#include "platform/log/log.h"
#include "platform/shell/window/activation_token.h"
#include "platform/surface/compositor.h"
#include "xdg-activation-v1-protocol.h"

#define TOKEN_TIMEOUT_MS 30000

static const struct xdg_activation_token_v1_interface token_impl;

// NULL once committed.
static struct activation_token *token_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&xdg_activation_token_v1_interface, &token_impl));
	return wl_resource_get_user_data(resource);
}

void shell_activation_token_destroy(struct activation_token *token) {
	if (token == NULL) {
		return;
	}
	if (token->resource != NULL) {
		wl_resource_set_user_data(token->resource, NULL);
	}
	if (token->timeout != NULL) {
		wl_event_source_remove(token->timeout);
	}

	wl_list_remove(&token->link);
	wl_list_remove(&token->seat_destroy.link);
	wl_list_remove(&token->surface_destroy.link);
	free(token);
}

static int token_handle_timeout(void *data) {
	struct activation_token *token = data;
	nx_log(NX_DEBUG, "Activation token '%s' has expired", token->token);
	shell_activation_token_destroy(token);
	return 0;
}

static void token_handle_resource_destroy(struct wl_resource *resource) {
	shell_activation_token_destroy(token_from_resource(resource));
}

static void token_handle_destroy(struct wl_client *client,
		struct wl_resource *token_resource) {
	wl_resource_destroy(token_resource);
}

static bool token_init(struct activation_token *token) {
	if (!generate_token(token->token)) {
		return false;
	}

	struct wl_event_loop *loop =
		wl_display_get_event_loop(token->activation->display);
	token->timeout = wl_event_loop_add_timer(loop, token_handle_timeout, token);
	if (token->timeout == NULL) {
		return false;
	}
	wl_event_source_timer_update(token->timeout, TOKEN_TIMEOUT_MS);

	assert(wl_list_empty(&token->link));
	wl_list_insert(&token->activation->tokens, &token->link);
	return true;
}

static bool token_is_trusted(const struct activation_token *token,
		struct wl_client *client) {
	if (token->seat == NULL) {
		return true;
	}

	struct nx_seat_client *seat_client =
		nx_seat_client_for_wl_client(token->seat, client);
	if (seat_client == NULL ||
			!nx_seat_client_validate_event_serial(seat_client, token->serial)) {
		nx_log(NX_DEBUG, "Rejecting token commit request: "
			"serial %"PRIu32" was never given to client", token->serial);
		return false;
	}

	if (token->surface != NULL &&
			token->surface != token->seat->keyboard_state.focused_surface &&
			token->surface != token->seat->pointer_state.focused_surface) {
		nx_log(NX_DEBUG, "Rejecting token commit request: "
			"surface doesn't have focus");
		return false;
	}
	return true;
}

// Rejection still sends a token.
static void token_reject(struct activation_token *token,
		struct wl_resource *token_resource) {
	char token_str[TOKEN_SIZE] = {0};
	if (!generate_token(token_str)) {
		wl_client_post_no_memory(wl_resource_get_client(token_resource));
		return;
	}

	xdg_activation_token_v1_send_done(token_resource, token_str);
	shell_activation_token_destroy(token);
}

static void token_handle_commit(struct wl_client *client,
		struct wl_resource *token_resource) {
	struct activation_token *token = token_from_resource(token_resource);
	if (token == NULL) {
		wl_resource_post_error(token_resource,
			XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
			"The activation token has already been used");
		return;
	}

	wl_resource_set_user_data(token->resource, NULL);
	token->resource = NULL;

	if (!token_is_trusted(token, client)) {
		token_reject(token, token_resource);
		return;
	}

	if (!token_init(token)) {
		wl_client_post_no_memory(client);
		return;
	}

	xdg_activation_token_v1_send_done(token_resource, token->token);
}

static void token_handle_set_app_id(struct wl_client *client,
		struct wl_resource *token_resource, const char *app_id) {
	if (token_from_resource(token_resource) != NULL) {
		return;
	}
	wl_resource_post_error(token_resource,
		XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
		"The activation token has already been used");
}

static void token_handle_seat_destroy(struct wl_listener *listener, void *data) {
	struct activation_token *token =
		wl_container_of(listener, token, seat_destroy);
	wl_list_remove(&token->seat_destroy.link);
	wl_list_init(&token->seat_destroy.link);
	token->serial = 0;
	token->seat = NULL;
}

static void token_handle_set_serial(struct wl_client *client,
		struct wl_resource *token_resource, uint32_t serial,
		struct wl_resource *seat_resource) {
	struct activation_token *token = token_from_resource(token_resource);
	if (token == NULL) {
		wl_resource_post_error(token_resource,
			XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
			"The activation token has already been used");
		return;
	}

	struct nx_seat_client *seat_client =
		nx_seat_client_from_resource(seat_resource);
	if (seat_client == NULL) {
		nx_log(NX_DEBUG, "Rejecting token set_serial request: seat is inert");
		return;
	}

	token->seat = seat_client->seat;
	token->serial = serial;

	token->seat_destroy.notify = token_handle_seat_destroy;
	wl_list_remove(&token->seat_destroy.link);
	wl_signal_add(&token->seat->events.destroy, &token->seat_destroy);
}

static void token_handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct activation_token *token =
		wl_container_of(listener, token, surface_destroy);
	wl_list_remove(&token->surface_destroy.link);
	wl_list_init(&token->surface_destroy.link);
	token->surface = NULL;
}

static void token_handle_set_surface(struct wl_client *client,
		struct wl_resource *token_resource,
		struct wl_resource *surface_resource) {
	struct activation_token *token = token_from_resource(token_resource);
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);
	if (token == NULL) {
		wl_resource_post_error(token_resource,
			XDG_ACTIVATION_TOKEN_V1_ERROR_ALREADY_USED,
			"The activation token has already been used");
		return;
	}

	token->surface = surface;

	token->surface_destroy.notify = token_handle_surface_destroy;
	wl_list_remove(&token->surface_destroy.link);
	wl_signal_add(&surface->events.destroy, &token->surface_destroy);
}

static const struct xdg_activation_token_v1_interface token_impl = {
	.destroy = token_handle_destroy,
	.commit = token_handle_commit,
	.set_app_id = token_handle_set_app_id,
	.set_serial = token_handle_set_serial,
	.set_surface = token_handle_set_surface,
};

void shell_activation_handle_get_token(struct wl_client *client,
		struct wl_resource *activation_resource, uint32_t id) {
	struct activation_token *token = calloc(1, sizeof(*token));
	if (token == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_list_init(&token->link);
	wl_list_init(&token->seat_destroy.link);
	wl_list_init(&token->surface_destroy.link);
	token->activation = wl_resource_get_user_data(activation_resource);

	token->resource = wl_resource_create(client,
		&xdg_activation_token_v1_interface,
		wl_resource_get_version(activation_resource), id);
	if (token->resource == NULL) {
		free(token);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(token->resource, &token_impl, token,
		token_handle_resource_destroy);
}
