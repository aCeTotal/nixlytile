#include <assert.h>
#include "platform/backend/backend_interface.h"

void nx_backend_init(struct nx_backend *backend,
		const struct nx_backend_impl *impl) {
	*backend = (struct nx_backend){
		.impl = impl,
	};
	wl_signal_init(&backend->events.destroy);
	wl_signal_init(&backend->events.new_input);
	wl_signal_init(&backend->events.new_output);
}

void nx_backend_finish(struct nx_backend *backend) {
	wl_signal_emit_mutable(&backend->events.destroy, backend);

	assert(wl_list_empty(&backend->events.destroy.listener_list));
	assert(wl_list_empty(&backend->events.new_input.listener_list));
	assert(wl_list_empty(&backend->events.new_output.listener_list));
}

bool nx_backend_start(struct nx_backend *backend) {
	return backend->impl->start(backend);
}

void nx_backend_destroy(struct nx_backend *backend) {
	if (backend == NULL) {
		return;
	}
	backend->impl->destroy(backend);
}

int nx_backend_get_drm_fd(struct nx_backend *backend) {
	if (backend->impl->get_drm_fd == NULL) {
		return -1;
	}
	return backend->impl->get_drm_fd(backend);
}
