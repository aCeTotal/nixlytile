/* Which bind a key chord fires. */
#include "modkey/modkey.h"
#include "client.h"
#include "diag.h"
#include "platform/input/keyboard/keyboard_state.h"

uint32_t
key_mod(uint32_t keycode)
{
	switch (keycode) {
	case KEY_LEFTMETA:
	case KEY_RIGHTMETA:
		return NX_MODIFIER_LOGO;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
		return NX_MODIFIER_CTRL;
	case KEY_LEFTALT:
		return NX_MODIFIER_ALT;
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
		return NX_MODIFIER_SHIFT;
	default:
		return 0;
	}
}

/* Held modifier keys, xkb aside. */
static uint32_t
held_mods(struct nx_keyboard *kb)
{
	uint32_t mods = 0;
	size_t i;

	for (i = 0; i < kb->num_keycodes; i++)
		mods |= key_mod(kb->keycodes[i]);
	return mods;
}

int
key_down(struct nx_keyboard *kb, uint32_t keycode)
{
	size_t i;

	for (i = 0; i < kb->num_keycodes; i++)
		if (kb->keycodes[i] == keycode)
			return 1;
	return 0;
}

uint32_t
binding_mods(struct nx_keyboard *kb)
{
	uint32_t xkb = nx_keyboard_get_modifiers(kb);

	/* Virtual keymaps reuse keycodes freely. */
	if (kb != &kb_group->nx_group->keyboard)
		return xkb;
	return xkb | held_mods(kb);
}

static int
is_modifier(xkb_keysym_t sym)
{
	return (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) ||
			(sym >= XKB_KEY_ISO_Lock && sym <= XKB_KEY_ISO_Level5_Lock);
}

static void
note(const char *what, uint32_t mods, xkb_keysym_t sym)
{
	struct nx_surface *s = seat->keyboard_state.focused_surface;
	Client *c = NULL;
	char name[64];

	if (!(mods & modkey) || is_modifier(sym))
		return;
	toplevel_from_nx_surface(s, &c, NULL);
	xkb_keysym_get_name(sym, name, sizeof(name));
	diag_logf("KEY", "%s sym=%s mods=%#x appid='%s'", what, name, mods,
			c && client_get_appid(c) ? client_get_appid(c) : "(none)");
}

/* A focused app blocked the bind. */
void
binding_inhibited(uint32_t mods, xkb_keysym_t sym)
{
	note("INHIBITED", mods, sym);
}

/* Mod chord with no bind. */
void
binding_unbound(uint32_t mods, xkb_keysym_t sym)
{
	note("UNBOUND", mods, sym);
}

/* Bind for a keysym on level. */
static const Key *
level_binding(struct nx_keyboard *kb, uint32_t keycode, xkb_level_index_t level,
		uint32_t mods)
{
	xkb_keycode_t code = keycode + EVDEV_XKB_OFFSET;
	xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, code);
	const xkb_keysym_t *syms;
	const Key *k = NULL;
	int i, n;

	n = xkb_keymap_key_get_syms_by_level(kb->keymap, code, layout, level, &syms);
	for (i = 0; i < n && !k; i++)
		k = key_lookup(mods, syms[i]);
	return k;
}

const Key *
chord_binding(struct nx_keyboard *kb, uint32_t mods, uint32_t keycode)
{
	xkb_keycode_t code = keycode + EVDEV_XKB_OFFSET;
	const xkb_keysym_t *syms;
	const Key *k = NULL;
	int i, n;

	/* Shift added back: its level first. */
	if (mods & NX_MODIFIER_SHIFT
			&& !(nx_keyboard_get_modifiers(kb) & NX_MODIFIER_SHIFT)) {
		k = level_binding(kb, keycode, 1, mods);
		return k ? k : level_binding(kb, keycode, 0, mods);
	}
	n = xkb_state_key_get_syms(kb->xkb_state, code, &syms);
	for (i = 0; i < n && !k; i++)
		k = key_lookup(mods, syms[i]);
	return k ? k : level_binding(kb, keycode, 0, mods);
}

int
chord_reachable(struct nx_keyboard *kb, uint32_t mods, uint32_t keycode)
{
	static const uint32_t extra[] = {
		0, NX_MODIFIER_SHIFT, NX_MODIFIER_CTRL, NX_MODIFIER_ALT,
	};
	size_t i;

	for (i = 0; i < LENGTH(extra); i++)
		if (chord_binding(kb, mods | modkey | extra[i], keycode))
			return 1;
	return 0;
}

/* Workspace hop, latch candidate. */
int
binding_hops(const Key *k)
{
	return k && (k->func == focus_workspace_n
			|| k->func == move_client_to_ws_n);
}

/* Run the bind this chord fires. */
int
modkey_bind(KeyboardGroup *group, const struct nx_keyboard_key_event *event,
		uint32_t mods)
{
	const Key *k = chord_binding(&group->nx_group->keyboard, mods,
			event->keycode);

	if (!k)
		return 0;
	key_run(k);
	if (group == kb_group)
		super_ran(k, event->time_msec);
	return 1;
}
