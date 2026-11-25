/* Super held through dropouts. */
#include "modkey/modkey.h"

#define MODKEY_LATCH_MS 500  /* first tap reaches 420 ms */
#define MODKEY_CHAIN_MS 1500 /* taps under one dropout */

/* Under cap, nothing typed. */
static int
dropout_open(uint32_t time_msec)
{
	return time_msec - super.up_ms < MODKEY_DROPOUT_MS && !super.texted;
}

/* Hop now would latch. */
static int
dropout_alive(uint32_t time_msec)
{
	if (!dropout_open(time_msec))
		return 0;
	return time_msec - super.up_ms < MODKEY_LATCH_MS
			|| (super.anchored
				&& time_msec - super.last_ms < MODKEY_CHAIN_MS);
}

/* Open dropout keeps the hold. */
void
super_pressed(uint32_t time_msec)
{
	super.fresh = !dropout_open(time_msec);
	if (super.fresh)
		super.bound = 0;
	super.down_ms = time_msec;
	super.keyed = 0;
}

/* Blips keep the dropout going. */
void
super_released(uint32_t time_msec)
{
	int blip = !super.keyed
			&& time_msec - super.down_ms < MODKEY_BOUNCE_MS;

	if (super.fresh)
		super.blipped = blip;
	if (super.fresh || !blip) {
		super.anchored = 0;
		super.texted = 0;
	}
	super.up_ms = time_msec;
}

void
super_ran(const Key *k, uint32_t time_msec)
{
	if (!(k->mod & modkey))
		return;
	super.last = k->func;
	super.last_ms = time_msec;
	super.bound = 1;
	super.anchored = 1;
}

/* Super mouse binds end hopping. */
void
modkey_click(uint32_t mods)
{
	if (!(mods & modkey))
		return;
	super.last = NULL;
	super.blipped = 0;
}

/* Repeats the last hop. */
int
super_hop(const Key *k)
{
	return binding_hops(k) && k->func == super.last;
}

/* Hop this hold may latch. */
int
super_tap(struct nx_keyboard *kb, uint32_t mods, uint32_t keycode)
{
	const Key *k = chord_binding(kb, mods | modkey, keycode);

	if (super.bound)
		return super_hop(k);
	return super.blipped && binding_hops(k);
}

/* Reported up, still held. */
int
super_latched(struct nx_keyboard *kb, const struct nx_keyboard_key_event *event,
		uint32_t mods)
{
	return event->time_msec - super.up_ms >= MODKEY_SKEW_MS
			&& dropout_alive(event->time_msec)
			&& super_tap(kb, mods, event->keycode);
}
