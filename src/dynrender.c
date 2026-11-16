#include "nixlytile.h"

#define DR_MISS_LIMIT       3
#define DR_MISS_WINDOW_NS   1000000000ULL

int dynamic_render_enabled = 1;

/* NULL game: none qualifies. */
void
dynrender_tick(Monitor *m, Client *game, uint64_t now)
{
	DynRender *d = &m->dr;
	int capped = fps_limit_enabled && fps_limit_value > 0;

	if (!dynamic_render_enabled || !rung_vblank_ns(m))
		game = NULL;
	if (game != d->game) {
		d->game = game;
		d->owner = 0;
	}
	if (!game)
		return;
	if (!capped && !d->owner) {
		autolock_reset(m);
		ladder_start(m, now);
	}
	d->owner = !capped;
	if (d->owner)
		ladder_tick(m, now);
}

void
dynrender_latched(Monitor *m)
{
	m->dr.fresh = 1;
}

void
dynrender_committed(Client *c)
{
	DynRender *d = &c->mon->dr;
	uint64_t now = get_time_ns();

	if (c != d->game || !d->owner || !d->released_ns)
		return;
	d->work[d->work_idx] = now - d->released_ns;
	d->work_idx = (d->work_idx + 1) % DR_WORK_SLOTS;
	d->work_n = MIN(d->work_n + 1, DR_WORK_SLOTS);
	d->released_ns = 0;
}

/* Empty slot means judder. */
void
dynrender_release(Monitor *m, uint64_t now)
{
	DynRender *d = &m->dr;
	int oldest;

	d->released_ns = now;
	if (d->fresh) {
		d->fresh = 0;
		return;
	}
	d->miss_ns[d->miss_idx] = now;
	d->miss_idx = (d->miss_idx + 1) % DR_MISS_SLOTS;
	d->calm_ns = now;
	oldest = (d->miss_idx + DR_MISS_SLOTS - DR_MISS_LIMIT) % DR_MISS_SLOTS;
	if (now - d->miss_ns[oldest] <= DR_MISS_WINDOW_NS)
		ladder_missed(m, now);
}

int
dynrender_vblanks(Monitor *m)
{
	return m->dr.rung + 1;
}

uint64_t
dynrender_interval_ns(Monitor *m)
{
	return rung_interval(m, m->dr.rung);
}

const char *
dynrender_diag(Monitor *m)
{
	static char buf[64];

	if (!m->dr.owner)
		return "dr=-";
	snprintf(buf, sizeof(buf), "dr=%.0f", rung_fps(m, m->dr.rung));
	return buf;
}
