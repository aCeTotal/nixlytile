/* gaming.conf: mic binds, dynamic rendering. */

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
#define DYNAMIC_RENDER_KEY "dynamic-render"

enum bind_role { BIND_TALK, BIND_VOIP };

static const struct {
	const char *key;
	enum bind_role role;
} bind_keys[] = {
	{ "ptt-bind", BIND_TALK },
	{ "ptm-bind", BIND_VOIP },
};

/* Either keysym or button is set; held is the code holding it. */
struct bind {
	enum bind_role role;
	uint32_t mods;
	xkb_keysym_t keysym;
	uint32_t button;
	uint32_t held;
};

struct keypress {
	uint32_t mods;
	const xkb_keysym_t *syms;
	int nsyms;
	const xkb_keysym_t *level0_syms;
	int nlevel0;
};

static struct bind *binds;
static size_t nbinds, capbinds;

static char gamingconf_dir[PATH_MAX];
static char gamingconf_path[PATH_MAX];
static int gamingconf_fd = -1;
static struct wl_event_source *gamingconf_source;

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

/* Push-to-mute opens the mic but closes Discord. */
static void
apply_gate(void)
{
	int has_talk = 0, talking = 0, muting = 0;
	uint32_t closed = 0;
	size_t i;

	for (i = 0; i < nbinds; i++) {
		if (binds[i].role == BIND_TALK) {
			has_talk = 1;
			talking |= binds[i].held != 0;
			continue;
		}
		muting |= binds[i].held != 0;
	}
	if (has_talk && !talking && !muting)
		closed |= MIC_GATE_TALK;
	if (muting)
		closed |= MIC_GATE_VOIP;
	mic_gate_set(closed);
}

static int
parse_bind(const char *value, struct bind *b)
{
	char buf[128];
	char *tok, *save = NULL;

	snprintf(buf, sizeof(buf), "%s", value);
	for (tok = strtok_r(buf, "+", &save); tok;
			tok = strtok_r(NULL, "+", &save)) {
		if (strcmp(tok, "ctrl") == 0)
			b->mods |= WLR_MODIFIER_CTRL;
		else if (strcmp(tok, "alt") == 0)
			b->mods |= WLR_MODIFIER_ALT;
		else if (strcmp(tok, "shift") == 0)
			b->mods |= WLR_MODIFIER_SHIFT;
		else if (strcmp(tok, "super") == 0)
			b->mods |= WLR_MODIFIER_LOGO;
		else if (strncmp(tok, "mouse:", 6) == 0)
			b->button = (uint32_t)strtoul(tok + 6, NULL, 0);
		else if (strncmp(tok, "key:", 4) == 0)
			b->keysym = (xkb_keysym_t)strtoul(tok + 4, NULL, 0);
		else
			b->keysym = xkb_keysym_from_name(tok,
				XKB_KEYSYM_CASE_INSENSITIVE);
	}
	if (b->button) {
		b->keysym = XKB_KEY_NoSymbol;
		return 1;
	}
	b->keysym = xkb_keysym_to_lower(b->keysym);
	return b->keysym != XKB_KEY_NoSymbol;
}

static void
add_bind(enum bind_role role, const char *value)
{
	struct bind b = { .role = role, .keysym = XKB_KEY_NoSymbol };
	struct bind *grown;

	if (!parse_bind(value, &b))
		return;
	if (nbinds == capbinds) {
		size_t cap = capbinds ? capbinds * 2 : 4;
		if (!(grown = realloc(binds, cap * sizeof(*binds))))
			return;
		binds = grown;
		capbinds = cap;
	}
	binds[nbinds++] = b;
}

static void
add_line(char *line)
{
	char *eq = strchr(line, '=');
	size_t i;

	if (line[0] == '#' || !eq)
		return;
	*eq = '\0';
	if (strcmp(line, DYNAMIC_RENDER_KEY) == 0) {
		dynamic_render_enabled = strcmp(eq + 1, "0") != 0;
		return;
	}
	for (i = 0; i < LENGTH(bind_keys); i++)
		if (strcmp(line, bind_keys[i].key) == 0)
			add_bind(bind_keys[i].role, eq + 1);
}

static void
gaming_conf_load(void)
{
	char line[256];
	FILE *fp;

	nbinds = 0;
	dynamic_render_enabled = 1;
	fp = fopen(gamingconf_path, "r");
	if (fp) {
		while (fgets(line, sizeof(line), fp)) {
			line[strcspn(line, "\n")] = '\0';
			add_line(line);
		}
		fclose(fp);
	}
	apply_gate();
	wlr_log(WLR_INFO, "gaming.conf: %zu mic bind(s), dynamic rendering %s",
		nbinds, dynamic_render_enabled ? "on" : "off");
}

static int
key_matches(const struct bind *b, const struct keypress *kp)
{
	int i;

	if (b->keysym == XKB_KEY_NoSymbol ||
			CLEANMASK(kp->mods) != CLEANMASK(b->mods))
		return 0;
	for (i = 0; i < kp->nsyms + kp->nlevel0; i++) {
		xkb_keysym_t sym = i < kp->nsyms ? kp->syms[i]
			: kp->level0_syms[i - kp->nsyms];
		if (xkb_keysym_to_lower(sym) == b->keysym)
			return 1;
	}
	return 0;
}

/* Release matches the holding code, so modifiers may lift first. */
static void
release_code(uint32_t code)
{
	int changed = 0;
	size_t i;

	for (i = 0; i < nbinds; i++) {
		if (binds[i].held != code)
			continue;
		binds[i].held = 0;
		changed = 1;
	}
	if (changed)
		apply_gate();
}

void
ptt_handle_key(uint32_t mods, uint32_t keycode, const xkb_keysym_t *syms,
	int nsyms, const xkb_keysym_t *level0_syms, int nlevel0, int pressed)
{
	struct keypress kp = { mods, syms, nsyms, level0_syms, nlevel0 };
	int changed = 0;
	size_t i;

	if (!pressed) {
		release_code(keycode);
		return;
	}
	for (i = 0; i < nbinds; i++) {
		if (binds[i].held || !key_matches(&binds[i], &kp))
			continue;
		binds[i].held = keycode;
		changed = 1;
	}
	if (changed)
		apply_gate();
}

void
ptt_handle_button(uint32_t button, int pressed)
{
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
	int changed = 0;
	size_t i;

	if (!pressed) {
		release_code(button);
		return;
	}
	for (i = 0; i < nbinds; i++) {
		if (binds[i].held || binds[i].button != button ||
				CLEANMASK(mods) != CLEANMASK(binds[i].mods))
			continue;
		binds[i].held = button;
		changed = 1;
	}
	if (changed)
		apply_gate();
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
	if (gamingconf_source) {
		wl_event_source_remove(gamingconf_source);
		gamingconf_source = NULL;
	}
	if (gamingconf_fd >= 0) {
		close(gamingconf_fd);
		gamingconf_fd = -1;
	}
	free(binds);
	binds = NULL;
	nbinds = capbinds = 0;
}
