#include "nixlytile.h"
#include "netsys.h"

#define BTIDLE_MS      60000
#define BTIDLE_WAKE_MS 2000

static struct wl_event_source *idle_timer;
static uint64_t woke_at;

static int
popup_open(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->statusbar.bt_popup.visible)
			return 1;
	}
	return 0;
}

static int
in_use(void)
{
	BtAdapter a;

	if (popup_open() || btmon_connected_count())
		return 1;
	return btmon_adapter(&a) && a.discovering;
}

static int
idle_cb(void *data)
{
	if (!btmon_daemon_up())
		return 0;
	if (in_use()) {
		btidle_touch();
		return 0;
	}
	btmon_daemon_stop();
	return 0;
}

void
btidle_touch(void)
{
	if (!idle_timer)
		idle_timer = wl_event_loop_add_timer(event_loop, idle_cb, NULL);
	if (idle_timer)
		wl_event_source_timer_update(idle_timer, BTIDLE_MS);
}

void
btidle_wake(void)
{
	uint64_t now = monotonic_msec();

	btidle_touch();
	if (btmon_daemon_up() || now - woke_at < BTIDLE_WAKE_MS)
		return;
	woke_at = now;
	btmon_daemon_wake();
}

void
btidle_ready(void)
{
	btidle_touch();
	if (popup_open())
		bt_popup_opened();
}

int
btidle_dormant(void)
{
	return !btmon_daemon_up() && findbluetoothdevice();
}
