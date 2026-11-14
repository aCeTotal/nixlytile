#ifndef PLATFORM_CLIENT_CONTEXT_H
#define PLATFORM_CLIENT_CONTEXT_H

#include <wayland-server-core.h>
#include "platform/client/security_context.h"

void security_context_create(struct wl_resource *manager_resource, uint32_t id,
		int listen_fd, int close_fd);
void security_context_destroy_all(struct nx_security_context_manager_v1 *manager);
void security_context_state_finish(struct nx_security_context_v1_state *state);
void security_context_accept_client(struct wl_display *display, int listen_fd,
		const struct nx_security_context_v1_state *state);

#endif
