#include <assert.h>
#include <stdlib.h>
#include "platform/buffer/buffer.h"
#include "platform/log/log.h"
#include "platform/backend/backend_interface.h"
#include "platform/backend/multi.h"

struct nx_multi_backend {
	struct nx_backend backend;
	struct wl_list backends;
	struct wl_listener event_loop_destroy;
};

struct subbackend_state {
	struct nx_backend *backend;
	struct nx_backend *container;
	struct wl_listener new_input;
	struct wl_listener new_output;
	struct wl_listener destroy;
	struct wl_list link;
};

static struct nx_multi_backend *multi_backend_from_backend(
		struct nx_backend *nx_backend) {
	assert(nx_backend_is_multi(nx_backend));
	struct nx_multi_backend *backend = wl_container_of(nx_backend, backend, backend);
	return backend;
}

static bool multi_backend_start(struct nx_backend *nx_backend) {
	struct nx_multi_backend *backend = multi_backend_from_backend(nx_backend);
	struct subbackend_state *sub;
	wl_list_for_each(sub, &backend->backends, link) {
		if (!nx_backend_start(sub->backend)) {
			nx_log(NX_ERROR, "Failed to initialize backend.");
			return false;
		}
	}
	return true;
}

static void subbackend_state_destroy(struct subbackend_state *sub) {
	wl_list_remove(&sub->new_input.link);
	wl_list_remove(&sub->new_output.link);
	wl_list_remove(&sub->destroy.link);
	wl_list_remove(&sub->link);
	free(sub);
}

static void multi_backend_destroy(struct nx_backend *nx_backend) {
	struct nx_multi_backend *backend = multi_backend_from_backend(nx_backend);

	wl_list_remove(&backend->event_loop_destroy.link);
	nx_backend_finish(nx_backend);

	// Children may destroy their siblings.
	while (!wl_list_empty(&backend->backends)) {
		struct subbackend_state *sub =
			wl_container_of(backend->backends.next, sub, link);
		nx_backend_destroy(sub->backend);
	}

	free(backend);
}

static int multi_backend_get_drm_fd(struct nx_backend *backend) {
	struct nx_multi_backend *multi = multi_backend_from_backend(backend);

	struct subbackend_state *sub;
	wl_list_for_each(sub, &multi->backends, link) {
		if (sub->backend->impl->get_drm_fd) {
			return nx_backend_get_drm_fd(sub->backend);
		}
	}
	return -1;
}

static const struct nx_backend_impl backend_impl = {
	.start = multi_backend_start,
	.destroy = multi_backend_destroy,
	.get_drm_fd = multi_backend_get_drm_fd,
};

static void handle_event_loop_destroy(struct wl_listener *listener, void *data) {
	struct nx_multi_backend *backend =
		wl_container_of(listener, backend, event_loop_destroy);
	multi_backend_destroy(&backend->backend);
}

struct nx_backend *nx_multi_backend_create(struct wl_event_loop *loop) {
	struct nx_multi_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) {
		nx_log(NX_ERROR, "Backend allocation failed");
		return NULL;
	}

	wl_list_init(&backend->backends);
	nx_backend_init(&backend->backend, &backend_impl);

	backend->event_loop_destroy.notify = handle_event_loop_destroy;
	wl_event_loop_add_destroy_listener(loop, &backend->event_loop_destroy);

	return &backend->backend;
}

bool nx_backend_is_multi(struct nx_backend *b) {
	return b->impl == &backend_impl;
}

static void new_input_reemit(struct wl_listener *listener, void *data) {
	struct subbackend_state *state = wl_container_of(listener, state, new_input);
	wl_signal_emit_mutable(&state->container->events.new_input, data);
}

static void new_output_reemit(struct wl_listener *listener, void *data) {
	struct subbackend_state *state = wl_container_of(listener, state, new_output);
	wl_signal_emit_mutable(&state->container->events.new_output, data);
}

static void handle_subbackend_destroy(struct wl_listener *listener,
		void *data) {
	struct subbackend_state *state = wl_container_of(listener, state, destroy);
	subbackend_state_destroy(state);
}

static bool has_subbackend(struct nx_multi_backend *multi,
		struct nx_backend *backend) {
	struct subbackend_state *sub;
	wl_list_for_each(sub, &multi->backends, link) {
		if (sub->backend == backend) {
			return true;
		}
	}
	return false;
}

static void multi_backend_refresh_features(struct nx_multi_backend *multi) {
	multi->backend.buffer_caps = 0;
	multi->backend.features.timeline = true;

	bool has_buffer_cap = false;
	uint32_t buffer_caps_intersection =
		NX_BUFFER_CAP_DATA_PTR | NX_BUFFER_CAP_DMABUF | NX_BUFFER_CAP_SHM;
	struct subbackend_state *sub;
	wl_list_for_each(sub, &multi->backends, link) {
		// Only presenting backends count.
		if (sub->backend->buffer_caps != 0) {
			has_buffer_cap = true;
			buffer_caps_intersection &= sub->backend->buffer_caps;
		}
		// Timelines need DMA-BUF support.
		if (sub->backend->buffer_caps & NX_BUFFER_CAP_DMABUF) {
			multi->backend.features.timeline = multi->backend.features.timeline &&
				sub->backend->features.timeline;
		}
	}

	if (has_buffer_cap) {
		multi->backend.buffer_caps = buffer_caps_intersection;
	}
}

bool nx_multi_backend_add(struct nx_backend *multi_backend,
		struct nx_backend *backend) {
	assert(multi_backend && backend);
	assert(multi_backend != backend);

	struct nx_multi_backend *multi = multi_backend_from_backend(multi_backend);
	if (has_subbackend(multi, backend)) {
		return true;
	}

	struct subbackend_state *sub = calloc(1, sizeof(*sub));
	if (sub == NULL) {
		nx_log(NX_ERROR, "Could not add backend: allocation failed");
		return false;
	}
	wl_list_insert(multi->backends.prev, &sub->link);

	sub->backend = backend;
	sub->container = &multi->backend;

	sub->destroy.notify = handle_subbackend_destroy;
	wl_signal_add(&backend->events.destroy, &sub->destroy);
	sub->new_input.notify = new_input_reemit;
	wl_signal_add(&backend->events.new_input, &sub->new_input);
	sub->new_output.notify = new_output_reemit;
	wl_signal_add(&backend->events.new_output, &sub->new_output);

	multi_backend_refresh_features(multi);
	return true;
}
