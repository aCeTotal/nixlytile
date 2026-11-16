#include "nixlytile.h"
#include "vrslayer/link.h"

/* Spanned games share one page. */
static int
held_elsewhere(Monitor *mon, const struct vrs_link *page)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link)
		if (m != mon && m->fov.page == page)
			return 1;
	return 0;
}

static void
relink(Monitor *m, Client *game)
{
	Foveation *f = &m->fov;
	struct vrs_link *page = game ? vrslink_for_pid(client_get_pid(game)) : NULL;

	if (f->page && f->page != page && !held_elsewhere(m, f->page))
		vrslink_idle(f->page);
	f->page = page;
	f->game = game;
	f->gen = vrslink_generation;
}

/* Sole owner of page lifecycle. */
void
foveation_update(Monitor *m, Client *game)
{
	Foveation *f = &m->fov;

	if (!dynamic_render_enabled)
		game = NULL;
	if (game != f->game || f->gen != vrslink_generation)
		relink(m, game);
	if (f->page)
		vrslink_publish(f->page, gaze_focus(m, f->game));
}

void
foveation_page_closed(const struct vrs_link *page)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link)
		if (m->fov.page == page)
			m->fov.page = NULL;
}
