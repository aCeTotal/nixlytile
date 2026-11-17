/* Binding modifiers that survive xkb desync. */
#include "nixlytile.h"
#include "client.h"
#include "diag.h"

static uint32_t
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

uint32_t
binding_mods(struct nx_keyboard *kb)
{
	uint32_t xkb = nx_keyboard_get_modifiers(kb);
	uint32_t held;

	/* Virtual keymaps reuse keycodes freely. */
	if (kb != &kb_group->nx_group->keyboard)
		return xkb;
	held = held_mods(kb);
	if (held & ~xkb)
		diag_logf("KEY", "MODDESYNC held=%#x xkb=%#x", held, xkb);
	return xkb | held;
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
