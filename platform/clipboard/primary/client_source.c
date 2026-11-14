#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include "platform/clipboard/mime_types.h"
#include "platform/clipboard/primary/device.h"
#include "platform/log/log.h"
#include "primary-selection-unstable-v1-protocol.h"

struct client_source {
	struct nx_primary_selection_source source;
	struct wl_resource *resource;
	bool finalized;
};

static void client_source_send(struct nx_primary_selection_source *nx_source,
		const char *mime_type, int fd) {
	struct client_source *source = wl_container_of(nx_source, source, source);
	zwp_primary_selection_source_v1_send_send(source->resource, mime_type, fd);
	close(fd);
}

static void client_source_destroy(struct nx_primary_selection_source *nx_source) {
	struct client_source *source = wl_container_of(nx_source, source, source);
	zwp_primary_selection_source_v1_send_cancelled(source->resource);
	wl_resource_set_user_data(source->resource, NULL);
	free(source);
}

static const struct nx_primary_selection_source_impl client_source_impl = {
	.send = client_source_send,
	.destroy = client_source_destroy,
};

static const struct zwp_primary_selection_source_v1_interface source_impl;

static struct client_source *client_source_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource,
		&zwp_primary_selection_source_v1_interface, &source_impl));
	return wl_resource_get_user_data(resource);
}

static void source_handle_offer(struct wl_client *client,
		struct wl_resource *resource, const char *mime_type) {
	struct client_source *source = client_source_from_resource(resource);
	if (source == NULL) {
		return;
	}
	if (source->finalized) {
		nx_log(NX_DEBUG, "Offering additional MIME type after set_selection");
	}

	mime_types_add(&source->source.mime_types, mime_type, resource);
}

static void source_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static const struct zwp_primary_selection_source_v1_interface source_impl = {
	.offer = source_handle_offer,
	.destroy = source_handle_destroy,
};

static void source_resource_handle_destroy(struct wl_resource *resource) {
	struct client_source *source = client_source_from_resource(resource);
	if (source == NULL) {
		return;
	}
	nx_primary_selection_source_destroy(&source->source);
}

struct nx_primary_selection_source *primary_source_finalize(
		struct wl_resource *source_resource) {
	if (source_resource == NULL) {
		return NULL;
	}

	struct client_source *source = client_source_from_resource(source_resource);
	if (source == NULL) {
		return NULL;
	}
	source->finalized = true;
	return &source->source;
}

void primary_source_create(struct wl_client *client,
		struct wl_resource *manager_resource, uint32_t id) {
	struct client_source *source = calloc(1, sizeof(*source));
	if (source == NULL) {
		wl_client_post_no_memory(client);
		return;
	}
	nx_primary_selection_source_init(&source->source, &client_source_impl);

	uint32_t version = wl_resource_get_version(manager_resource);
	source->resource = wl_resource_create(client,
		&zwp_primary_selection_source_v1_interface, version, id);
	if (source->resource == NULL) {
		free(source);
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(source->resource, &source_impl, source,
		source_resource_handle_destroy);
}
