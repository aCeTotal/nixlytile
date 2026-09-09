/*
 * dwt.c — disable-touchpad-while-typing. See dwt.h.
 */
#include <linux/input-event-codes.h>

#include "nixlytile.h"
#include "dwt.h"

/* How long after the last keystroke the touchpad stays deaf.  libinput's
 * own DWT uses 180ms, which is short enough that a palm resting between
 * words still gets through; 500ms covers normal typing rhythm without
 * being noticeable when you deliberately reach for the pad. */
#define DWT_LOCKOUT_MS 500

static uint64_t dwt_last_type_ms;

void
dwt_note_key(uint32_t keycode, int pressed)
{
	if (!pressed)
		return;

	switch (keycode) {
	case KEY_LEFTSHIFT:  case KEY_RIGHTSHIFT:
	case KEY_LEFTCTRL:   case KEY_RIGHTCTRL:
	case KEY_LEFTALT:    case KEY_RIGHTALT:
	case KEY_LEFTMETA:   case KEY_RIGHTMETA:
	case KEY_CAPSLOCK:
		/* Modifiers are how the user reaches for the pad (Mod+drag to
		 * move a window, Mod+scroll to switch column) — arming the
		 * lockout on them would break exactly those bindings. */
		return;
	default:
		break;
	}

	dwt_last_type_ms = monotonic_msec();
}

int
dwt_suppress(struct wlr_input_device *dev)
{
	struct libinput_device *ldev;

	if (!disable_while_typing || !dwt_last_type_ms || !dev)
		return 0;
	/* A held button means an interaction is in flight (drag, resize,
	 * text selection).  Cutting motion out from under it would strand
	 * the drag mid-air, so typing never interrupts one. */
	if (cursor_mode != CurNormal)
		return 0;
	if (monotonic_msec() - dwt_last_type_ms >= DWT_LOCKOUT_MS)
		return 0;

	/* Touchpads are the only pointers libinput gives a gesture
	 * capability to — mice and trackpoints are unaffected. */
	if (!wlr_input_device_is_libinput(dev))
		return 0;
	if (!(ldev = wlr_libinput_get_device_handle(dev)))
		return 0;
	return libinput_device_has_capability(ldev, LIBINPUT_DEVICE_CAP_GESTURE);
}
