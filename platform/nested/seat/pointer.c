#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/input/pointer/pointer_interface.h"
#include "platform/log/log.h"
#include "platform/nested/output/output.h"
#include "platform/nested/seat/seat.h"

#include "relative-pointer-unstable-v1-client-protocol.h"

static const struct nx_pointer_impl pointer_impl = {
	.name = "wl-pointer",
};

struct nx_wl_pointer *nested_pointer_for_output(struct nx_wl_output *output,
		const struct wl_pointer *wl_pointer) {
	struct nx_wl_seat *seat;
	wl_list_for_each(seat, &output->backend->seats, link) {
		if (seat->wl_pointer != wl_pointer) {
			continue;
		}
		struct nx_wl_pointer *pointer;
		wl_list_for_each(pointer, &seat->pointers, link) {
			if (pointer->output == output) {
				return pointer;
			}
		}
	}
	return NULL;
}

static void destroy_pointer(struct nx_wl_pointer *pointer) {
	if (pointer->output->cursor.pointer == pointer) {
		pointer->output->cursor.pointer = NULL;
	}
	if (pointer->seat->active_pointer == pointer) {
		pointer->seat->active_pointer = NULL;
	}

	nx_pointer_finish(&pointer->nx_pointer);
	wl_list_remove(&pointer->output_destroy.link);
	wl_list_remove(&pointer->link);
	free(pointer);
}

static void pointer_handle_output_destroy(struct wl_listener *listener, void *data) {
	struct nx_wl_pointer *pointer =
		wl_container_of(listener, pointer, output_destroy);
	destroy_pointer(pointer);
}

void nested_pointer_create(struct nx_wl_seat *seat, struct nx_wl_output *output) {
	assert(seat->wl_pointer);

	if (nested_pointer_for_output(output, seat->wl_pointer)) {
		nx_log(NX_DEBUG, "pointer for output '%s' from seat '%s' already exists",
			output->nx_output.name, seat->name);
		return;
	}
	nx_log(NX_DEBUG, "creating pointer for output '%s' from seat '%s'",
		output->nx_output.name, seat->name);

	struct nx_wl_pointer *pointer = calloc(1, sizeof(*pointer));
	if (pointer == NULL) {
		nx_log(NX_ERROR, "failed to allocate nx_wl_pointer");
		return;
	}

	char name[64] = {0};
	snprintf(name, sizeof(name), "wayland-pointer-%s", seat->name);
	nx_pointer_init(&pointer->nx_pointer, &pointer_impl, name);
	pointer->nx_pointer.output_name = strdup(output->nx_output.name);

	pointer->seat = seat;
	pointer->output = output;

	pointer->output_destroy.notify = pointer_handle_output_destroy;
	wl_signal_add(&output->nx_output.events.destroy, &pointer->output_destroy);

	wl_signal_emit_mutable(&seat->backend->backend.events.new_input,
		&pointer->nx_pointer.base);

	wl_list_insert(&seat->pointers, &pointer->link);
}

void nested_seat_init_pointer(struct nx_wl_seat *seat) {
	assert(seat->wl_pointer);
	struct nx_wl_backend *backend = seat->backend;

	wl_list_init(&seat->pointers);
	struct nx_wl_output *output;
	wl_list_for_each(output, &backend->outputs, link) {
		nested_pointer_create(seat, output);
	}

	nested_gestures_init(seat);

	if (backend->zwp_relative_pointer_manager_v1) {
		seat->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(
			backend->zwp_relative_pointer_manager_v1, seat->wl_pointer);
		zwp_relative_pointer_v1_add_listener(seat->relative_pointer,
			&nested_relative_pointer_listener, seat);
	}

	wl_pointer_add_listener(seat->wl_pointer, &nested_pointer_listener, seat);
}

void nested_seat_finish_pointer(struct nx_wl_seat *seat) {
	assert(seat->wl_pointer);

	wl_pointer_release(seat->wl_pointer);

	struct nx_wl_pointer *pointer, *tmp;
	wl_list_for_each_safe(pointer, tmp, &seat->pointers, link) {
		destroy_pointer(pointer);
	}

	nested_gestures_finish(seat);
	if (seat->relative_pointer != NULL) {
		zwp_relative_pointer_v1_destroy(seat->relative_pointer);
	}

	seat->wl_pointer = NULL;
	seat->active_pointer = NULL;
}
