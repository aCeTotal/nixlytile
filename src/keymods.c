/* Chatter-proof binding modifiers. */
#include "nixlytile.h"
#include "client.h"
#include "diag.h"
#include "platform/input/keyboard/keyboard_state.h"

#define MODKEY_BOUNCE_MS 140 /* EC drops whole 22 ms scans */
#define MODKEY_LEAD_MS 25    /* same-scan key precedes Super */
#define MODKEY_IDLE_MS 250   /* typing streams skip the lead */

static uint32_t modkey_up_ms;
static uint32_t last_press_ms;

/* Bound key waiting on a late Super. */
static struct {
	struct nx_keyboard_key_event press;
	struct nx_keyboard_key_event release;
	uint32_t mods;
	int armed;
	int released;
	int lead;
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

/* No Super came: replay as typed. */
static void
pending_deliver(void)
{
	key_replay(kb_group, &pending.press, pending.mods);
	if (pending.released)
		key_deliver(kb_group, &pending.release);
}

static int
pending_expired(void *data)
{
	(void)data;
	pending.armed = 0;
	if (!pending.lead)
		diag_logf("KEY", "MODBOUNCE expired");
	pending_deliver();
	return 0;
}

/* Late Super binds; 1 = event kept. */
int
modkey_settle(KeyboardGroup *group, const struct nx_keyboard_key_event *event)
{
	int pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	int is_modkey = key_mod(event->keycode) == modkey;
	const Key *k = NULL;

	if (group != kb_group)
		return 0;
	if (is_modkey && !pressed)
		modkey_up_ms = event->time_msec;
	if (!pending.armed)
		return 0;
	if (!pressed && !pending.lead
			&& event->keycode == pending.press.keycode) {
		pending.release = *event;
		pending.released = 1;
		return 1;
	}
	pending.armed = 0;
	wl_event_source_timer_update(pending.timer, 0);
	if (is_modkey && pressed && !locked)
		k = chord_binding(&group->nx_group->keyboard,
				pending.mods | modkey, pending.press.keycode);
	if (!k) {
		pending_deliver();
		return 0;
	}
	if (pending.lead)
		diag_logf("KEY", "MODLEAD lead=%ums",
				event->time_msec - pending.press.time_msec);
	else
		diag_logf("KEY", "MODBOUNCE gap=%ums",
				event->time_msec - modkey_up_ms);
	key_run(k);
	/* Bound keys still release normally. */
	if (pending.released)
		key_deliver(kb_group, &pending.release);
	else
		key_repeat_bind(group, k, pending.mods | modkey);
	return 0;
}

/* Super edge on the chattering keyboard. */
int
modkey_chatter(KeyboardGroup *group, uint32_t keycode)
{
	return group == kb_group && key_mod(keycode) == modkey;
}

/* Repeats pause through Super chatter. */
enum repeat_tick
modkey_repeat_tick(KeyboardGroup *group)
{
	uint32_t now_ms = (uint32_t)monotonic_msec();

	if (group != kb_group || !(group->mods & modkey)
			|| binding_mods(&group->nx_group->keyboard) & modkey)
		return REPEAT_RUN;
	if (now_ms - modkey_up_ms < MODKEY_BOUNCE_MS)
		return REPEAT_PAUSE;
	return REPEAT_STOP;
}

/* Hold a bound key for a late Super. */
int
modkey_hold(KeyboardGroup *group, const struct nx_keyboard_key_event *event,
		uint32_t mods)
{
	uint32_t since = event->time_msec - modkey_up_ms;
	uint32_t idle = event->time_msec - last_press_ms;
	int lead = since >= MODKEY_BOUNCE_MS;

	if (group != kb_group || key_mod(event->keycode))
		return 0;
	last_press_ms = event->time_msec;
	if (mods & modkey || (lead && idle < MODKEY_IDLE_MS)
			|| !chord_binding(&group->nx_group->keyboard,
				mods | modkey, event->keycode)
			|| chord_binding(&group->nx_group->keyboard,
				mods, event->keycode)
			|| (lead && looks_like_game(focustop(selmon))))
		return 0;
	if (!pending.timer)
		pending.timer = wl_event_loop_add_timer(event_loop, pending_expired, NULL);
	if (!pending.timer)
		return 0;
	pending.press = *event;
	pending.mods = mods;
	pending.released = 0;
	pending.lead = lead;
	pending.armed = 1;
	wl_event_source_timer_update(pending.timer,
			lead ? MODKEY_LEAD_MS : MODKEY_BOUNCE_MS - since);
	return 1;
}
