#ifndef PLATFORM_CLIENT_SECURITY_CONTEXT_H
#define PLATFORM_CLIENT_SECURITY_CONTEXT_H

#include <wayland-server-core.h>

// Pair with a global filter.
struct nx_security_context_manager_v1 {
	struct wl_global *global;
	struct wl_list contexts; // nx_security_context_v1.link
	struct wl_listener display_destroy;
};

struct nx_security_context_v1_state {
	char *sandbox_engine; // may be NULL
	char *app_id; // may be NULL
	char *instance_id; // may be NULL
};

struct nx_security_context_manager_v1 *nx_security_context_manager_v1_create(
		struct wl_display *display);
// NULL unless client is sandboxed.
const struct nx_security_context_v1_state *nx_security_context_manager_v1_lookup_client(
		struct nx_security_context_manager_v1 *manager,
		const struct wl_client *client);

#endif
