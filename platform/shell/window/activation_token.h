#ifndef PLATFORM_SHELL_WINDOW_ACTIVATION_TOKEN_H
#define PLATFORM_SHELL_WINDOW_ACTIVATION_TOKEN_H

#include "platform/protocol/token.h"
#include "platform/shell/window/xdg_activation.h"

struct activation_token {
	struct nx_xdg_activation_v1 *activation;
	struct wl_list link; // nx_xdg_activation_v1.tokens
	char token[TOKEN_SIZE];

	// Client hints, checked on commit.
	struct nx_surface *surface;
	struct nx_seat *seat;
	uint32_t serial;

	struct wl_resource *resource; // NULL after commit
	struct wl_event_source *timeout;

	struct wl_listener seat_destroy;
	struct wl_listener surface_destroy;
};

// xdg_activation_v1.get_activation_token request.
void shell_activation_handle_get_token(struct wl_client *client,
	struct wl_resource *activation_resource, uint32_t id);
void shell_activation_token_destroy(struct activation_token *token);

#endif
