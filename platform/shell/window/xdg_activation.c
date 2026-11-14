#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/log/log.h"
#include "platform/shell/window/activation_token.h"
#include "platform/surface/compositor.h"
#include "xdg-activation-v1-protocol.h"

#define XDG_ACTIVATION_V1_VERSION 1

static const struct xdg_activation_v1_interface activation_impl;

static struct nx_xdg_activation_v1 *activation_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&xdg_activation_v1_interface, &activation_impl));
	return wl_resource_get_user_data(resource);
}

static struct activation_token *find_token(
		struct nx_xdg_activation_v1 *activation, const char *token_str) {
	struct activation_token *token;
	wl_list_for_each(token, &activation->tokens, link) {
		if (strcmp(token_str, token->token) == 0) {
			return token;
		}
	}
	return NULL;
}

static void activation_handle_destroy(struct wl_client *client,
		struct wl_resource *activation_resource) {
	wl_resource_destroy(activation_resource);
}

static void activation_handle_activate(struct wl_client *client,
		struct wl_resource *activation_resource, const char *token_str,
		struct wl_resource *surface_resource) {
	struct nx_xdg_activation_v1 *activation =
		activation_from_resource(activation_resource);
	struct nx_surface *surface = nx_surface_from_resource(surface_resource);

	struct activation_token *token = find_token(activation, token_str);
	if (token == NULL) {
		nx_log(NX_DEBUG, "Rejecting activate request: unknown token");
		return;
	}

	struct nx_xdg_activation_v1_request_activate_event event = {
		.surface = surface,
	};
	wl_signal_emit_mutable(&activation->events.request_activate, &event);

	shell_activation_token_destroy(token);
}

static const struct xdg_activation_v1_interface activation_impl = {
	.destroy = activation_handle_destroy,
	.get_activation_token = shell_activation_handle_get_token,
	.activate = activation_handle_activate,
};

static void activation_bind(struct wl_client *client, void *data,
		uint32_t version, uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&xdg_activation_v1_interface, version, id);
	if (resource == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &activation_impl, data, NULL);
}

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct nx_xdg_activation_v1 *activation =
		wl_container_of(listener, activation, display_destroy);

	assert(wl_list_empty(&activation->events.request_activate.listener_list));

	struct activation_token *token, *tmp;
	wl_list_for_each_safe(token, tmp, &activation->tokens, link) {
		shell_activation_token_destroy(token);
	}

	wl_list_remove(&activation->display_destroy.link);
	wl_global_destroy(activation->global);
	free(activation);
}

struct nx_xdg_activation_v1 *nx_xdg_activation_v1_create(
		struct wl_display *display) {
	struct nx_xdg_activation_v1 *activation = calloc(1, sizeof(*activation));
	if (activation == NULL) {
		return NULL;
	}

	wl_list_init(&activation->tokens);
	wl_signal_init(&activation->events.request_activate);

	activation->global = wl_global_create(display,
		&xdg_activation_v1_interface, XDG_ACTIVATION_V1_VERSION, activation,
		activation_bind);
	if (activation->global == NULL) {
		free(activation);
		return NULL;
	}

	activation->display = display;

	activation->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &activation->display_destroy);

	return activation;
}
