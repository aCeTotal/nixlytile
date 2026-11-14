#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include "platform/input/keyboard/keyboard_state.h"
#include "platform/log/log.h"
#include "platform/os/shm.h"

static const char *const led_names[NX_LED_COUNT] = {
	XKB_LED_NAME_NUM,
	XKB_LED_NAME_CAPS,
	XKB_LED_NAME_SCROLL,
	XKB_LED_NAME_COMPOSE,
	XKB_LED_NAME_KANA,
};

// Indexed like enum nx_keyboard_modifier.
static const char *const mod_names[NX_MODIFIER_COUNT] = {
	XKB_MOD_NAME_SHIFT,
	XKB_MOD_NAME_CAPS,
	XKB_MOD_NAME_CTRL,
	XKB_MOD_NAME_ALT,
	XKB_MOD_NAME_NUM,
	"Mod3",
	XKB_MOD_NAME_LOGO,
	"Mod5",
};

void input_keyboard_unset_keymap(struct nx_keyboard *kb) {
	xkb_keymap_unref(kb->keymap);
	kb->keymap = NULL;
	xkb_state_unref(kb->xkb_state);
	kb->xkb_state = NULL;
	kb->keymap_size = 0;
	if (kb->keymap_fd >= 0) {
		close(kb->keymap_fd);
	}
	kb->keymap_fd = -1;
}

// Read-only shm keymap copy, or -1.
static int keymap_shm_fd(const char *keymap_str, size_t keymap_size) {
	int rw_fd = -1, ro_fd = -1;
	if (!allocate_shm_file_pair(keymap_size, &rw_fd, &ro_fd)) {
		nx_log(NX_ERROR, "Failed to allocate shm file for keymap");
		return -1;
	}

	void *dst = mmap(NULL, keymap_size, PROT_READ | PROT_WRITE, MAP_SHARED, rw_fd, 0);
	close(rw_fd);
	if (dst == MAP_FAILED) {
		nx_log_errno(NX_ERROR, "mmap failed");
		close(ro_fd);
		return -1;
	}

	memcpy(dst, keymap_str, keymap_size);
	munmap(dst, keymap_size);
	return ro_fd;
}

static void apply_keymap(struct nx_keyboard *kb, struct xkb_keymap *keymap,
		struct xkb_state *xkb_state) {
	kb->keymap = xkb_keymap_ref(keymap);
	kb->xkb_state = xkb_state;

	for (size_t i = 0; i < NX_LED_COUNT; ++i) {
		kb->led_indexes[i] = xkb_map_led_get_index(kb->keymap, led_names[i]);
	}
	for (size_t i = 0; i < NX_MODIFIER_COUNT; ++i) {
		kb->mod_indexes[i] = xkb_map_mod_get_index(kb->keymap, mod_names[i]);
	}

	for (size_t i = 0; i < kb->num_keycodes; ++i) {
		xkb_keycode_t keycode = kb->keycodes[i] + EVDEV_XKB_OFFSET;
		xkb_state_update_key(kb->xkb_state, keycode, XKB_KEY_DOWN);
	}

	input_keyboard_modifier_update(kb);
}

bool nx_keyboard_set_keymap(struct nx_keyboard *kb, struct xkb_keymap *keymap) {
	if (keymap == NULL) {
		input_keyboard_unset_keymap(kb);
		wl_signal_emit_mutable(&kb->events.keymap, kb);
		return true;
	}

	struct xkb_state *xkb_state = xkb_state_new(keymap);
	if (xkb_state == NULL) {
		nx_log(NX_ERROR, "Failed to create XKB state");
		return false;
	}

	char *keymap_str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (keymap_str == NULL) {
		nx_log(NX_ERROR, "Failed to get string version of keymap");
		xkb_state_unref(xkb_state);
		return false;
	}
	size_t keymap_size = strlen(keymap_str) + 1;
	int keymap_fd = keymap_shm_fd(keymap_str, keymap_size);
	free(keymap_str);
	if (keymap_fd < 0) {
		xkb_state_unref(xkb_state);
		return false;
	}

	input_keyboard_unset_keymap(kb);
	kb->keymap_size = keymap_size;
	kb->keymap_fd = keymap_fd;
	apply_keymap(kb, keymap, xkb_state);

	wl_signal_emit_mutable(&kb->events.keymap, kb);
	return true;
}

bool nx_keyboard_keymaps_match(struct xkb_keymap *km1,
		struct xkb_keymap *km2) {
	if (!km1 && !km2) {
		return true;
	}
	if (!km1 || !km2) {
		return false;
	}
	char *km1_str = xkb_keymap_get_as_string(km1, XKB_KEYMAP_FORMAT_TEXT_V1);
	char *km2_str = xkb_keymap_get_as_string(km2, XKB_KEYMAP_FORMAT_TEXT_V1);
	bool match = strcmp(km1_str, km2_str) == 0;
	free(km1_str);
	free(km2_str);
	return match;
}
