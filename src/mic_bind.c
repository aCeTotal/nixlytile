#include "nixlytile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIND_TEXT_MAX   128
#define BINDS_INITIAL   4
#define BIND_SEPARATOR  "+"
#define MOUSE_PREFIX    "mouse:"
#define KEYSYM_PREFIX   "key:"

enum bind_role { BIND_TALK, BIND_VOIP };

static const struct {
	const char *key;
	enum bind_role role;
} bind_keys[] = {
	{ "ptt-bind", BIND_TALK },
	{ "ptm-bind", BIND_VOIP },
};

static const struct {
	const char *name;
	uint32_t mod;
} mod_names[] = {
	{ "ctrl", NX_MODIFIER_CTRL },
	{ "alt", NX_MODIFIER_ALT },
	{ "shift", NX_MODIFIER_SHIFT },
	{ "super", NX_MODIFIER_LOGO },
};

/* Keysym or button, never both. */
struct bind {
	enum bind_role role;
	uint32_t mods;
	xkb_keysym_t keysym;
	uint32_t button;
	uint32_t held;              /* code holding it */
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

/* Mute opens mic, closes Discord. */
void
mic_bind_apply(void)
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

static uint32_t
mod_from_name(const char *tok)
{
	size_t i;

	for (i = 0; i < LENGTH(mod_names); i++)
		if (strcmp(tok, mod_names[i].name) == 0)
			return mod_names[i].mod;
	return 0;
}

/* Text past prefix, else NULL. */
static const char *
skip_prefix(const char *tok, const char *prefix)
{
	size_t n = strlen(prefix);

	return strncmp(tok, prefix, n) == 0 ? tok + n : NULL;
}

static void
parse_token(const char *tok, struct bind *b)
{
	uint32_t mod = mod_from_name(tok);
	const char *code;

	if (mod) {
		b->mods |= mod;
		return;
	}
	if ((code = skip_prefix(tok, MOUSE_PREFIX))) {
		b->button = (uint32_t)strtoul(code, NULL, 0);
		return;
	}
	if ((code = skip_prefix(tok, KEYSYM_PREFIX))) {
		b->keysym = (xkb_keysym_t)strtoul(code, NULL, 0);
		return;
	}
	b->keysym = xkb_keysym_from_name(tok, XKB_KEYSYM_CASE_INSENSITIVE);
}

static int
parse_bind(const char *value, struct bind *b)
{
	char buf[BIND_TEXT_MAX];
	char *tok, *save = NULL;

	snprintf(buf, sizeof(buf), "%s", value);
	for (tok = strtok_r(buf, BIND_SEPARATOR, &save); tok;
			tok = strtok_r(NULL, BIND_SEPARATOR, &save))
		parse_token(tok, b);
	if (b->button) {
		b->keysym = XKB_KEY_NoSymbol;
		return 1;
	}
	b->keysym = xkb_keysym_to_lower(b->keysym);
	return b->keysym != XKB_KEY_NoSymbol;
}

static int
grow_binds(void)
{
	size_t cap = capbinds ? capbinds * 2 : BINDS_INITIAL;
	struct bind *grown = realloc(binds, cap * sizeof(*binds));

	if (!grown)
		return 0;
	binds = grown;
	capbinds = cap;
	return 1;
}

static void
add_bind(enum bind_role role, const char *value)
{
	struct bind b = { .role = role, .keysym = XKB_KEY_NoSymbol };

	if (!parse_bind(value, &b))
		return;
	if (nbinds == capbinds && !grow_binds())
		return;
	binds[nbinds++] = b;
}

void
mic_bind_parse(const char *key, const char *value)
{
	size_t i;

	for (i = 0; i < LENGTH(bind_keys); i++)
		if (strcmp(key, bind_keys[i].key) == 0)
			add_bind(bind_keys[i].role, value);
}

size_t
mic_bind_count(void)
{
	return nbinds;
}

void
mic_bind_clear(void)
{
	free(binds);
	binds = NULL;
	nbinds = capbinds = 0;
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

/* Modifiers may lift first. */
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
		mic_bind_apply();
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
		mic_bind_apply();
}

void
ptt_handle_button(uint32_t button, int pressed)
{
	struct nx_keyboard *kb = nx_seat_get_keyboard(seat);
	uint32_t mods = kb ? nx_keyboard_get_modifiers(kb) : 0;
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
		mic_bind_apply();
}
