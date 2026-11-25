/* Super survives EC chatter. */
#include "modkey/modkey.h"
#include "diag.h"

#define MODKEY_IDLE_MS 250 /* gap ending a typing stream */

struct SuperState super;

static void
pending_latch(void)
{
	diag_logf("KEY", "MODLATCH since=%ums",
			pending.ev[0].time_msec - super.up_ms);
	pending_bind(kb_group);
}

/* Latched binds, rest types. */
static void
pending_yield(void)
{
	if (pending.latched && !locked)
		pending_latch();
	else
		pending_type();
}

/* Deadline passed without Super. */
static void
pending_expire(void)
{
	uint32_t since;

	if (pending.latched && !locked) {
		pending_latch();
		return;
	}
	since = pending.ev[0].time_msec - super.up_ms;
	if (!pending.lead || since < MODKEY_DROPOUT_MS)
		diag_logf("KEY", "%s expired since=%ums",
				pending.lead ? "MODLEAD" : "MODBOUNCE", since);
	super.missed = 1;
	pending_type();
}

static int
pending_expired(void *data)
{
	unsigned seq = pending.seq;

	(void)data;
	/* A queued Super settles first. */
	nx_backend_dispatch_input(backend);
	if (pending.n && pending.seq == seq)
		pending_expire();
	return 0;
}

/* Super after the hold ended. */
static int
super_late(uint32_t time_msec)
{
	return (int32_t)(time_msec - pending.until_ms) > 0;
}

/* Logs a Super that came too late. */
static void
super_down(uint32_t time_msec)
{
	uint32_t late = time_msec - pending.until_ms;

	if (super.missed && super_late(time_msec)
			&& late < (pending.lead ? MODKEY_SKEW_MS : MODKEY_BOUNCE_MS))
		diag_logf("KEY", "MODMISS late=%ums", late);
	super.missed = 0;
	super_pressed(time_msec);
}

/* Super arrived: bind queue. */
static void
pending_super(KeyboardGroup *group, uint32_t time_msec)
{
	if (pending.lead)
		diag_logf("KEY", "MODLEAD lead=%ums",
				time_msec - pending.ev[0].time_msec);
	else
		diag_logf("KEY", "MODBOUNCE gap=%ums", time_msec - super.up_ms);
	pending_rebase(&group->nx_group->keyboard);
	pending_bind(group);
	super.keyed = 1;
}

/* Bound key joins the dropout. */
static int
pending_joins(KeyboardGroup *group, uint32_t keycode)
{
	struct nx_keyboard *kb = &group->nx_group->keyboard;
	uint32_t mods;

	if (pending.lead || pending.n == PENDING_MAX)
		return 0;
	mods = binding_mods(kb);
	return pending.latched ? super_tap(kb, mods, keycode)
			: chord_reachable(kb, mods, keycode);
}

/* Queue an edge, or flush if full; 1 = kept. */
static int
pending_keep(const struct nx_keyboard_key_event *event)
{
	if (pending.n == PENDING_MAX) {
		pending_type();
		return 0;
	}
	pending_push(event, 0, 0);
	return 1;
}

/* Plain key during a hold; 1 = kept. */
static int
pending_key(KeyboardGroup *group, const struct nx_keyboard_key_event *event)
{
	int released = event->state == WL_KEYBOARD_KEY_STATE_RELEASED;

	if (released && !pending_presses(event->keycode))
		return 0;
	if (released)
		return pending_keep(event);
	if (!pending_joins(group, event->keycode))
		pending_yield();
	return 0;
}

/* Modifier within a scan joins. */
static void
pending_late(uint32_t mod, uint32_t time_msec)
{
	int i = pending.n - 1;

	while (i > 0 && (pending.ev[i].state != WL_KEYBOARD_KEY_STATE_PRESSED
			|| key_mod(pending.ev[i].keycode)))
		i--;
	if (time_msec - pending.ev[i].time_msec < MODKEY_STREAM_MS)
		pending.late[i] |= mod;
}

/* Shift, Ctrl or Alt edge; 1 = kept. */
static int
pending_modifier(const struct nx_keyboard_key_event *event, uint32_t mod)
{
	int pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;

	if (!pressed && pending.lead) {
		pending_type();
		return 0;
	}
	if (pressed)
		pending_late(mod, event->time_msec);
	return pending_keep(event);
}

/* Late Super binds; 1 = event kept. */
int
modkey_settle(KeyboardGroup *group, const struct nx_keyboard_key_event *event)
{
	int pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
	uint32_t mod = key_mod(event->keycode);

	if (group != kb_group)
		return 0;
	/* Event time, not loop time. */
	if (pending.n && super_late(event->time_msec))
		pending_expire();
	if (mod == modkey && !pressed)
		super_released(event->time_msec);
	if (mod == modkey && pressed)
		super_down(event->time_msec);
	if (!mod && pressed)
		super.keyed = 1;
	if (mod && mod != modkey && !pressed)
		super_gone(mod, event->time_msec);
	if (!pending.n)
		return 0;
	if (!mod)
		return pending_key(group, event);
	if (mod != modkey)
		return pending_modifier(event, mod);
	if (pressed && !locked)
		pending_super(group, event->time_msec);
	else
		pending_type();
	return 0;
}

/* Typed press: held keys first. */
void
modkey_flush(KeyboardGroup *group, const struct nx_keyboard_key_event *event)
{
	if (group != kb_group || key_mod(event->keycode)
			|| event->state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;
	if (pending.n)
		pending_type();
	super.typed_ms = event->time_msec;
	super.texted = 1;
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
	if (now_ms - super.up_ms < MODKEY_BOUNCE_MS)
		return REPEAT_PAUSE;
	return REPEAT_STOP;
}

/* Hold time, one scan minimum. */
static uint32_t
hold_ms(uint32_t since, uint32_t time_msec)
{
	if (since < MODKEY_BOUNCE_MS)
		return MAX(MODKEY_BOUNCE_MS - since, MODKEY_SKEW_MS);
	if (time_msec - super.typed_ms < MODKEY_IDLE_MS)
		return MODKEY_STREAM_MS;
	return MODKEY_SKEW_MS;
}

/* Hold a Super-bound key for Super. */
int
modkey_hold(KeyboardGroup *group, const struct nx_keyboard_key_event *event,
		uint32_t mods)
{
	uint32_t since = event->time_msec - super.up_ms;
	int lead = since >= MODKEY_BOUNCE_MS;
	int game, latched;
	int32_t wait;

	if (group != kb_group || key_mod(event->keycode))
		return 0;
	/* Settle already vetted the join. */
	if (pending.n) {
		pending_push(event, mods, 0);
		return 1;
	}
	if (mods & modkey)
		return 0;
	game = looks_like_game(focustop(selmon));
	if (!chord_reachable(&group->nx_group->keyboard, mods, event->keycode)
			|| (lead && game))
		return 0;
	if (!pending.timer)
		pending.timer = wl_event_loop_add_timer(event_loop, pending_expired, NULL);
	if (!pending.timer)
		return 0;
	latched = !game && super_latched(&group->nx_group->keyboard, event,
			mods);
	pending.latched = latched;
	pending.lead = lead && !latched;
	pending.seq++;
	pending.until_ms = event->time_msec + (latched ? MODKEY_STREAM_MS
			: hold_ms(since, event->time_msec));
	super.missed = 0;
	/* Super stamped at the deadline counts. */
	wait = (int32_t)(pending.until_ms - (uint32_t)monotonic_msec()) + 1;
	wl_event_source_timer_update(pending.timer, MAX(wait, 1));
	pending_push(event, mods,
			since < MODKEY_SKEW_MS && !game ? super_gone_with() : 0);
	return 1;
}
