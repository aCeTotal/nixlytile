/* Super released a scan early. */
#include "modkey/modkey.h"
#include "diag.h"

/* Modifier releases within one scan. */
void
super_gone(uint32_t mod, uint32_t time_msec)
{
	if (time_msec - super.gone_ms >= MODKEY_STREAM_MS)
		super.gone_mods = 0;
	super.gone_mods |= mod;
	super.gone_ms = time_msec;
}

/* Modifiers released with Super. */
uint32_t
super_gone_with(void)
{
	int32_t gap = (int32_t)(super.up_ms - super.gone_ms);

	if (gap <= -MODKEY_STREAM_MS || gap >= MODKEY_STREAM_MS)
		return 0;
	return super.gone_mods;
}

/* Super released within one scan. */
uint32_t
modkey_skew(KeyboardGroup *group, const struct nx_keyboard_key_event *event,
		uint32_t mods)
{
	struct nx_keyboard *kb = &group->nx_group->keyboard;
	uint32_t since = event->time_msec - super.up_ms;
	uint32_t gone = super_gone_with();
	const Key *k, *g;

	if (group != kb_group || locked || mods & modkey
			|| event->state != WL_KEYBOARD_KEY_STATE_PRESSED
			|| key_mod(event->keycode) || since >= MODKEY_SKEW_MS
			|| looks_like_game(focustop(selmon)))
		return 0;
	k = chord_binding(kb, mods | modkey, event->keycode);
	g = gone ? chord_binding(kb, mods | gone | modkey, event->keycode) : NULL;
	/* Dropout or release: Super decides. */
	if (k && g && g != k)
		return 0;
	if (!k && !g)
		return 0;
	diag_logf("KEY", "MODSKEW since=%ums", since);
	return k ? modkey : modkey | gone;
}
