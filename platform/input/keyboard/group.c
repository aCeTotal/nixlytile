#include <stdlib.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include "platform/input/keyboard/group.h"
#include "platform/input/keyboard/keyboard_interface.h"
#include "platform/input/keyboard/keyboard_state.h"
#include "platform/log/log.h"

struct keyboard_group_device {
	struct nx_keyboard *keyboard;
	struct wl_listener key;
	struct wl_listener modifiers;
	struct wl_listener keymap;
	struct wl_listener repeat_info;
	struct wl_listener destroy;
	struct wl_list link; // nx_keyboard_group.devices
};

// A keycode held by count members.
struct keyboard_group_key {
	uint32_t keycode;
	size_t count;
	struct wl_list link; // nx_keyboard_group.keys
};

static void keyboard_set_leds(struct nx_keyboard *kb, uint32_t leds) {
	struct nx_keyboard_group *group = wl_container_of(kb, group, keyboard);
	struct keyboard_group_device *device;
	wl_list_for_each(device, &group->devices, link) {
		nx_keyboard_led_update(device->keyboard, leds);
	}
}

static const struct nx_keyboard_impl impl = {
	.name = "keyboard-group",
	.led_update = keyboard_set_leds
};

struct nx_keyboard_group *nx_keyboard_group_create(void) {
	struct nx_keyboard_group *group = calloc(1, sizeof(*group));
	if (!group) {
		nx_log(NX_ERROR, "Failed to allocate nx_keyboard_group");
		return NULL;
	}

	nx_keyboard_init(&group->keyboard, &impl, "nx_keyboard_group");
	wl_list_init(&group->devices);
	wl_list_init(&group->keys);

	return group;
}

static struct keyboard_group_key *find_key(struct nx_keyboard_group *group,
		uint32_t keycode) {
	struct keyboard_group_key *key;
	wl_list_for_each(key, &group->keys, link) {
		if (key->keycode == keycode) {
			return key;
		}
	}
	return NULL;
}

// False while still held elsewhere.
static bool update_held_key(struct keyboard_group_key *key,
		enum wl_keyboard_key_state state) {
	switch (state) {
	case WL_KEYBOARD_KEY_STATE_PRESSED:
		key->count++;
		return false;
	case WL_KEYBOARD_KEY_STATE_RELEASED:
		key->count--;
		if (key->count > 0) {
			return false;
		}
		wl_list_remove(&key->link);
		free(key);
		return true;
	default:
		return true;
	}
}

// True if group state changes.
static bool process_key(struct keyboard_group_device *group_device,
		struct nx_keyboard_key_event *event) {
	struct nx_keyboard_group *group = group_device->keyboard->group;

	struct keyboard_group_key *key = find_key(group, event->keycode);
	if (key != NULL) {
		return update_held_key(key, event->state);
	}
	if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return true;
	}

	key = calloc(1, sizeof(*key));
	if (!key) {
		nx_log(NX_ERROR, "Failed to allocate keyboard_group_key");
		return false;
	}
	key->keycode = event->keycode;
	key->count = 1;
	wl_list_insert(&group->keys, &key->link);
	return true;
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
	struct keyboard_group_device *group_device =
		wl_container_of(listener, group_device, key);
	if (process_key(group_device, data)) {
		nx_keyboard_notify_key(&group_device->keyboard->group->keyboard, data);
	}
}

// Sync members first, group last.
static void handle_keyboard_modifiers(struct wl_listener *listener,
		void *data) {
	struct keyboard_group_device *group_device =
		wl_container_of(listener, group_device, modifiers);
	struct nx_keyboard_modifiers mods = group_device->keyboard->modifiers;

	struct keyboard_group_device *device;
	wl_list_for_each(device, &group_device->keyboard->group->devices, link) {
		if (input_keyboard_modifiers_equal(&mods, &device->keyboard->modifiers)) {
			continue;
		}
		nx_keyboard_notify_modifiers(device->keyboard,
			mods.depressed, mods.latched, mods.locked, mods.group);
		return;
	}

	nx_keyboard_notify_modifiers(&group_device->keyboard->group->keyboard,
		mods.depressed, mods.latched, mods.locked, mods.group);
}

static void handle_keyboard_keymap(struct wl_listener *listener, void *data) {
	struct keyboard_group_device *group_device =
		wl_container_of(listener, group_device, keymap);
	struct nx_keyboard *keyboard = group_device->keyboard;
	struct nx_keyboard_group *group = keyboard->group;

	if (nx_keyboard_keymaps_match(group->keyboard.keymap, keyboard->keymap)) {
		nx_keyboard_set_keymap(&group->keyboard, keyboard->keymap);
		return;
	}

	struct keyboard_group_device *device;
	wl_list_for_each(device, &group->devices, link) {
		if (nx_keyboard_keymaps_match(keyboard->keymap,
				device->keyboard->keymap)) {
			continue;
		}
		nx_keyboard_set_keymap(device->keyboard, keyboard->keymap);
		return;
	}

	nx_keyboard_set_keymap(&group->keyboard, keyboard->keymap);
}

static void handle_keyboard_repeat_info(struct wl_listener *listener,
		void *data) {
	struct keyboard_group_device *group_device =
		wl_container_of(listener, group_device, repeat_info);
	struct nx_keyboard *keyboard = group_device->keyboard;

	struct keyboard_group_device *device;
	wl_list_for_each(device, &keyboard->group->devices, link) {
		struct nx_keyboard *devkb = device->keyboard;
		if (devkb->repeat_info.rate == keyboard->repeat_info.rate &&
				devkb->repeat_info.delay == keyboard->repeat_info.delay) {
			continue;
		}
		nx_keyboard_set_repeat_info(devkb, keyboard->repeat_info.rate,
			keyboard->repeat_info.delay);
		return;
	}

	nx_keyboard_set_repeat_info(&keyboard->group->keyboard,
		keyboard->repeat_info.rate, keyboard->repeat_info.delay);
}

// Group applies device's unique keys.
static void refresh_state(struct keyboard_group_device *device,
		enum wl_keyboard_key_state state) {
	struct nx_keyboard *group_kb = &device->keyboard->group->keyboard;
	for (size_t i = 0; i < device->keyboard->num_keycodes; i++) {
		struct nx_keyboard_key_event event = {
			.keycode = device->keyboard->keycodes[i],
			.state = state,
		};
		if (!process_key(device, &event)) {
			continue;
		}
		input_keyboard_key_update(group_kb, &event);
		input_keyboard_modifier_update(group_kb);
		input_keyboard_led_update(group_kb);
	}
}

static void remove_keyboard_group_device(struct keyboard_group_device *device) {
	refresh_state(device, WL_KEYBOARD_KEY_STATE_RELEASED);
	device->keyboard->group = NULL;
	wl_list_remove(&device->link);
	wl_list_remove(&device->key.link);
	wl_list_remove(&device->modifiers.link);
	wl_list_remove(&device->keymap.link);
	wl_list_remove(&device->repeat_info.link);
	wl_list_remove(&device->destroy.link);
	free(device);
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
	struct keyboard_group_device *device =
		wl_container_of(listener, device, destroy);
	remove_keyboard_group_device(device);
}

static void sync_with_group(struct nx_keyboard *keyboard,
		struct nx_keyboard *group_kb) {
	if (keyboard->modifiers.group != group_kb->modifiers.group) {
		nx_keyboard_notify_modifiers(keyboard, keyboard->modifiers.depressed,
			keyboard->modifiers.latched, keyboard->modifiers.locked,
			group_kb->modifiers.group);
	}
	if (keyboard->repeat_info.rate != group_kb->repeat_info.rate ||
			keyboard->repeat_info.delay != group_kb->repeat_info.delay) {
		nx_keyboard_set_repeat_info(keyboard, group_kb->repeat_info.rate,
			group_kb->repeat_info.delay);
	}
}

bool nx_keyboard_group_add_keyboard(struct nx_keyboard_group *group,
		struct nx_keyboard *keyboard) {
	if (keyboard->group) {
		nx_log(NX_ERROR, "A nx_keyboard can only belong to one group");
		return false;
	}

	if (keyboard->impl == &impl) {
		nx_log(NX_ERROR, "Cannot add a group's keyboard to a group");
		return false;
	}

	if (!nx_keyboard_keymaps_match(group->keyboard.keymap, keyboard->keymap)) {
		nx_log(NX_ERROR, "Device keymap does not match keyboard group's");
		return false;
	}

	struct keyboard_group_device *device = calloc(1, sizeof(*device));
	if (!device) {
		nx_log(NX_ERROR, "Failed to allocate keyboard_group_device");
		return false;
	}

	device->keyboard = keyboard;
	keyboard->group = group;
	wl_list_insert(&group->devices, &device->link);

	wl_signal_add(&keyboard->events.key, &device->key);
	device->key.notify = handle_keyboard_key;

	wl_signal_add(&keyboard->events.modifiers, &device->modifiers);
	device->modifiers.notify = handle_keyboard_modifiers;

	wl_signal_add(&keyboard->events.keymap, &device->keymap);
	device->keymap.notify = handle_keyboard_keymap;

	wl_signal_add(&keyboard->events.repeat_info, &device->repeat_info);
	device->repeat_info.notify = handle_keyboard_repeat_info;

	wl_signal_add(&keyboard->base.events.destroy, &device->destroy);
	device->destroy.notify = handle_keyboard_destroy;

	sync_with_group(keyboard, &group->keyboard);

	refresh_state(device, WL_KEYBOARD_KEY_STATE_PRESSED);
	return true;
}

void nx_keyboard_group_destroy(struct nx_keyboard_group *group) {
	struct keyboard_group_device *device, *next_device;
	wl_list_for_each_safe(device, next_device, &group->devices, link) {
		remove_keyboard_group_device(device);
	}

	// Duplicate presses leave stale keys.
	struct keyboard_group_key *key, *next_key;
	wl_list_for_each_safe(key, next_key, &group->keys, link) {
		wl_list_remove(&key->link);
		free(key);
	}

	nx_keyboard_finish(&group->keyboard);

	free(group);
}
