/* Keys held for a late Super. */
#include "modkey/modkey.h"
#include "client.h"
#include "platform/input/keyboard/keyboard_state.h"

struct Pending pending;

/* Bind keysym, else NoSymbol. */
static xkb_keysym_t
bound_sym(struct nx_keyboard *kb, uint32_t mods, uint32_t keycode)
{
	const Key *k = chord_binding(kb, mods, keycode);

	return k ? k->keysym : XKB_KEY_NoSymbol;
}

/* Queued presses of keycode. */
int
pending_presses(uint32_t keycode)
{
	int i, n = 0;

	for (i = 0; i < pending.n; i++)
		n += pending.ev[i].state == WL_KEYBOARD_KEY_STATE_PRESSED
				&& pending.ev[i].keycode == keycode;
	return n;
}

/* Edge replays as is. */
static int
plain_edge(const struct nx_keyboard_key_event *event)
{
	return event->state == WL_KEYBOARD_KEY_STATE_RELEASED
			|| key_mod(event->keycode);
}

void
pending_push(const struct nx_keyboard_key_event *event, uint32_t mods,
		uint32_t released)
{
	struct nx_keyboard *kb = &kb_group->nx_group->keyboard;
	int i = pending.n++;

	pending.ev[i] = *event;
	pending.mods[i] = mods;
	pending.sym[i] = bound_sym(kb, mods | modkey, event->keycode);
	pending.plain[i] = bound_sym(kb, mods, event->keycode);
	pending.late[i] = 0;
	pending.released[i] = released;
	pending.seen[i] = client_modifiers(kb);
}

/* Client modifiers become want. */
static void
client_mods_sync(struct nx_keyboard_modifiers *cur,
		const struct nx_keyboard_modifiers *want)
{
	if (input_keyboard_modifiers_equal(cur, want))
		return;
	*cur = *want;
	nx_seat_keyboard_notify_modifiers(seat, cur);
}

/* Replay press with its modifiers. */
static void
replay_press(int i, struct nx_keyboard_modifiers *cur)
{
	client_mods_sync(cur, &pending.seen[i]);
	if (key_replay(kb_group, &pending.ev[i], pending.mods[i]))
		super.typed_ms = pending.ev[i].time_msec;
}

/* Bind a press runs without Super. */
static const Key *
pending_fallback(struct nx_keyboard *kb, int i)
{
	const Key *k = NULL;

	if (pending.released[i])
		k = chord_binding(kb, pending.mods[i] | pending.released[i]
				| modkey, pending.ev[i].keycode);
	return k ? k : key_lookup(pending.mods[i], pending.plain[i]);
}

/* Replay held keys without Super. */
void
pending_type(void)
{
	struct nx_keyboard *kb = &kb_group->nx_group->keyboard;
	struct nx_keyboard_modifiers now = client_modifiers(kb), cur = now;
	const Key *k;
	int i, n = pending.n;

	pending.n = 0;
	wl_event_source_timer_update(pending.timer, 0);
	for (i = 0; i < n; i++) {
		if (plain_edge(&pending.ev[i])) {
			key_deliver(kb_group, &pending.ev[i]);
			continue;
		}
		k = pending_fallback(kb, i);
		if (k) {
			key_run(k);
			continue;
		}
		replay_press(i, &cur);
	}
	client_mods_sync(&cur, &now);
}

/* Bind held keys with Super. */
void
pending_bind(KeyboardGroup *group)
{
	struct nx_keyboard *kb = &group->nx_group->keyboard;
	struct nx_keyboard_modifiers now = client_modifiers(kb), cur = now;
	const Key *k = NULL;
	uint32_t code = 0, mods = 0;
	int i, n = pending.n;

	pending.n = 0;
	wl_event_source_timer_update(pending.timer, 0);
	for (i = 0; i < n; i++) {
		if (plain_edge(&pending.ev[i])) {
			key_deliver(kb_group, &pending.ev[i]);
			continue;
		}
		code = pending.ev[i].keycode;
		mods = pending.mods[i] | modkey;
		k = key_lookup(mods, pending.sym[i]);
		if (!k && pending.released[i]) {
			mods |= pending.released[i];
			k = chord_binding(kb, mods, code);
		}
		if (k)
			key_run(k);
		else
			replay_press(i, &cur);
	}
	client_mods_sync(&cur, &now);
	if (k && key_down(kb, code))
		key_repeat_bind(group, k, mods);
}

/* Modifiers right after a key join it. */
void
pending_rebase(struct nx_keyboard *kb)
{
	xkb_keysym_t sym;
	int i;

	for (i = 0; i < pending.n; i++) {
		if (!pending.late[i])
			continue;
		sym = bound_sym(kb, pending.mods[i] | pending.late[i] | modkey,
				pending.ev[i].keycode);
		if (sym == XKB_KEY_NoSymbol)
			continue;
		pending.mods[i] |= pending.late[i];
		pending.sym[i] = sym;
	}
}
