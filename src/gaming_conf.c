/* gaming_conf.c — ~/.local/nixlyos/gaming.conf hot-reload (push-to-talk).
 *
 * The file is written by nixlycc (Gaming page).  Format, one key=value per
 * line:
 *
 *   ptt-bind=ctrl+key:0x6d      keysym in hex (layout-safe, from xkb)
 *   ptt-bind=mouse:275          evdev button code (275 = BTN_SIDE)
 *   ptt-bind=alt+f13            keysym by name also accepted
 *   ptt-label=Ctrl+M            display text, ignored here
 *
 * Push-to-talk: the microphone is unmuted while the bound key/button is
 * held and muted again the moment it is released.  The press is matched
 * before any lock/shortcut-inhibitor checks in input.c so it works inside
 * fullscreen games, and the event is still forwarded to the client.  The
 * release is tracked by keycode/button, so letting go of the modifier
 * first cannot leave the mic open.
 *
 * The mic is also force-muted at startup (with retries, since PipeWire
 * usually comes up after the compositor).  An inotify watch on the
 * ~/.local/nixlyos directory reloads the bind whenever nixlycc rewrites
 * the file.
 */

#include "nixlytile.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define GAMINGCONF_NAME "gaming.conf"

/* Parsed bind.  Either keysym or button is set, never both. */
static uint32_t ptt_mods;
static xkb_keysym_t ptt_keysym;   /* XKB_KEY_NoSymbol when unset */
static uint32_t ptt_button;       /* 0 when unset */

/* Held state: the exact keycode/button that armed PTT, so release
 * matches even after the modifiers were dropped. */
static int ptt_key_held;
static uint32_t ptt_held_keycode;
static int ptt_button_held;
static uint32_t ptt_held_button;

static char gamingconf_dir[PATH_MAX];
static char gamingconf_path[PATH_MAX];
static int gamingconf_fd = -1;
static struct wl_event_source *gamingconf_source;

/* PipeWire comes up after nixlytile; retry the startup mute until the
 * grace period is over. */
static struct wl_event_source *startmute_timer;
static int startmute_tries;
static const int startmute_delays_ms[] = { 2000, 4000, 8000 };

static void
gamingconf_resolve_paths(void)
{
	const char *home = getenv("HOME");
	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		if (pw)
			home = pw->pw_dir;
	}
	if (!home)
		home = "/";
	snprintf(gamingconf_dir, sizeof(gamingconf_dir),
		"%s/.local/nixlyos", home);
	snprintf(gamingconf_path, sizeof(gamingconf_path),
		"%s/.local/nixlyos/" GAMINGCONF_NAME, home);
}

/* Mute/unmute plus the optimistic statusbar refresh the click handler
 * uses: cache the new state and stamp the read time so refreshstatusmic
 * shows it immediately instead of re-reading stale PipeWire state. */
static void
ptt_set_mute(int mute)
{
	set_pipewire_mic_mute(mute);
	mic_last_read_ms = monotonic_msec();
	refreshstatusmic();
}

static void
parse_bind(const char *value)
{
	char buf[128];
	char *tok, *save = NULL;
	uint32_t mods = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;
	uint32_t button = 0;

	snprintf(buf, sizeof(buf), "%s", value);

	for (tok = strtok_r(buf, "+", &save); tok;
			tok = strtok_r(NULL, "+", &save)) {
		if (strcmp(tok, "ctrl") == 0)
			mods |= WLR_MODIFIER_CTRL;
		else if (strcmp(tok, "alt") == 0)
			mods |= WLR_MODIFIER_ALT;
		else if (strcmp(tok, "shift") == 0)
			mods |= WLR_MODIFIER_SHIFT;
		else if (strcmp(tok, "super") == 0)
			mods |= WLR_MODIFIER_LOGO;
		else if (strncmp(tok, "mouse:", 6) == 0)
			button = (uint32_t)strtoul(tok + 6, NULL, 0);
		else if (strncmp(tok, "key:", 4) == 0)
			sym = (xkb_keysym_t)strtoul(tok + 4, NULL, 0);
		else
			sym = xkb_keysym_from_name(tok,
				XKB_KEYSYM_CASE_INSENSITIVE);
	}

	if (button) {
		ptt_button = button;
		ptt_keysym = XKB_KEY_NoSymbol;
	} else if (sym != XKB_KEY_NoSymbol) {
		ptt_keysym = xkb_keysym_to_lower(sym);
		ptt_button = 0;
	}
	ptt_mods = mods;
}

static void
gaming_conf_load(void)
{
	char line[256];
	FILE *fp;

	/* A removed or rewritten bind must never leave the mic open. */
	if (ptt_key_held || ptt_button_held) {
		ptt_key_held = ptt_button_held = 0;
		ptt_set_mute(1);
	}
	ptt_mods = 0;
	ptt_keysym = XKB_KEY_NoSymbol;
	ptt_button = 0;

	fp = fopen(gamingconf_path, "r");
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
		if (strcmp(line, "ptt-bind") == 0)
			parse_bind(eq + 1);
	}
	fclose(fp);

	if (ptt_keysym != XKB_KEY_NoSymbol)
		wlr_log(WLR_INFO, "gaming.conf: push-to-talk on keysym 0x%x mods 0x%x",
			ptt_keysym, ptt_mods);
	else if (ptt_button)
		wlr_log(WLR_INFO, "gaming.conf: push-to-talk on button %u mods 0x%x",
			ptt_button, ptt_mods);
}

void
ptt_handle_key(uint32_t mods, uint32_t keycode, const xkb_keysym_t *syms,
	int nsyms, const xkb_keysym_t *level0_syms, int nlevel0, int pressed)
{
	int i;

	if (!pressed) {
		if (ptt_key_held && keycode == ptt_held_keycode) {
			ptt_key_held = 0;
			ptt_set_mute(1);
		}
		return;
	}

	if (ptt_keysym == XKB_KEY_NoSymbol || ptt_key_held)
		return;
	if (CLEANMASK(mods) != CLEANMASK(ptt_mods))
		return;

	for (i = 0; i < nsyms + nlevel0; i++) {
		xkb_keysym_t sym = i < nsyms ? syms[i] : level0_syms[i - nsyms];
		if (xkb_keysym_to_lower(sym) == ptt_keysym) {
			ptt_key_held = 1;
			ptt_held_keycode = keycode;
			ptt_set_mute(0);
			return;
		}
	}
}

void
ptt_handle_button(uint32_t button, int pressed)
{
	if (!pressed) {
		if (ptt_button_held && button == ptt_held_button) {
			ptt_button_held = 0;
			ptt_set_mute(1);
		}
		return;
	}

	if (!ptt_button || ptt_button_held || button != ptt_button)
		return;
	if (ptt_mods) {
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
		uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
		if (CLEANMASK(mods) != CLEANMASK(ptt_mods))
			return;
	}

	ptt_button_held = 1;
	ptt_held_button = button;
	ptt_set_mute(0);
}

static int
startmute_cb(void *data)
{
	(void)data;

	set_pipewire_mic_mute(1);
	set_status_task_due(refreshstatusmic, monotonic_msec() + 500);

	if (startmute_tries < (int)(sizeof(startmute_delays_ms)
			/ sizeof(startmute_delays_ms[0])))
		wl_event_source_timer_update(startmute_timer,
			startmute_delays_ms[startmute_tries++]);
	return 0;
}

static int
gamingconf_readable(int fd, uint32_t mask, void *data)
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
			if (ev->len && strcmp(ev->name, GAMINGCONF_NAME) == 0)
				relevant = 1;
			p += sizeof(*ev) + ev->len;
		}
	}

	if (relevant)
		gaming_conf_load();
	return 0;
}

void
gaming_conf_setup(void)
{
	int wd;

	gamingconf_resolve_paths();
	gaming_conf_load();

	/* The mic always starts muted; PipeWire may not be up yet, so the
	 * timer retries a few times. */
	set_pipewire_mic_mute(1);
	startmute_timer = wl_event_loop_add_timer(event_loop, startmute_cb, NULL);
	if (startmute_timer)
		wl_event_source_timer_update(startmute_timer,
			startmute_delays_ms[startmute_tries++]);

	mkdir(gamingconf_dir, 0755);

	gamingconf_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (gamingconf_fd < 0) {
		wlr_log(WLR_ERROR, "gaming.conf: inotify_init failed: %s",
			strerror(errno));
		return;
	}

	wd = inotify_add_watch(gamingconf_fd, gamingconf_dir,
		IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	if (wd < 0) {
		wlr_log(WLR_ERROR, "gaming.conf: inotify_add_watch(%s) failed: %s",
			gamingconf_dir, strerror(errno));
		close(gamingconf_fd);
		gamingconf_fd = -1;
		return;
	}

	gamingconf_source = wl_event_loop_add_fd(event_loop, gamingconf_fd,
		WL_EVENT_READABLE, gamingconf_readable, NULL);
	wlr_log(WLR_INFO, "gaming.conf: watching %s", gamingconf_path);
}

void
gaming_conf_cleanup(void)
{
	if (startmute_timer) {
		wl_event_source_remove(startmute_timer);
		startmute_timer = NULL;
	}
	if (gamingconf_source) {
		wl_event_source_remove(gamingconf_source);
		gamingconf_source = NULL;
	}
	if (gamingconf_fd >= 0) {
		close(gamingconf_fd);
		gamingconf_fd = -1;
	}
}
