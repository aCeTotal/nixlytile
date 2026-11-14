#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "platform/input/keyboard/keyboard_interface.h"
#include "platform/log/log.h"
#include "platform/nested/seat/seat.h"

static const struct wl_seat_listener seat_listener;

bool nested_seat_create(struct wl_seat *wl_seat, struct nx_wl_backend *wl,
		uint32_t global_name) {
	struct nx_wl_seat *seat = calloc(1, sizeof(*seat));
	if (!seat) {
		nx_log_errno(NX_ERROR, "Allocation failed");
		return false;
	}
	seat->wl_seat = wl_seat;
	seat->backend = wl;
	seat->global_name = global_name;
	wl_list_insert(&wl->seats, &seat->link);
	wl_seat_add_listener(wl_seat, &seat_listener, seat);
	return true;
}

// nx_keyboard is set up on start.
static void release_keyboard(struct nx_wl_seat *seat) {
	wl_keyboard_release(seat->wl_keyboard);
	if (seat->backend->started) {
		nx_keyboard_finish(&seat->nx_keyboard);
	}
}

void nested_seat_destroy(struct nx_wl_seat *seat) {
	if (seat->wl_pointer) {
		nested_seat_finish_pointer(seat);
	}
	if (seat->wl_keyboard) {
		release_keyboard(seat);
	}

	free(seat->name);
	assert(seat->wl_seat);
	wl_seat_destroy(seat->wl_seat);

	wl_list_remove(&seat->link);
	free(seat);
}

static void add_pointer(struct nx_wl_seat *seat) {
	nx_log(NX_DEBUG, "seat '%s' offering pointer", seat->name);
	seat->wl_pointer = wl_seat_get_pointer(seat->wl_seat);
	nested_seat_init_pointer(seat);
}

static void drop_pointer(struct nx_wl_seat *seat) {
	nx_log(NX_DEBUG, "seat '%s' dropping pointer", seat->name);
	nested_seat_finish_pointer(seat);
}

static void add_keyboard(struct nx_wl_seat *seat) {
	nx_log(NX_DEBUG, "seat '%s' offering keyboard", seat->name);
	seat->wl_keyboard = wl_seat_get_keyboard(seat->wl_seat);
	if (seat->backend->started) {
		nested_keyboard_init(seat);
	}
}

static void drop_keyboard(struct nx_wl_seat *seat) {
	nx_log(NX_DEBUG, "seat '%s' dropping keyboard", seat->name);
	wl_keyboard_release(seat->wl_keyboard);
	nx_keyboard_finish(&seat->nx_keyboard);
	seat->wl_keyboard = NULL;
}

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat,
		enum wl_seat_capability caps) {
	struct nx_wl_seat *seat = data;

	bool has_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
	if (has_pointer && seat->wl_pointer == NULL) {
		add_pointer(seat);
	}
	if (!has_pointer && seat->wl_pointer != NULL) {
		drop_pointer(seat);
	}

	bool has_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (has_keyboard && seat->wl_keyboard == NULL) {
		add_keyboard(seat);
	}
	if (!has_keyboard && seat->wl_keyboard != NULL) {
		drop_keyboard(seat);
	}
}

static void seat_handle_name(void *data, struct wl_seat *wl_seat,
		const char *name) {
	struct nx_wl_seat *seat = data;
	free(seat->name);
	seat->name = strdup(name);
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};
