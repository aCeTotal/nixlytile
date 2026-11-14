#include <assert.h>
#include <stdlib.h>
#include "platform/log/log.h"
#include "platform/backend/headless/backend.h"

static const struct nx_backend_impl backend_impl;

static bool backend_is_headless(struct nx_backend *backend) {
	return backend->impl == &backend_impl;
}

struct nx_headless_backend *headless_backend_from_backend(
		struct nx_backend *nx_backend) {
	assert(backend_is_headless(nx_backend));
	struct nx_headless_backend *backend = wl_container_of(nx_backend, backend, backend);
	return backend;
}

static bool backend_start(struct nx_backend *nx_backend) {
	struct nx_headless_backend *backend =
		headless_backend_from_backend(nx_backend);
	nx_log(NX_INFO, "Starting headless backend");

	struct nx_headless_output *output;
	wl_list_for_each(output, &backend->outputs, link) {
		wl_signal_emit_mutable(&backend->backend.events.new_output,
			&output->nx_output);
	}

	backend->started = true;
	return true;
}

static void backend_destroy(struct nx_backend *nx_backend) {
	struct nx_headless_backend *backend =
		headless_backend_from_backend(nx_backend);

	nx_backend_finish(nx_backend);

	struct nx_headless_output *output, *output_tmp;
	wl_list_for_each_safe(output, output_tmp, &backend->outputs, link) {
		nx_output_destroy(&output->nx_output);
	}

	wl_list_remove(&backend->event_loop_destroy.link);
	free(backend);
}

static const struct nx_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
};

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
	struct nx_headless_backend *backend =
		wl_container_of(listener, backend, event_loop_destroy);
	backend_destroy(&backend->backend);
}

struct nx_backend *nx_headless_backend_create(struct wl_event_loop *loop) {
	nx_log(NX_INFO, "Creating headless backend");

	struct nx_headless_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		nx_log(NX_ERROR, "Failed to allocate nx_headless_backend");
		return NULL;
	}

	nx_backend_init(&backend->backend, &backend_impl);
	backend->backend.buffer_caps =
		NX_BUFFER_CAP_DATA_PTR | NX_BUFFER_CAP_DMABUF | NX_BUFFER_CAP_SHM;
	backend->backend.features.timeline = true;

	backend->event_loop = loop;
	wl_list_init(&backend->outputs);

	backend->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(loop, &backend->event_loop_destroy);

	return &backend->backend;
}
