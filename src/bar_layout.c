#include "nixlytile.h"

#define ISLAND_PAD BAR_INSET
#define ISLAND_GAP 8

typedef struct {
	StatusModule *mods[BARMOD_MAX];
	int n, w;
} Run;

typedef struct {
	Run run[BAR_SIDE_RUNS];
	int id, nruns, w;
} Side;

static void
start_run(Side *s)
{
	if (s->nruns < BAR_SIDE_RUNS)
		s->nruns++;
}

static void
add_module(Side *s, StatusModule *mod)
{
	Run *r;

	if (s->nruns == 0)
		s->nruns = 1;
	r = &s->run[s->nruns - 1];
	r->mods[r->n++] = mod;
	r->w += mod->width;
}

/* Workspaces get own island. */
static void
collect_side(Monitor *m, int id, Side *s)
{
	StatusModule *tags = &m->statusbar.tags, *prev = NULL;

	memset(s, 0, sizeof(*s));
	s->id = id;
	for (int i = 0; i < barmod_count(); i++) {
		const BarModCfg *cfg = barmod_get(i);
		StatusModule *mod = barmod_by_name(m, cfg->name);

		if (cfg->side != id || !mod || !mod->tree || mod->width <= 0)
			continue;
		if (prev == tags || mod == tags)
			start_run(s);
		add_module(s, mod);
		prev = mod;
	}
	for (int k = 0; k < s->nruns; k++) {
		s->run[k].w += 2 * ISLAND_PAD;
		s->w += s->run[k].w + (k ? ISLAND_GAP : 0);
	}
}

static void
place_run(Monitor *m, const Run *r, int x)
{
	StatusBar *bar = &m->statusbar;

	x += ISLAND_PAD;
	for (int i = 0; i < r->n; i++) {
		StatusModule *mod = r->mods[i];

		mod->x = x;
		mod->placed = 1;
		bar->placed[bar->nplaced++] = mod;
		nx_scene_node_set_position(&mod->tree->node, x, 0);
		x += mod->width;
	}
}

/* Content changes snap; new islands fade. */
static void
place_island(Monitor *m, BarPill *island, const BarSpan *s)
{
	int shown = island->a > 0.0;

	if (bar_pill_target(island, m, s) && shown)
		bar_pill_settle(island);
}

static void
place_side(Monitor *m, const Side *s, int x)
{
	BarPill *islands = &m->statusbar.pill[s->id * BAR_SIDE_RUNS];
	int h = m->statusbar.area.height;

	for (int k = 0; k < s->nruns; k++) {
		place_island(m, &islands[k], &(BarSpan){ x, 0, s->run[k].w, h });
		place_run(m, &s->run[k], x);
		x += s->run[k].w + ISLAND_GAP;
	}
	for (int k = s->nruns; k < BAR_SIDE_RUNS; k++)
		bar_pill_hide(&islands[k], m);
}

static void
hide_side(Monitor *m, int id)
{
	for (int k = 0; k < BAR_SIDE_RUNS; k++)
		bar_pill_hide(&m->statusbar.pill[id * BAR_SIDE_RUNS + k], m);
}

static void
place_ws_active(Monitor *m)
{
	StatusModule *tags = &m->statusbar.tags;
	int h = m->statusbar.area.height, a = tags->active_box;

	if (!tags->placed || a < 0 || a >= tags->box_count) {
		bar_pill_hide(&m->statusbar.pill[PILL_WS], m);
		return;
	}
	bar_pill_target(&m->statusbar.pill[PILL_WS], m, &(BarSpan){
		tags->box_x[a], BAR_INSET,
		tags->box_w[a], h - 2 * BAR_INSET });
}

static void
settle_first_layout(StatusBar *bar)
{
	if (bar->pills_settled)
		return;
	for (int i = 0; i < BAR_PILLS; i++)
		bar_pill_settle(&bar->pill[i]);
	bar->pills_settled = 1;
}

/* Middle island centres when room. */
void
bar_layout_modules(Monitor *m)
{
	StatusBar *bar = &m->statusbar;
	Side left, mid, right;
	int lo, hi;

	for (int i = 0; i < bar->nplaced; i++)
		bar->placed[i]->placed = 0;
	bar->nplaced = 0;
	collect_side(m, BAR_LEFT, &left);
	collect_side(m, BAR_MIDDLE, &mid);
	collect_side(m, BAR_RIGHT, &right);

	place_side(m, &left, 0);
	place_side(m, &right, bar->area.width - right.w);
	lo = left.w ? left.w + ISLAND_GAP : 0;
	hi = bar->area.width - (right.w ? right.w + ISLAND_GAP : 0) - mid.w;
	if (mid.w > 0 && hi >= lo)
		place_side(m, &mid, MAX(lo, MIN((bar->area.width - mid.w) / 2, hi)));
	else
		hide_side(m, BAR_MIDDLE);

	place_ws_active(m);
	settle_first_layout(bar);
	if (statusbar_hover_lit(m))
		statusbar_hover_update(m, cursor->x, cursor->y);
}
