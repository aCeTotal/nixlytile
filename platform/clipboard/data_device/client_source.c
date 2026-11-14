#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/clipboard/mime_types.h"
#include "platform/log/log.h"

static const struct wl_data_source_interface data_source_impl;

struct nx_client_data_source *client_data_source_from_resource(
		struct wl_resource *resource) {
	assert(wl_resource_instance_of(resource, &wl_data_source_interface,
		&data_source_impl));
	return wl_resource_get_user_data(resource);
}

static void client_data_source_accept(struct nx_data_source *nx_source,
	uint32_t serial, const char *mime_type);

static struct nx_client_data_source *client_data_source_from_nx_data_source(
		struct nx_data_source *nx_source) {
	assert(nx_source->impl->accept == client_data_source_accept);
	struct nx_client_data_source *source =
		wl_container_of(nx_source, source, source);
	return source;
}

static void client_data_source_accept(struct nx_data_source *nx_source,
		uint32_t serial, const char *mime_type) {
	struct nx_client_data_source *source =
		client_data_source_from_nx_data_source(nx_source);
	wl_data_source_send_target(source->resource, mime_type);
}

static void client_data_source_send(struct nx_data_source *nx_source,
		const char *mime_type, int32_t fd) {
	struct nx_client_data_source *source =
		client_data_source_from_nx_data_source(nx_source);
	wl_data_source_send_send(source->resource, mime_type, fd);
	close(fd);
}

static void client_data_source_destroy(struct nx_data_source *nx_source) {
	struct nx_client_data_source *source =
		client_data_source_from_nx_data_source(nx_source);
	wl_data_source_send_cancelled(source->resource);
	wl_resource_set_user_data(source->resource, NULL);
	free(source);
}

static void client_data_source_dnd_drop(struct nx_data_source *nx_source) {
	struct nx_client_data_source *source =
		client_data_source_from_nx_data_source(nx_source);
	assert(wl_resource_get_version(source->resource) >=
		WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION);
	wl_data_source_send_dnd_drop_performed(source->resource);
}

static void client_data_source_dnd_finish(struct nx_data_source *nx_source) {
	struct nx_client_data_source *source =
		client_data_source_from_nx_data_source(nx_source);
	assert(wl_resource_get_version(source->resource) >=
		WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION);
	wl_data_source_send_dnd_finished(source->resource);
}

static void client_data_source_dnd_action(struct nx_data_source *nx_source,
		enum wl_data_device_manager_dnd_action action) {
	struct nx_client_data_source *source =
		client_data_source_from_nx_data_source(nx_source);
	assert(wl_resource_get_version(source->resource) >=
		WL_DATA_SOURCE_ACTION_SINCE_VERSION);
	wl_data_source_send_action(source->resource, action);
}

static void data_source_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void data_source_set_actions(struct wl_client *client,
		struct wl_resource *resource, uint32_t dnd_actions) {
	struct nx_client_data_source *source =
		client_data_source_from_resource(resource);
	if (source == NULL) {
		return;
	}

	if (source->source.actions >= 0) {
		wl_resource_post_error(source->resource,
			WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
			"cannot set actions more than once");
		return;
	}

	if (dnd_actions & ~DATA_DEVICE_ALL_ACTIONS) {
		wl_resource_post_error(source->resource,
			WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
			"invalid action mask %x", dnd_actions);
		return;
	}

	if (source->finalized) {
		wl_resource_post_error(source->resource,
			WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
			"invalid action change after wl_data_device.start_drag");
		return;
	}

	source->source.actions = dnd_actions;
}

static void data_source_offer(struct wl_client *client,
		struct wl_resource *resource, const char *mime_type) {
	struct nx_client_data_source *source =
		client_data_source_from_resource(resource);
	if (source == NULL) {
		return;
	}
	if (source->finalized) {
		nx_log(NX_DEBUG, "Offering additional MIME type after "
			"wl_data_device.set_selection");
	}

	mime_types_add(&source->source.mime_types, mime_type, resource);
}

static const struct wl_data_source_interface data_source_impl = {
	.destroy = data_source_destroy,
	.offer = data_source_offer,
	.set_actions = data_source_set_actions,
};

static void data_source_handle_resource_destroy(struct wl_resource *resource) {
	struct nx_client_data_source *source =
		client_data_source_from_resource(resource);
	if (source != NULL) {
		nx_data_source_destroy(&source->source);
	}
}

// Hooks gated by client version.
static void client_data_source_init_impl(struct nx_client_data_source *source) {
	int version = wl_resource_get_version(source->resource);

	source->impl.accept = client_data_source_accept;
	source->impl.send = client_data_source_send;
	source->impl.destroy = client_data_source_destroy;

	if (version >= WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION) {
		source->impl.dnd_drop = client_data_source_dnd_drop;
	}
	if (version >= WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION) {
		source->impl.dnd_finish = client_data_source_dnd_finish;
	}
	if (version >= WL_DATA_SOURCE_ACTION_SINCE_VERSION) {
		source->impl.dnd_action = client_data_source_dnd_action;
	}
}

void client_data_source_create(struct wl_client *client, uint32_t version,
		uint32_t id) {
	struct nx_client_data_source *source = calloc(1, sizeof(*source));
	if (source == NULL) {
		return;
	}

	source->resource = wl_resource_create(client, &wl_data_source_interface,
		version, id);
	if (source->resource == NULL) {
		wl_resource_post_no_memory(source->resource);
		free(source);
		return;
	}
	wl_resource_set_implementation(source->resource, &data_source_impl,
		source, data_source_handle_resource_destroy);

	client_data_source_init_impl(source);
	nx_data_source_init(&source->source, &source->impl);
}
