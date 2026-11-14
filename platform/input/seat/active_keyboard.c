#include <fcntl.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "platform/input/seat/client.h"
#include "platform/log/log.h"

void seat_client_send_keymap(struct nx_seat_client *client,
		struct nx_keyboard *keyboard) {
	enum wl_keyboard_keymap_format format = WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1;
	int fd = keyboard->keymap_fd;
	uint32_t size = keyboard->keymap_size;
	int devnull = -1;
	if (keyboard->keymap == NULL) {
		// Clients still need an fd.
		devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (devnull < 0) {
			nx_log_errno(NX_ERROR, "Failed to open /dev/null");
			return;
		}
		format = WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP;
		fd = devnull;
		size = 0;
	}

	struct wl_resource *resource;
	wl_resource_for_each(resource, &client->keyboards) {
		if (seat_client_from_keyboard_resource(resource) == NULL) {
			continue;
		}
		wl_keyboard_send_keymap(resource, format, fd, size);
	}
	if (devnull >= 0) {
		close(devnull);
	}
}

void seat_client_send_repeat_info(struct nx_seat_client *client,
		struct nx_keyboard *keyboard) {
	struct wl_resource *resource;
	wl_resource_for_each(resource, &client->keyboards) {
		if (seat_client_from_keyboard_resource(resource) == NULL ||
				wl_resource_get_version(resource) <
				WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION) {
			continue;
		}
		wl_keyboard_send_repeat_info(resource,
			keyboard->repeat_info.rate, keyboard->repeat_info.delay);
	}
}

static void handle_keyboard_keymap(struct wl_listener *listener, void *data) {
	struct nx_seat_keyboard_state *state =
		wl_container_of(listener, state, keyboard_keymap);
	struct nx_seat_client *client;
	wl_list_for_each(client, &state->seat->clients, link) {
		seat_client_send_keymap(client, state->keyboard);
	}
}

static void handle_keyboard_repeat_info(struct wl_listener *listener,
		void *data) {
	struct nx_seat_keyboard_state *state =
		wl_container_of(listener, state, keyboard_repeat_info);
	struct nx_seat_client *client;
	wl_list_for_each(client, &state->seat->clients, link) {
		seat_client_send_repeat_info(client, state->keyboard);
	}
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
	struct nx_seat_keyboard_state *state =
		wl_container_of(listener, state, keyboard_destroy);
	nx_seat_set_keyboard(state->seat, NULL);
}

static void watch_keyboard(struct nx_seat_keyboard_state *state,
		struct nx_keyboard *keyboard) {
	wl_signal_add(&keyboard->base.events.destroy, &state->keyboard_destroy);
	state->keyboard_destroy.notify = handle_keyboard_destroy;
	wl_signal_add(&keyboard->events.keymap, &state->keyboard_keymap);
	state->keyboard_keymap.notify = handle_keyboard_keymap;
	wl_signal_add(&keyboard->events.repeat_info, &state->keyboard_repeat_info);
	state->keyboard_repeat_info.notify = handle_keyboard_repeat_info;
}

static void unwatch_keyboard(struct nx_seat_keyboard_state *state) {
	wl_list_remove(&state->keyboard_destroy.link);
	wl_list_remove(&state->keyboard_keymap.link);
	wl_list_remove(&state->keyboard_repeat_info.link);
}

void nx_seat_set_keyboard(struct nx_seat *seat, struct nx_keyboard *keyboard) {
	struct nx_seat_keyboard_state *state = &seat->keyboard_state;
	if (state->keyboard == keyboard) {
		return;
	}

	bool keymap_changed = state->keyboard == NULL || keyboard == NULL ||
		state->keyboard->keymap != keyboard->keymap;
	if (state->keyboard != NULL) {
		unwatch_keyboard(state);
	}
	state->keyboard = keyboard;
	if (keyboard == NULL) {
		return;
	}

	watch_keyboard(state, keyboard);
	struct nx_seat_client *client;
	wl_list_for_each(client, &seat->clients, link) {
		if (keymap_changed) {
			seat_client_send_keymap(client, keyboard);
		}
		seat_client_send_repeat_info(client, keyboard);
	}
	nx_seat_keyboard_send_modifiers(seat, &keyboard->modifiers);
}

struct nx_keyboard *nx_seat_get_keyboard(struct nx_seat *seat) {
	return seat->keyboard_state.keyboard;
}
