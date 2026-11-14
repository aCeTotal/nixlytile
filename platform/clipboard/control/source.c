#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include "platform/clipboard/control/data_control.h"
#include "platform/clipboard/mime_types.h"
#include "ext-data-control-v1-protocol.h"
#include "wlr-data-control-unstable-v1-protocol.h"

static const struct ext_data_control_source_v1_interface source_impl;

struct control_source *control_source_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
			&ext_data_control_source_v1_interface, &source_impl) ||
		wl_resource_instance_of(resource,
			&zwlr_data_control_source_v1_interface, &source_impl));
	return wl_resource_get_user_data(resource);
}

static void source_handle_offer(struct wl_client *client,
		struct wl_resource *resource, const char *mime_type) {
	struct control_source *source = control_source_from_resource(resource);
	if (source == NULL) {
		return;
	}

	if (source->finalized) {
		wl_resource_post_error(resource,
			EXT_DATA_CONTROL_SOURCE_V1_ERROR_INVALID_OFFER,
			"cannot mutate offer after set_selection or "
			"set_primary_selection");
		return;
	}

	mime_types_add(&source->mime_types, mime_type, resource);
}

static void source_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct ext_data_control_source_v1_interface source_impl = {
	.offer = source_handle_offer,
	.destroy = source_handle_destroy,
};

static void source_destroy(struct control_source *source) {
	if (source == NULL) {
		return;
	}

	mime_types_release(&source->mime_types);

	// Stops destructors below from recursing.
	wl_resource_set_user_data(source->resource, NULL);

	if (source->active_source != NULL) {
		nx_data_source_destroy(source->active_source);
	} else if (source->active_primary_source != NULL) {
		nx_primary_selection_source_destroy(source->active_primary_source);
	}

	free(source);
}

static void source_handle_resource_destroy(struct wl_resource *resource) {
	source_destroy(control_source_from_resource(resource));
}

// Seat dropped the source.
static void source_cancel(struct wl_resource *resource) {
	struct control_source *source = control_source_from_resource(resource);
	if (source == NULL) {
		return;
	}

	source->active_source = NULL;
	source->active_primary_source = NULL;

	ext_data_control_source_v1_send_cancelled(source->resource);
	source_destroy(source);
}

struct client_source {
	struct nx_data_source source;
	struct wl_resource *resource;
};

static const struct nx_data_source_impl client_source_impl;

static struct client_source *client_source_from_source(
		struct nx_data_source *nx_source) {
	assert(nx_source->impl == &client_source_impl);
	struct client_source *source = wl_container_of(nx_source, source, source);
	return source;
}

static void client_source_send(struct nx_data_source *nx_source,
		const char *mime_type, int fd) {
	struct client_source *source = client_source_from_source(nx_source);
	ext_data_control_source_v1_send_send(source->resource, mime_type, fd);
	close(fd);
}

static void client_source_destroy(struct nx_data_source *nx_source) {
	struct client_source *client_source = client_source_from_source(nx_source);
	struct wl_resource *resource = client_source->resource;
	free(client_source);
	source_cancel(resource);
}

static const struct nx_data_source_impl client_source_impl = {
	.send = client_source_send,
	.destroy = client_source_destroy,
};

struct client_primary_source {
	struct nx_primary_selection_source source;
	struct wl_resource *resource;
};

static const struct nx_primary_selection_source_impl client_primary_source_impl;

static struct client_primary_source *client_primary_source_from_source(
		struct nx_primary_selection_source *nx_source) {
	assert(nx_source->impl == &client_primary_source_impl);
	struct client_primary_source *source =
		wl_container_of(nx_source, source, source);
	return source;
}

static void client_primary_source_send(
		struct nx_primary_selection_source *nx_source,
		const char *mime_type, int fd) {
	struct client_primary_source *source =
		client_primary_source_from_source(nx_source);
	ext_data_control_source_v1_send_send(source->resource, mime_type, fd);
	close(fd);
}

static void client_primary_source_destroy(
		struct nx_primary_selection_source *nx_source) {
	struct client_primary_source *client_source =
		client_primary_source_from_source(nx_source);
	struct wl_resource *resource = client_source->resource;
	free(client_source);
	source_cancel(resource);
}

static const struct nx_primary_selection_source_impl client_primary_source_impl = {
	.send = client_primary_source_send,
	.destroy = client_primary_source_destroy,
};

// Moves offered types, finalizes.
static void source_finalize(struct control_source *source,
		struct wl_array *mime_types) {
	wl_array_release(mime_types);
	*mime_types = source->mime_types;
	wl_array_init(&source->mime_types);
	source->finalized = true;
}

struct nx_data_source *control_source_set_selection(
		struct control_source *source) {
	struct client_source *client_source = calloc(1, sizeof(*client_source));
	if (client_source == NULL) {
		return NULL;
	}
	client_source->resource = source->resource;

	struct nx_data_source *nx_source = &client_source->source;
	nx_data_source_init(nx_source, &client_source_impl);
	source->active_source = nx_source;

	source_finalize(source, &nx_source->mime_types);
	return nx_source;
}

struct nx_primary_selection_source *control_source_set_primary_selection(
		struct control_source *source) {
	struct client_primary_source *client_source =
		calloc(1, sizeof(*client_source));
	if (client_source == NULL) {
		return NULL;
	}
	client_source->resource = source->resource;

	struct nx_primary_selection_source *nx_source = &client_source->source;
	nx_primary_selection_source_init(nx_source, &client_primary_source_impl);
	source->active_primary_source = nx_source;

	source_finalize(source, &nx_source->mime_types);
	return nx_source;
}

void control_source_create(const struct control_protocol *protocol,
		struct wl_resource *manager_resource, uint32_t id) {
	struct control_source *source = calloc(1, sizeof(*source));
	if (source == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}
	wl_array_init(&source->mime_types);

	source->resource = wl_resource_create(
		wl_resource_get_client(manager_resource), protocol->source,
		wl_resource_get_version(manager_resource), id);
	if (source->resource == NULL) {
		wl_resource_post_no_memory(manager_resource);
		wl_array_release(&source->mime_types);
		free(source);
		return;
	}
	wl_resource_set_implementation(source->resource, &source_impl, source,
		source_handle_resource_destroy);
}
