#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/client/context.h"
#include "security-context-v1-protocol.h"

struct nx_security_context_v1 {
	struct nx_security_context_manager_v1 *manager;
	struct nx_security_context_v1_state state;
	struct wl_list link; // nx_security_context_manager_v1.contexts
	int listen_fd, close_fd;
	struct wl_event_source *listen_source, *close_source;
};

static const struct wp_security_context_v1_interface security_context_impl;

// Returns NULL once committed.
static struct nx_security_context_v1 *security_context_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&wp_security_context_v1_interface, &security_context_impl));
	return wl_resource_get_user_data(resource);
}

void security_context_state_finish(struct nx_security_context_v1_state *state) {
	free(state->app_id);
	free(state->sandbox_engine);
	free(state->instance_id);
}

static void security_context_destroy(
		struct nx_security_context_v1 *security_context) {
	if (security_context->listen_source != NULL) {
		wl_event_source_remove(security_context->listen_source);
	}
	if (security_context->close_source != NULL) {
		wl_event_source_remove(security_context->close_source);
	}
	close(security_context->listen_fd);
	close(security_context->close_fd);

	security_context_state_finish(&security_context->state);
	wl_list_remove(&security_context->link);
	free(security_context);
}

void security_context_destroy_all(struct nx_security_context_manager_v1 *manager) {
	struct nx_security_context_v1 *security_context, *tmp;
	wl_list_for_each_safe(security_context, tmp, &manager->contexts, link) {
		security_context_destroy(security_context);
	}
}

static int handle_listen_fd_event(int listen_fd, uint32_t mask, void *data) {
	struct nx_security_context_v1 *security_context = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		security_context_destroy(security_context);
		return 0;
	}
	if (mask & WL_EVENT_READABLE) {
		security_context_accept_client(
			wl_global_get_display(security_context->manager->global),
			listen_fd, &security_context->state);
	}
	return 0;
}

static int handle_close_fd_event(int fd, uint32_t mask, void *data) {
	struct nx_security_context_v1 *security_context = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		security_context_destroy(security_context);
	}
	return 0;
}

// Posts an error once committed.
static struct nx_security_context_v1 *uncommitted_from_resource(
		struct wl_resource *resource) {
	struct nx_security_context_v1 *security_context =
		security_context_from_resource(resource);
	if (security_context == NULL) {
		wl_resource_post_error(resource,
			WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_USED,
			"Security context has already been committed");
	}
	return security_context;
}

static void security_context_handle_commit(struct wl_client *client,
		struct wl_resource *resource) {
	struct nx_security_context_v1 *security_context =
		uncommitted_from_resource(resource);
	if (security_context == NULL) {
		return;
	}
	// Global filters normally prevent this.
	if (nx_security_context_manager_v1_lookup_client(security_context->manager,
			client) != NULL) {
		wl_resource_post_error(resource,
			WP_SECURITY_CONTEXT_MANAGER_V1_ERROR_NESTED,
			"Nested security contexts are forbidden");
		return;
	}

	struct wl_event_loop *loop =
		wl_display_get_event_loop(wl_client_get_display(client));
	security_context->listen_source = wl_event_loop_add_fd(loop,
		security_context->listen_fd, WL_EVENT_READABLE,
		handle_listen_fd_event, security_context);
	if (security_context->listen_source == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}
	security_context->close_source = wl_event_loop_add_fd(loop,
		security_context->close_fd, 0, handle_close_fd_event,
		security_context);
	if (security_context->close_source == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}
	wl_resource_set_user_data(resource, NULL);
}

static void set_state_field(struct wl_resource *resource, char **field,
		const char *value, const char *field_name) {
	if (*field != NULL) {
		wl_resource_post_error(resource,
			WP_SECURITY_CONTEXT_V1_ERROR_ALREADY_SET,
			"%s has already been set", field_name);
		return;
	}
	*field = strdup(value);
	if (*field == NULL) {
		wl_resource_post_no_memory(resource);
	}
}

static void security_context_handle_set_sandbox_engine(struct wl_client *client,
		struct wl_resource *resource, const char *sandbox_engine) {
	struct nx_security_context_v1 *security_context =
		uncommitted_from_resource(resource);
	if (security_context != NULL) {
		set_state_field(resource, &security_context->state.sandbox_engine,
			sandbox_engine, "Sandbox engine");
	}
}

static void security_context_handle_set_app_id(struct wl_client *client,
		struct wl_resource *resource, const char *app_id) {
	struct nx_security_context_v1 *security_context =
		uncommitted_from_resource(resource);
	if (security_context != NULL) {
		set_state_field(resource, &security_context->state.app_id,
			app_id, "App ID");
	}
}

static void security_context_handle_set_instance_id(struct wl_client *client,
		struct wl_resource *resource, const char *instance_id) {
	struct nx_security_context_v1 *security_context =
		uncommitted_from_resource(resource);
	if (security_context != NULL) {
		set_state_field(resource, &security_context->state.instance_id,
			instance_id, "Instance ID");
	}
}

static void security_context_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct wp_security_context_v1_interface security_context_impl = {
	.destroy = security_context_handle_destroy,
	.commit = security_context_handle_commit,
	.set_sandbox_engine = security_context_handle_set_sandbox_engine,
	.set_app_id = security_context_handle_set_app_id,
	.set_instance_id = security_context_handle_set_instance_id,
};

static void security_context_resource_destroy(struct wl_resource *resource) {
	struct nx_security_context_v1 *security_context =
		security_context_from_resource(resource);
	if (security_context != NULL) {
		security_context_destroy(security_context);
	}
}

void security_context_create(struct wl_resource *manager_resource, uint32_t id,
		int listen_fd, int close_fd) {
	struct nx_security_context_v1 *security_context =
		calloc(1, sizeof(*security_context));
	if (security_context == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	security_context->manager = wl_resource_get_user_data(manager_resource);
	security_context->listen_fd = listen_fd;
	security_context->close_fd = close_fd;

	struct wl_resource *resource = wl_resource_create(
		wl_resource_get_client(manager_resource),
		&wp_security_context_v1_interface,
		wl_resource_get_version(manager_resource), id);
	if (resource == NULL) {
		free(security_context);
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_resource_set_implementation(resource, &security_context_impl,
		security_context, security_context_resource_destroy);
	wl_list_insert(&security_context->manager->contexts, &security_context->link);
}
