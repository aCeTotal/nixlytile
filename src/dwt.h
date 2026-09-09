/*
 * dwt.h — disable-touchpad-while-typing.
 *
 * libinput has its own DWT (enabled via the `disable-while-typing` touchpad
 * option), but it only holds the pad off for ~180ms after the last key and
 * it never cancels a touch that was already down when typing started — a
 * palm resting on the pad still moves the cursor between keystrokes.  This
 * adds a compositor-side lockout on top: while the user is typing, pointer
 * events coming from a TOUCHPAD are dropped outright.  External mice and
 * trackpoints are untouched.
 */
#ifndef DWT_H
#define DWT_H

#include <stdint.h>

struct wlr_input_device;

/* Called for every key event.  Non-modifier keys arm the lockout; Shift /
 * Ctrl / Alt / Super / CapsLock do not, so Mod+drag with the touchpad keeps
 * working. */
void dwt_note_key(uint32_t keycode, int pressed);

/* 1 when `dev` is a touchpad whose events should be swallowed right now. */
int dwt_suppress(struct wlr_input_device *dev);

#endif /* DWT_H */
