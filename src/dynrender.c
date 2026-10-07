#include "nixlytile.h"
#include "vrslayer/link.h"

#define DR_MISS_LIMIT       3
#define DR_MISS_WINDOW_NS   1000000000ULL
#define NS_PER_MS           1e6f

int dynamic_render_enabled = 1;

/* Manual caps still get VRS. */
static void
publish(Monitor *m, Client *game)
{
	DynRender *d = &m->dr;
	uint64_t interval = d->owner ? rung_interval(m, d->budget_rung)
		: m->fps_limit_interval_ns;

	if (!d->link)
		return;
	vrslink_target(d->link, interval, (uint64_t)((float)interval * d->budget_frac));
	vrslink_cursor(d->link, game);
}

static void
unlink_game(DynRender *d)
{
	if (d->link)
		vrslink_idle(d->link);
	d->link = NULL;
	d->game = NULL;
	d->owner = 0;
}

static void
track_level(DynRender *d, uint64_t now)
{
	uint32_t level = d->link ? atomic_load(&d->link->level) : 0;

	if (level > d->level_seen)
		d->level_rise_ns = now;
	d->level_seen = level;
}

/* NULL game: none qualifies. */
void
dynrender_tick(Monitor *m, Client *game, uint64_t now)
{
	DynRender *d = &m->dr;
	int capped = fps_limit_enabled && fps_limit_value > 0;

	if (!dynamic_render_enabled || !game || !rung_vblank_ns(m)) {
		unlink_game(d);
		return;
	}
	if (game != d->game || d->link_gen != vrslink_generation) {
		unlink_game(d);
		d->game = game;
		d->link = vrslink_for_pid(client_get_pid(game));
		d->link_gen = vrslink_generation;
		d->budget_frac = DR_BUDGET_MAX;
	}
	if (!capped && !d->owner) {
		autolock_reset(m);
		ladder_start(m, now);
	}
	d->owner = !capped;
	track_level(d, now);
	if (d->owner)
		ladder_tick(m, now);
	publish(m, game);
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

void
dynrender_link_closed(const struct vrs_link *page)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link)
		if (m->dr.link == page)
			m->dr.link = NULL;
}

const char *
dynrender_diag(Monitor *m)
{
	static char buf[64];
	const struct vrs_link *l = m->dr.link;

	if (!m->dr.owner) {
		snprintf(buf, sizeof(buf), "dr=-");
		return buf;
	}
	snprintf(buf, sizeof(buf), "dr=%.0f lvl=%u gpu=%.1f/%.1f",
		rung_fps(m, m->dr.rung),
		l ? atomic_load(&l->level) : 0,
		l ? (float)atomic_load(&l->gpu_ns) / NS_PER_MS : 0.0f,
		l ? (float)atomic_load(&l->budget_ns) / NS_PER_MS : 0.0f);
	return buf;
}
