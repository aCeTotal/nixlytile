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
	Client *c;

	if (!m->wlr_output->enabled)
		return;

	/* Update HTPC view visibility when tags change */
	htpc_views_update_visibility(m);

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m) {
			wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));
			client_set_suspended(c, !VISIBLEON(c, m));
		}
	}

	/*
	 * fullscreen_bg is used to hide content behind fullscreen windows that
	 * don't fill the entire screen. However, for direct scanout to work
	 * (bypassing composition entirely), the fullscreen window must be the
	 * ONLY visible element. So we only show fullscreen_bg when the fullscreen
	 * window doesn't cover the full monitor area.
	 *
	 * This allows games and videos to achieve direct scanout for best
	 * frame pacing and lowest latency when they fill the screen.
	 */
	/* Find fullscreen client on this monitor's current tag */
	c = NULL;
	wl_list_for_each(c, &clients, link) {
		if (c->isfullscreen && VISIBLEON(c, m))
			break;
	}
	if (&c->link == &clients)
		c = NULL;

	if (c && c->isfullscreen) {
		/* Check if window covers the full monitor */
		int covers_full_screen =
			c->geom.x <= m->m.x &&
			c->geom.y <= m->m.y &&
			c->geom.x + c->geom.width >= m->m.x + m->m.width &&
			c->geom.y + c->geom.height >= m->m.y + m->m.height;

		/* Only show bg if fullscreen window doesn't cover everything */
		wlr_scene_node_set_enabled(&m->fullscreen_bg->node, !covers_full_screen);

		/*
		 * Hide ALL background elements when fullscreen covers the whole screen.
		 * This is CRITICAL for direct scanout - wlroots can only do direct
		 * scanout when there's exactly one visible surface covering the output.
		 * Any other visible scene nodes (root_bg, swaybg, etc.) prevent this.
		 */
		wlr_scene_node_set_enabled(&layers[LyrBg]->node, !covers_full_screen);
		wlr_scene_node_set_enabled(&root_bg->node, !covers_full_screen);

		/*
		 * AUTOMATIC FOCUS FOR FULLSCREEN CLIENTS
		 * If there's a visible fullscreen client on the current tag, it MUST
		 * have keyboard focus. This handles Steam games and other apps that
		 * go fullscreen but don't receive focus automatically.
		 */
		if (m == selmon && seat->keyboard_state.focused_surface != client_surface(c)) {
			exclusive_focus = NULL;
			focusclient(c, 0);  /* Don't lift/warp, just give focus */
		}
	} else {
		wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);
		wlr_scene_node_set_enabled(&layers[LyrBg]->node, 1);
		wlr_scene_node_set_enabled(&root_bg->node, 1);
	}

	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));

	/* We move all clients (except fullscreen and unmanaged) to LyrTile while
	 * in floating layout to avoid "real" floating clients be always on top */
	wl_list_for_each(c, &clients, link) {
		if (c->mon != m || c->scene->node.parent == layers[LyrFS])
			continue;

		wlr_scene_node_reparent(&c->scene->node,
				(!m->lt[m->sellt]->arrange && c->isfloating)
						? layers[LyrTile]
						: (m->lt[m->sellt]->arrange && c->isfloating)
								? layers[LyrFloat]
								: c->scene->node.parent);
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
