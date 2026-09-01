/* input_conf.c — pointer and keyboard settings from /etc/nixlyos/input.conf.
 *
 * nixlycc's Mouse & Keyboard page owns modules/core/input.nix, which writes
 * that file through environment.etc. A nixos-rebuild switch replaces it, and
 * the inotify watch here picks the new values up and re-applies them to every
 * device already open — the settings land without logging out.
 *
 * Format is one key=value per line:
 *
 *   accel-profile=flat
 *   accel-speed=0.25
 *   natural-scroll=false
 *   left-handed=false
 *   middle-button-emulation=false
 *   scroll-method=wheel        # wheel keeps the device default
 *   button-map=lrm
 *   keyboard-layout=no
 *   keyboard-variant=
 *   repeat-delay=300
 *   repeat-rate=100
 *
 * Anything the file leaves out keeps the value the KDL config set.
 */

#include "nixlytile.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define INPUTCONF_DIR  "/etc/nixlyos"
#define INPUTCONF_NAME "input.conf"
#define INPUTCONF_PATH INPUTCONF_DIR "/" INPUTCONF_NAME

static int inputconf_fd = -1;
static struct wl_event_source *inputconf_source;

/* xkb_rule_names keeps the pointers, so the strings have to outlive the
 * parse. */
static char inputconf_layout[64];
static char inputconf_variant[64];

/* "wheel" means the device decides; only the other three are pushed. */
static int inputconf_set_scroll;

static int
parse_bool(const char *value)
{
	return strcmp(value, "true") == 0 || strcmp(value, "1") == 0;
}

static void
apply_line(const char *key, const char *value)
{
	if (strcmp(key, "accel-profile") == 0) {
		accel_profile = strcmp(value, "flat") == 0
			? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
			: LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
	} else if (strcmp(key, "accel-speed") == 0) {
		accel_speed = atof(value);
	} else if (strcmp(key, "natural-scroll") == 0) {
		natural_scrolling = parse_bool(value);
	} else if (strcmp(key, "left-handed") == 0) {
		left_handed = parse_bool(value);
	} else if (strcmp(key, "middle-button-emulation") == 0) {
		middle_button_emulation = parse_bool(value);
	} else if (strcmp(key, "scroll-method") == 0) {
		inputconf_set_scroll = 1;
		if (strcmp(value, "button") == 0)
			scroll_method = LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
		else if (strcmp(value, "edge") == 0)
			scroll_method = LIBINPUT_CONFIG_SCROLL_EDGE;
		else if (strcmp(value, "no-scroll") == 0)
			scroll_method = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
		else
			inputconf_set_scroll = 0; /* wheel — leave the device alone */
	} else if (strcmp(key, "button-map") == 0) {
		button_map = strcmp(value, "lmr") == 0
			? LIBINPUT_CONFIG_TAP_MAP_LMR
			: LIBINPUT_CONFIG_TAP_MAP_LRM;
	} else if (strcmp(key, "keyboard-layout") == 0) {
		snprintf(inputconf_layout, sizeof(inputconf_layout), "%.63s", value);
	} else if (strcmp(key, "keyboard-variant") == 0) {
		snprintf(inputconf_variant, sizeof(inputconf_variant), "%.63s", value);
	} else if (strcmp(key, "repeat-delay") == 0) {
		repeat_delay = atoi(value);
	} else if (strcmp(key, "repeat-rate") == 0) {
		repeat_rate = atoi(value);
	}
}

/* Pushes the current globals onto every pointer that is already open. */
static void
apply_to_pointers(void)
{
	int i;

	pthread_mutex_lock(&pointer_devices_lock);
	for (i = 0; i < pointer_device_count; i++) {
		struct libinput_device *device = pointer_devices[i];
		if (!device)
			continue;

		if (libinput_device_config_scroll_has_natural_scroll(device))
			libinput_device_config_scroll_set_natural_scroll_enabled(device,
				natural_scrolling);
		if (libinput_device_config_left_handed_is_available(device))
			libinput_device_config_left_handed_set(device, left_handed);
		if (libinput_device_config_middle_emulation_is_available(device))
			libinput_device_config_middle_emulation_set_enabled(device,
				middle_button_emulation);
		if (inputconf_set_scroll && libinput_device_config_scroll_get_methods(device)
				!= LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method(device, scroll_method);
		if (libinput_device_config_tap_get_finger_count(device))
			libinput_device_config_tap_set_button_map(device, button_map);
		if (libinput_device_config_accel_is_available(device)) {
			libinput_device_config_accel_set_profile(device, accel_profile);
			libinput_device_config_accel_set_speed(device, accel_speed);
		}
	}
	pthread_mutex_unlock(&pointer_devices_lock);
}

/* Recompiles the keymap for the shared keyboard group and refreshes repeat. */
static void
apply_to_keyboards(void)
{
	struct xkb_context *context;
	struct xkb_keymap *keymap;
	struct xkb_rule_names names;

	if (!kb_group)
		return;

	wlr_keyboard_set_repeat_info(&kb_group->wlr_group->keyboard, repeat_rate, repeat_delay);

	if (!inputconf_layout[0])
		return;

	runtime_xkb_rules.layout = inputconf_layout;
	runtime_xkb_rules.variant = inputconf_variant[0] ? inputconf_variant : NULL;
	runtime_xkb_rules_set = 1;

	/* Skip the recompile when the effective tuple is unchanged — a
	 * keymap compile parses the whole XKB rules tree from disk
	 * (30-100 ms on the compositor thread), and a NixOS /etc swap
	 * rewrites input.conf on every rebuild even when identical. */
	{
		static char last_tuple[512];
		char tuple[512];

		names = getxkbrules();
		snprintf(tuple, sizeof(tuple), "%s\x1f%s\x1f%s\x1f%s\x1f%s",
			names.rules ? names.rules : "",
			names.model ? names.model : "",
			names.layout ? names.layout : "",
			names.variant ? names.variant : "",
			names.options ? names.options : "");
		if (strcmp(tuple, last_tuple) == 0)
			return;
		snprintf(last_tuple, sizeof(last_tuple), "%s", tuple);
	}

	if (!(context = xkb_context_new(XKB_CONTEXT_NO_FLAGS)))
		return;
	keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap) {
		wlr_keyboard_set_keymap(&kb_group->wlr_group->keyboard, keymap);
		xkb_keymap_unref(keymap);
	} else {
		wlr_log(WLR_ERROR, "input_conf: layout \"%s\" did not compile", inputconf_layout);
	}
	xkb_context_unref(context);
}

void
input_conf_update(void)
{
	char line[256];
	FILE *fp = fopen(INPUTCONF_PATH, "r");

	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		char *nl = strchr(line, '\n');
		char *eq;

		if (nl)
			*nl = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;
		if (!(eq = strchr(line, '=')))
			continue;
		*eq = '\0';
		apply_line(line, eq + 1);
	}
	fclose(fp);

	apply_to_pointers();
	apply_to_keyboards();
	wlr_log(WLR_INFO, "input_conf: applied %s", INPUTCONF_PATH);
}

static int
inputconf_readable(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;
	int relevant = 0;
	(void)mask;
	(void)data;

	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		char *p = buf;
		while (p < buf + len) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if (ev->len && strcmp(ev->name, INPUTCONF_NAME) == 0)
				relevant = 1;
			p += sizeof(*ev) + ev->len;
		}
	}

	if (relevant)
		input_conf_update();
	return 0;
}

void
setup_input_conf_watch(void)
{
	int wd;

	input_conf_update();

	inputconf_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (inputconf_fd < 0) {
		wlr_log(WLR_ERROR, "input_conf: inotify_init failed: %s", strerror(errno));
		return;
	}

	/* /etc is swapped wholesale by a switch, so watch the directory rather
	 * than the file — the old inode goes away. */
	wd = inotify_add_watch(inputconf_fd, INPUTCONF_DIR,
		IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF);
	if (wd < 0) {
		wlr_log(WLR_INFO, "input_conf: no %s yet (%s)", INPUTCONF_DIR, strerror(errno));
		close(inputconf_fd);
		inputconf_fd = -1;
		return;
	}

	inputconf_source = wl_event_loop_add_fd(event_loop, inputconf_fd, WL_EVENT_READABLE,
		inputconf_readable, NULL);
	if (!inputconf_source) {
		close(inputconf_fd);
		inputconf_fd = -1;
		return;
	}
	wlr_log(WLR_INFO, "input_conf: watching %s", INPUTCONF_PATH);
}
