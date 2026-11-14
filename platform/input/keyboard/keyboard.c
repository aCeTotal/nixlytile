#include <assert.h>
#include <wayland-server-core.h>
#include "platform/container/set.h"
#include "platform/input/device/input_device_interface.h"
#include "platform/input/keyboard/keyboard_interface.h"
#include "platform/input/keyboard/keyboard_state.h"
#include "platform/os/clock.h"

#define DEFAULT_REPEAT_RATE_HZ 25
#define DEFAULT_REPEAT_DELAY_MS 600

struct nx_keyboard *nx_keyboard_from_input_device(
		struct nx_input_device *input_device) {
	assert(input_device->type == NX_INPUT_DEVICE_KEYBOARD);
	return wl_container_of(input_device, (struct nx_keyboard *)NULL, base);
}

void input_keyboard_led_update(struct nx_keyboard *keyboard) {
	if (keyboard->xkb_state == NULL) {
		return;
	}

	uint32_t leds = 0;
	for (uint32_t i = 0; i < NX_LED_COUNT; ++i) {
		if (xkb_state_led_index_is_active(keyboard->xkb_state,
				keyboard->led_indexes[i])) {
			leds |= (1 << i);
		}
	}
	nx_keyboard_led_update(keyboard, leds);
}

bool input_keyboard_modifier_update(struct nx_keyboard *keyboard) {
	if (keyboard->xkb_state == NULL) {
		return false;
	}

	struct nx_keyboard_modifiers modifiers = {
		.depressed = xkb_state_serialize_mods(keyboard->xkb_state,
			XKB_STATE_MODS_DEPRESSED),
		.latched = xkb_state_serialize_mods(keyboard->xkb_state,
			XKB_STATE_MODS_LATCHED),
		.locked = xkb_state_serialize_mods(keyboard->xkb_state,
			XKB_STATE_MODS_LOCKED),
		.group = xkb_state_serialize_layout(keyboard->xkb_state,
			XKB_STATE_LAYOUT_EFFECTIVE),
	};
	if (input_keyboard_modifiers_equal(&modifiers, &keyboard->modifiers)) {
		return false;
	}

	keyboard->modifiers = modifiers;
	return true;
}

void input_keyboard_key_update(struct nx_keyboard *keyboard,
		struct nx_keyboard_key_event *event) {
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		set_add(keyboard->keycodes, &keyboard->num_keycodes,
			NX_KEYBOARD_KEYS_CAP, event->keycode);
	}
	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		set_remove(keyboard->keycodes, &keyboard->num_keycodes,
			NX_KEYBOARD_KEYS_CAP, event->keycode);
	}

	assert(keyboard->num_keycodes <= NX_KEYBOARD_KEYS_CAP);
}

static void update_modifiers_and_leds(struct nx_keyboard *keyboard) {
	if (input_keyboard_modifier_update(keyboard)) {
		wl_signal_emit_mutable(&keyboard->events.modifiers, keyboard);
	}
	input_keyboard_led_update(keyboard);
}

void nx_keyboard_notify_modifiers(struct nx_keyboard *keyboard,
		uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
		uint32_t group) {
	if (keyboard->xkb_state != NULL) {
		xkb_state_update_mask(keyboard->xkb_state, mods_depressed,
			mods_latched, mods_locked, 0, 0, group);
		update_modifiers_and_leds(keyboard);
		return;
	}

	struct nx_keyboard_modifiers modifiers = {
		.depressed = mods_depressed,
		.latched = mods_latched,
		.locked = mods_locked,
		.group = group,
	};
	if (input_keyboard_modifiers_equal(&modifiers, &keyboard->modifiers)) {
		return;
	}
	keyboard->modifiers = modifiers;
	wl_signal_emit_mutable(&keyboard->events.modifiers, keyboard);
}

void nx_keyboard_notify_key(struct nx_keyboard *keyboard,
		struct nx_keyboard_key_event *event) {
	input_keyboard_key_update(keyboard, event);
	wl_signal_emit_mutable(&keyboard->events.key, event);

	if (keyboard->xkb_state == NULL) {
		return;
	}

	if (event->update_state) {
		uint32_t keycode = event->keycode + EVDEV_XKB_OFFSET;
		xkb_state_update_key(keyboard->xkb_state, keycode,
			event->state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP);
	}

	update_modifiers_and_leds(keyboard);
}

void nx_keyboard_init(struct nx_keyboard *kb,
		const struct nx_keyboard_impl *impl, const char *name) {
	*kb = (struct nx_keyboard){
		.impl = impl,
		.keymap_fd = -1,
		.repeat_info.rate = DEFAULT_REPEAT_RATE_HZ,
		.repeat_info.delay = DEFAULT_REPEAT_DELAY_MS,
	};
	nx_input_device_init(&kb->base, NX_INPUT_DEVICE_KEYBOARD, name);

	wl_signal_init(&kb->events.key);
	wl_signal_init(&kb->events.modifiers);
	wl_signal_init(&kb->events.keymap);
	wl_signal_init(&kb->events.repeat_info);
}

void nx_keyboard_finish(struct nx_keyboard *kb) {
	int64_t time_msec = get_current_time_msec();
	while (kb->num_keycodes > 0) {
		struct nx_keyboard_key_event event = {
			.time_msec = time_msec,
			.keycode = kb->keycodes[kb->num_keycodes - 1],
			.update_state = false,
			.state = WL_KEYBOARD_KEY_STATE_RELEASED,
		};
		nx_keyboard_notify_key(kb, &event); // shrinks num_keycodes
	}

	nx_input_device_finish(&kb->base);

	assert(wl_list_empty(&kb->events.key.listener_list));
	assert(wl_list_empty(&kb->events.modifiers.listener_list));
	assert(wl_list_empty(&kb->events.keymap.listener_list));
	assert(wl_list_empty(&kb->events.repeat_info.listener_list));

	input_keyboard_unset_keymap(kb);
}

void nx_keyboard_led_update(struct nx_keyboard *kb, uint32_t leds) {
	if (kb->leds == leds) {
		return;
	}

	kb->leds = leds;

	if (kb->impl && kb->impl->led_update) {
		kb->impl->led_update(kb, leds);
	}
}

void nx_keyboard_set_repeat_info(struct nx_keyboard *kb, int32_t rate,
		int32_t delay) {
	if (kb->repeat_info.rate == rate && kb->repeat_info.delay == delay) {
		return;
	}
	kb->repeat_info.rate = rate;
	kb->repeat_info.delay = delay;
	wl_signal_emit_mutable(&kb->events.repeat_info, kb);
}

uint32_t nx_keyboard_get_modifiers(struct nx_keyboard *kb) {
	xkb_mod_mask_t mask = kb->modifiers.depressed | kb->modifiers.latched;
	uint32_t modifiers = 0;
	for (size_t i = 0; i < NX_MODIFIER_COUNT; ++i) {
		if (kb->mod_indexes[i] != XKB_MOD_INVALID &&
				(mask & (1 << kb->mod_indexes[i]))) {
			modifiers |= (1 << i);
		}
	}
	return modifiers;
}
