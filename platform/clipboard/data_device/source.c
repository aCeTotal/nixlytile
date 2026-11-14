#include <assert.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include "platform/clipboard/data_device/data_device.h"
#include "platform/clipboard/mime_types.h"

void nx_data_source_init(struct nx_data_source *source,
		const struct nx_data_source_impl *impl) {
	assert(impl->send);

	*source = (struct nx_data_source){
		.impl = impl,
		.actions = -1,
	};
	wl_array_init(&source->mime_types);
	wl_signal_init(&source->events.destroy);
}

void nx_data_source_send(struct nx_data_source *source, const char *mime_type,
		int32_t fd) {
	source->impl->send(source, mime_type, fd);
}

void nx_data_source_accept(struct nx_data_source *source, uint32_t serial,
		const char *mime_type) {
	source->accepted = (mime_type != NULL);
	if (source->impl->accept) {
		source->impl->accept(source, serial, mime_type);
	}
}

void nx_data_source_destroy(struct nx_data_source *source) {
	if (source == NULL) {
		return;
	}

	wl_signal_emit_mutable(&source->events.destroy, source);

	assert(wl_list_empty(&source->events.destroy.listener_list));

	mime_types_release(&source->mime_types);

	if (source->impl->destroy) {
		source->impl->destroy(source);
		return;
	}
	free(source);
}

void nx_data_source_dnd_drop(struct nx_data_source *source) {
	if (source->impl->dnd_drop) {
		source->impl->dnd_drop(source);
	}
}

void nx_data_source_dnd_finish(struct nx_data_source *source) {
	if (source->impl->dnd_finish) {
		source->impl->dnd_finish(source);
	}
}

void nx_data_source_dnd_action(struct nx_data_source *source,
		enum wl_data_device_manager_dnd_action action) {
	source->current_dnd_action = action;
	if (source->impl->dnd_action) {
		source->impl->dnd_action(source, action);
	}
}
