/* Chatter-proof binding modifiers. */
#include "nixlytile.h"
#include "client.h"
#include "diag.h"
#include "platform/input/keyboard/keyboard_state.h"

#define MODKEY_BOUNCE_MS 80 /* measured gaps: 20-30 ms */

static uint32_t modkey_up_ms;

/* Bound key pressed in a modkey gap. */
static struct {
	struct nx_keyboard_key_event event;
	uint32_t mods;
	int armed;
	struct wl_event_source *timer;
} pending;

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

/* Bind this key would fire. */
static const Key *
chord_binding(struct nx_keyboard *kb, uint32_t mods, uint32_t keycode)
{
	xkb_keycode_t code = keycode + EVDEV_XKB_OFFSET;
	xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, code);
	const xkb_keysym_t *syms;
	const Key *k = NULL;
	int i, n;

	n = xkb_state_key_get_syms(kb->xkb_state, code, &syms);
	for (i = 0; i < n && !k; i++)
		k = key_lookup(mods, syms[i]);
	n = xkb_keymap_key_get_syms_by_level(kb->keymap, code, layout, 0, &syms);
	for (i = 0; i < n && !k; i++)
		k = key_lookup(mods, syms[i]);
	return k;
}

static int
pending_expired(void *data)
{
	(void)data;
	pending.armed = 0;
	diag_logf("KEY", "MODBOUNCE expired");
	key_deliver(kb_group, &pending.event);
	return 0;
}

/* Modkey return binds pending key. */
void
modkey_settle(KeyboardGroup *group, const struct nx_keyboard_key_event *event)
{
	int pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	int is_modkey = key_mod(event->keycode) == modkey;
	const Key *k = NULL;

	if (group != kb_group)
		return;
	if (is_modkey && !pressed)
		modkey_up_ms = event->time_msec;
	if (!pending.armed)
		return;
	pending.armed = 0;
	wl_event_source_timer_update(pending.timer, 0);
	if (is_modkey && pressed && !locked)
		k = chord_binding(&group->nx_group->keyboard,
				pending.mods | modkey, pending.event.keycode);
	if (!k) {
		key_deliver(group, &pending.event);
		return;
	}
	diag_logf("KEY", "MODBOUNCE gap=%ums", event->time_msec - modkey_up_ms);
	key_run(k);
}

/* Hold bound key in modkey gap. */
int
modkey_hold(KeyboardGroup *group, const struct nx_keyboard_key_event *event,
		uint32_t mods)
{
	uint32_t since = event->time_msec - modkey_up_ms;

	if (group != kb_group || mods & modkey || key_mod(event->keycode)
			|| since >= MODKEY_BOUNCE_MS
			|| !chord_binding(&group->nx_group->keyboard,
				mods | modkey, event->keycode))
		return 0;
	if (!pending.timer)
		pending.timer = wl_event_loop_add_timer(event_loop, pending_expired, NULL);
	if (!pending.timer)
		return 0;
	pending.event = *event;
	pending.mods = mods;
	pending.armed = 1;
	wl_event_source_timer_update(pending.timer, MODKEY_BOUNCE_MS - since);
	return 1;
}
