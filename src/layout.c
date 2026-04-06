#include "nixlytile.h"
#include "client.h"

void
hidetagthumbnail(Monitor *m)
{
	(void)m;
}

int
htpc_view_is_active(Monitor *m, unsigned int view_tag, int visible)
{
	if (!m || !htpc_mode_active || !visible || !view_tag)
		return 0;
	return (view_tag & m->tagset[m->seltags]) != 0;
}

void
htpc_views_update_visibility(Monitor *m)
{
	unsigned int tagset;

	if (!m || !htpc_mode_active)
		return;

	tagset = m->tagset[m->seltags];

	/* PC Gaming view - visible on tag 4 (bit 3) */
	if (m->pc_gaming.tree) {
		int vis = m->pc_gaming.visible && (m->pc_gaming.view_tag & tagset);
		wlr_scene_node_set_enabled(&m->pc_gaming.tree->node, vis);
	}

	/* Retro Gaming view - visible on tag 3 (bit 2) */
	if (m->retro_gaming.tree) {
		int vis = m->retro_gaming.visible && (m->retro_gaming.view_tag & tagset);
		wlr_scene_node_set_enabled(&m->retro_gaming.tree->node, vis);
		if (m->retro_gaming.dim)
			wlr_scene_node_set_enabled(&m->retro_gaming.dim->node, vis);
	}

	/* Movies view - visible on tag 2 (bit 1) */
	if (m->movies_view.tree) {
		int vis = m->movies_view.visible && (m->movies_view.view_tag & tagset);
		wlr_scene_node_set_enabled(&m->movies_view.tree->node, vis);
	}

	/* TV-shows view - visible on tag 1 (bit 0) */
	if (m->tvshows_view.tree) {
		int vis = m->tvshows_view.visible && (m->tvshows_view.view_tag & tagset);
		wlr_scene_node_set_enabled(&m->tvshows_view.tree->node, vis);
	}
}

void
arrange(Monitor *m)
{
	Client *c, *fsc;
	int covers_full_screen = 0;

	if (!m->wlr_output->enabled)
		return;

	/* Update HTPC view visibility when tags change */
	htpc_views_update_visibility(m);

	/*
	 * Find fullscreen client on this monitor BEFORE the per-client loop
	 * so we can hide other clients on the same monitor for direct scanout.
	 */
	fsc = NULL;
	wl_list_for_each(fsc, &clients, link) {
		if (fsc->isfullscreen && VISIBLEON(fsc, m))
			break;
	}
	if (&fsc->link == &clients)
		fsc = NULL;

	if (fsc) {
		covers_full_screen =
			fsc->geom.x <= m->m.x &&
			fsc->geom.y <= m->m.y &&
			fsc->geom.x + fsc->geom.width >= m->m.x + m->m.width &&
			fsc->geom.y + fsc->geom.height >= m->m.y + m->m.height;
	}

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m) {
			int vis = VISIBLEON(c, m);
			/*
			 * When a fullscreen client covers the entire monitor,
			 * disable all OTHER clients on this monitor.  This is
			 * needed for direct scanout — wlroots bypasses GPU
			 * composition only when the fullscreen surface is the
			 * single visible buffer on the output.
			 *
			 * We ONLY touch clients on THIS monitor — global layer
			 * nodes are shared across all monitors and must not be
			 * disabled here, or every other monitor goes black.
			 */
			if (fsc && c != fsc && covers_full_screen) {
				/* Keep children/descendants of the fullscreen client visible
				 * so dialogs, settings windows, and popups appear on top */
				Client *p = client_get_parent(c);
				int is_child = 0;
				int depth = 0;
				while (p && depth < 10) {
					if (p == fsc) { is_child = 1; break; }
					p = client_get_parent(p);
					depth++;
				}
				if (!is_child)
					vis = 0;
			}
			wlr_scene_node_set_enabled(&c->scene->node, vis);
			client_set_suspended(c, !vis);
		}
	}

	/*
	 * fullscreen_bg is per-monitor: show it when a fullscreen window
	 * doesn't fill the entire screen (letterboxed games, etc.).
	 */
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node,
		fsc && !covers_full_screen);

	if (fsc) {
		/*
		 * AUTOMATIC FOCUS FOR FULLSCREEN CLIENTS
		 * If there's a visible fullscreen client on the current tag, it MUST
		 * have keyboard focus. This handles Steam games and other apps that
		 * go fullscreen but don't receive focus automatically.
		 */
		if (m == selmon && seat->keyboard_state.focused_surface != client_surface(fsc)) {
			exclusive_focus = NULL;
			focusclient(fsc, 0);  /* Don't lift/warp, just give focus */
		}
	}

	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));

	/* We move all clients (except fullscreen and unmanaged) to LyrTile while
	 * in floating layout to avoid "real" floating clients be always on top */
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || c->scene->node.parent == layers[LyrFS])
			continue;

		struct wlr_scene_tree *target =
				(!m->lt[m->sellt]->arrange && c->isfloating)
						? layers[LyrTile]
						: (m->lt[m->sellt]->arrange && c->isfloating)
								? layers[LyrFloat]
								: NULL;
		if (target && c->scene->node.parent != target)
			wlr_scene_node_reparent(&c->scene->node, target);
	}

	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
	motionnotify(0, NULL, 0, 0, 0, 0);
	checkidleinhibitor(NULL);
	refreshstatustags();
	warpcursor(focustop(selmon));
}

void
arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive)
{
	LayerSurface *l;
	struct wlr_box full_area = m->m;

	wl_list_for_each(l, list, link) {
		struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;

		if (!layer_surface->initialized)
			continue;

		if (exclusive != (layer_surface->current.exclusive_zone > 0))
			continue;

		wlr_scene_layer_surface_v1_configure(l->scene_layer, &full_area, usable_area);
		wlr_scene_node_set_position(&l->popups->node, l->scene->node.x, l->scene->node.y);
	}
}

void
arrangelayers(Monitor *m)
{
	int i;
	struct wlr_box usable_area = m->m;
	struct wlr_box client_area;
	struct wlr_box old_w = m->w;
	LayerSurface *l;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	if (!m->wlr_output->enabled)
		return;

	/* Arrange exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 1);

	client_area = usable_area;
	layoutstatusbar(m, &usable_area, &client_area);

	if (!wlr_box_equal(&client_area, &old_w)) {
		m->w = client_area;
		arrange(m);
	} else {
		positionstatusmodules(m);
	}

	/* Arrange non-exlusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);

	refreshstatusclock();

	/* Find topmost keyboard interactive layer, if such a layer exists */
	for (i = 0; i < (int)LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(l, &m->layers[layers_above_shell[i]], link) {
			if (locked || !l->layer_surface->current.keyboard_interactive || !l->mapped)
				continue;
			/* Deactivate the focused client. */
			focusclient(NULL, 0);
			exclusive_focus = l;
			client_notify_enter(l->layer_surface->surface, wlr_seat_get_keyboard(seat));
			return;
		}
	}
}

void
incnmaster(const Arg *arg)
{
	if (!arg || !selmon)
		return;
	selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

void
monocle(Monitor *m)
{
	Client *c;
	int n = 0;

	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		resize(c, m->w, 0);
		n++;
	}
	if (n)
		snprintf(m->ltsymbol, LENGTH(m->ltsymbol), "[%d]", n);
	if ((c = focustop(m)))
		wlr_scene_node_raise_to_top(&c->scene->node);
}

void
setlayout(const Arg *arg)
{
	if (!selmon)
		return;
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
		selmon->sellt ^= 1;
	if (arg && arg->v)
		selmon->lt[selmon->sellt] = (Layout *)arg->v;
	strncpy(selmon->ltsymbol, selmon->lt[selmon->sellt]->symbol, LENGTH(selmon->ltsymbol));
	arrange(selmon);
	printstatus();
}

void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0f ? arg->f + selmon->mfact : arg->f - 1.0f;
	if (f < 0.1 || f > 0.9)
		return;
	selmon->mfact = f;
	arrange(selmon);
}

void
tile(Monitor *m)
{
	unsigned int h, r, e = m->gaps, mw, my, ty;
	int i, n = 0;
	Client *c;

	wl_list_for_each(c, &clients, link)
		if (VISIBLEON(c, m) && !c->isfloating && !c->isfullscreen)
			n++;
	if (n == 0)
		return;
	if (smartgaps == n)
		e = 0;

	/* When all windows fit in master area, arrange them horizontally as columns */
	if (n <= m->nmaster) {
		unsigned int w, mx = gappx * e;
		i = 0;
		wl_list_for_each(c, &clients, link) {
			if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
				continue;
			r = n - i;
			w = (m->w.width - mx - gappx * e - gappx * e * (r - 1)) / r;
			resize(c, (struct wlr_box){.x = m->w.x + mx, .y = m->w.y + gappx * e,
				.width = w, .height = m->w.height - 2 * gappx * e}, 0);
			mx += c->geom.width + gappx * e;
			i++;
		}
		return;
	}

	mw = m->nmaster ? (int)roundf((m->w.width + gappx * e) * m->mfact) : 0;
	i = 0;
	my = ty = gappx * e;
	wl_list_for_each(c, &clients, link) {
		if (!VISIBLEON(c, m) || c->isfloating || c->isfullscreen)
			continue;
		if (i < m->nmaster) {
			r = MIN(n, m->nmaster) - i;
			h = (m->w.height - my - gappx * e - gappx * e * (r - 1)) / r;
			resize(c, (struct wlr_box){.x = m->w.x + gappx * e, .y = m->w.y + my,
				.width = mw - 2 * gappx * e, .height = h}, 0);
			my += c->geom.height + gappx * e;
		} else {
			r = n - i;
			h = (m->w.height - ty - gappx * e - gappx * e * (r - 1)) / r;
			resize(c, (struct wlr_box){.x = m->w.x + mw, .y = m->w.y + ty,
				.width = m->w.width - mw - gappx * e, .height = h}, 0);
			ty += c->geom.height + gappx * e;
		}
		i++;
	}
}

void
togglegaps(const Arg *arg)
{
	if (!selmon)
		return;
	selmon->gaps = !selmon->gaps;
	arrangelayers(selmon);
	arrange(selmon);
}


int node_contains_client(LayoutNode *node, Client *c)
{
	if (!node)
		return 0;
	if (node->is_client_node)
		return node->client == c;
	return node_contains_client(node->left, c) || node_contains_client(node->right, c);
}

LayoutNode *
ancestor_split(LayoutNode *node, int want_vert)
{
	if (!node)
		return NULL;
	node = node->split_node;
	while (node) {
		if (!node->is_client_node && node->is_split_vertically == (unsigned int)want_vert)
			return node;
		node = node->split_node;
	}
	return NULL;
}
