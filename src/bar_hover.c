#include "nixlytile.h"

int
bar_module_hit(const StatusModule *mod, int lx)
{
	return mod->placed && lx >= mod->x && lx < mod->x + mod->width;
}

/* Workspace box index at module x, or -1. */
int
bar_ws_cell(const StatusModule *tags, int local)
{
	for (int i = 0; i < tags->box_count; i++)
		if (local >= tags->box_x[i] && local < tags->box_x[i] + tags->box_w[i])
			return i;
	return -1;
}

TrayItem *
bar_tray_item_at(int local)
{
	TrayItem *it;

	wl_list_for_each(it, &tray_items, link)
		if (it->w > 0 && local >= it->x && local < it->x + it->w)
			return it;
	return NULL;
}

static int
ws_cell_span(const StatusModule *tags, int local, BarSpan *s)
{
	int i = bar_ws_cell(tags, local);

	if (i < 0)
		return 0;
	s->x = tags->x + tags->box_x[i];
	s->w = tags->box_w[i];
	return 1;
}

static int
tray_cell_span(const StatusModule *tray, int local, BarSpan *s)
{
	TrayItem *it = bar_tray_item_at(local);

	if (!it)
		return 0;
	s->x = tray->x + it->x;
	s->w = it->w;
	return 1;
}

/* Interactive cell under bar x. */
static int
cell_at(StatusBar *bar, int lx, BarSpan *s)
{
	for (int i = 0; i < bar->nplaced; i++) {
		StatusModule *mod = bar->placed[i];

		if (mod == &bar->terminfo || !bar_module_hit(mod, lx))
			continue;
		if (mod == &bar->tags)
			return ws_cell_span(mod, lx - mod->x, s);
		if (mod == &bar->traylabel)
			return tray_cell_span(mod, lx - mod->x, s);
		s->x = mod->x;
		s->w = mod->width;
		return 1;
	}
	return 0;
}

/* Module with open popup. */
static int
owner_span(StatusBar *bar, BarSpan *s)
{
	const struct {
		int visible;
		StatusModule *mod;
	} owners[] = {
		{ bar->cpu_popup.visible, &bar->cpu },
		{ bar->ram_popup.visible, &bar->ram },
		{ bar->battery_popup.visible, &bar->battery },
		{ bar->net_popup.visible, &bar->net },
		{ bar->clock_popup.visible, &bar->clock },
		{ bar->volume_popup.visible, &bar->volume },
		{ bar->mic_popup.visible, &bar->mic },
		{ bar->light_popup.visible, &bar->light },
		{ bar->disk_popup.visible, &bar->disk },
		{ bar->bt_popup.visible, &bar->bluetooth },
		{ bar->display_popup.visible, &bar->display },
		{ bar->power_popup.visible, &bar->power },
	};

	for (size_t i = 0; i < LENGTH(owners); i++) {
		if (!owners[i].visible || !owners[i].mod->placed)
			continue;
		s->x = owners[i].mod->x;
		s->w = owners[i].mod->width;
		return 1;
	}
	return 0;
}

/* Only one monitor shows hover. */
void
statusbar_hover_focus(Monitor *m)
{
	Monitor *o;

	wl_list_for_each(o, &mons, link)
		if (o != m)
			bar_pill_hide(&o->statusbar.pill[PILL_HOVER], o);
}

int
statusbar_hover_lit(Monitor *m)
{
	return m->statusbar.pill[PILL_HOVER].ta > 0.0;
}

void
statusbar_hover_update(Monitor *m, double cx, double cy)
{
	StatusBar *bar = &m->statusbar;
	BarPill *hover = &bar->pill[PILL_HOVER];
	int lx = (int)floor(cx) - bar->area.x;
	int ly = (int)floor(cy) - bar->area.y;
	int over = ly >= 0 && ly < bar->area.height;
	BarSpan s = { 0, BAR_INSET, 0, bar->area.height - 2 * BAR_INSET };

	statusbar_hover_focus(m);
	if (m->showbar && ((over && cell_at(bar, lx, &s)) || owner_span(bar, &s)))
		bar_pill_target(hover, m, &s);
	else
		bar_pill_hide(hover, m);
}
