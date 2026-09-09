/* remote_mouse.c — analog sticks drive the pointer in remote mode.
 *
 * Both sticks move the cursor; L3 left-clicks, R3 right-clicks.
 * Radial deadzone plus a quadratic curve, velocity scaled by real dt so
 * the feel is identical at any output refresh.
 */

#include <math.h>
#include <string.h>
#include <time.h>

#include <linux/input-event-codes.h>

#include "nixlytile.h"
#include "client.h"
#include "remote.h"

#define STICK_DEADZONE 0.15
#define STICK_TICK_MS  8

static double stick_lx, stick_ly, stick_rx, stick_ry;
static struct wl_event_source *tick_src;
static uint64_t last_tick_ns;

static uint64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Sticks stay out of the way of a fullscreen game */
static int
mouse_blocked(void)
{
	Client *c;

	if (!remote_is_active() || !selmon)
		return 1;
	c = focustop(selmon);
	return c && c->isfullscreen;
}

/* Sum both sticks, apply radial deadzone and quadratic response */
static int
stick_vector(double *out_x, double *out_y)
{
	double x = stick_lx + stick_rx;
	double y = stick_ly + stick_ry;
	double mag = sqrt(x * x + y * y);
	double curved;

	if (mag > 1.0) {
		x /= mag;
		y /= mag;
		mag = 1.0;
	}
	if (mag <= STICK_DEADZONE)
		return 0;

	curved = (mag - STICK_DEADZONE) / (1.0 - STICK_DEADZONE);
	curved *= curved;

	*out_x = x / mag * curved;
	*out_y = y / mag * curved;
	return 1;
}

static int
tick_cb(void *data)
{
	double dir_x, dir_y, dt;
	uint64_t now = now_ns();

	(void)data;

	if (!stick_vector(&dir_x, &dir_y) || mouse_blocked()) {
		tick_src = NULL;
		last_tick_ns = 0;
		return 0;
	}

	dt = last_tick_ns ? (double)(now - last_tick_ns) / 1e9 : STICK_TICK_MS / 1000.0;
	if (dt > 0.1)
		dt = 0.1;
	last_tick_ns = now;

	motionnotify(0, NULL,
		dir_x * remote_mouse_speed * dt,
		dir_y * remote_mouse_speed * dt, 0, 0);

	wl_event_source_timer_update(tick_src, STICK_TICK_MS);
	return 0;
}

static void
arm_tick(void)
{
	double dx, dy;

	if (tick_src || !stick_vector(&dx, &dy) || mouse_blocked())
		return;
	if (!(tick_src = wl_event_loop_add_timer(event_loop, tick_cb, NULL)))
		return;
	last_tick_ns = 0;
	wl_event_source_timer_update(tick_src, STICK_TICK_MS);
}

/* Normalized axis value from the pad reader, -1.0 to 1.0 */
void
remote_mouse_axis(int code, double value)
{
	switch (code) {
	case ABS_X:  stick_lx = value; break;
	case ABS_Y:  stick_ly = value; break;
	case ABS_RX: stick_rx = value; break;
	case ABS_RY: stick_ry = value; break;
	default: return;
	}
	arm_tick();
}

void
remote_mouse_click(int right, int pressed)
{
	struct wlr_pointer_button_event ev = {0};

	if (mouse_blocked())
		return;

	ev.button = right ? BTN_RIGHT : BTN_LEFT;
	ev.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED
			   : WL_POINTER_BUTTON_STATE_RELEASED;
	ev.time_msec = (uint32_t)(now_ns() / 1000000ULL);

	buttonpress(NULL, &ev);
	wlr_seat_pointer_notify_frame(seat);
}

void
remote_mouse_reset(void)
{
	stick_lx = stick_ly = stick_rx = stick_ry = 0.0;
	if (tick_src) {
		wl_event_source_remove(tick_src);
		tick_src = NULL;
	}
	last_tick_ns = 0;
}
