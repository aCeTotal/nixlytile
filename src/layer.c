#include "nixlytile.h"
#include "client.h"

void
checkidleinhibitor(struct nx_surface *exclude)
{
	int inhibited = 0, unused_lx, unused_ly;
	struct nx_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
		struct nx_surface *surface = nx_surface_get_root_surface(inhibitor->surface);
		struct nx_scene_tree *tree = surface->data;
		if (exclude != surface && (bypass_surface_visibility || (!tree
				|| nx_scene_node_coords(&tree->node, &unused_lx, &unused_ly)))) {
			inhibited = 1;
			break;
		}
	}

	/* A running game or playing fullscreen video must never idle into
	 * the lockscreen (nixly-idled locks via ext-idle-notify), and games
	 * and some players never take a protocol inhibitor.  Re-evaluated
	 * every second from the idle-gate timer, since neither condition
	 * emits an event when it ends. */
	if (!inhibited && (game_mode_active || fullscreen_video_playing()))
		inhibited = 1;

	nx_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
}

void
commitlayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, surface_commit);
	struct nx_layer_surface_v1 *layer_surface = l->layer_surface;
	struct nx_scene_tree *scene_layer = layers[layermap[layer_surface->current.layer]];
	struct nx_layer_surface_v1_state old_state;

	if (l->layer_surface->initial_commit) {
		client_set_scale(layer_surface->surface, l->mon->nx_output->scale);

		/* Temporarily set the layer's current state to pending
		 * so that we can easily arrange it */
		old_state = l->layer_surface->current;
		l->layer_surface->current = l->layer_surface->pending;
		arrangelayers(l->mon);
		l->layer_surface->current = old_state;
		return;
	}

	if (layer_surface->current.committed == 0 && l->mapped == layer_surface->surface->mapped)
		return;
	l->mapped = layer_surface->surface->mapped;

	if (scene_layer != l->scene->node.parent) {
		nx_scene_node_reparent(&l->scene->node, scene_layer);
		wl_list_remove(&l->link);
		wl_list_insert(&l->mon->layers[layer_surface->current.layer], &l->link);
		nx_scene_node_reparent(&l->popups->node, (layer_surface->current.layer
				< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer));
	}

	arrangelayers(l->mon);
}

void
commitpopup(struct wl_listener *listener, void *data)
{
	struct nx_surface *surface = data;
	struct nx_xdg_popup *popup = nx_xdg_popup_try_from_nx_surface(surface);
	LayerSurface *l = NULL;
	Client *c = NULL;
	struct nx_box box;
	int type = -1;

	if (!popup->base->initial_commit)
		return;

	type = toplevel_from_nx_surface(popup->base->surface, &c, &l);
	if (!popup->parent || type < 0)
		return;
	popup->base->surface->data = nx_scene_xdg_surface_create(
			popup->parent->data, popup->base);
	if ((l && !l->mon) || (c && !c->mon)) {
		nx_xdg_popup_destroy(popup);
		return;
	}
	box = type == LayerShell ? l->mon->m : c->mon->w;
	box.x -= (type == LayerShell ? l->scene->node.x : c->geom.x);
	box.y -= (type == LayerShell ? l->scene->node.y : c->geom.y);
	nx_xdg_popup_unconstrain_from_box(popup, &box);
	wl_list_remove(&listener->link);
	free(listener);
}

void
createdecoration(struct wl_listener *listener, void *data)
{
	struct nx_xdg_toplevel_decoration_v1 *deco = data;
	Client *c = deco->toplevel->base->data;
	c->decoration = deco;

	LISTEN(&deco->events.request_mode, &c->set_decoration_mode, requestdecorationmode);
	LISTEN(&deco->events.destroy, &c->destroy_decoration, destroydecoration);

	requestdecorationmode(&c->set_decoration_mode, deco);
}

void
createidleinhibitor(struct wl_listener *listener, void *data)
{
	struct nx_idle_inhibitor_v1 *idle_inhibitor = data;
	LISTEN_STATIC(&idle_inhibitor->events.destroy, destroyidleinhibitor);

	checkidleinhibitor(NULL);
}

void
createlayersurface(struct wl_listener *listener, void *data)
{
	struct nx_layer_surface_v1 *layer_surface = data;
	LayerSurface *l;
	struct nx_surface *surface = layer_surface->surface;
	struct nx_scene_tree *scene_layer = layers[layermap[layer_surface->pending.layer]];

	if (!layer_surface->output
			&& !(layer_surface->output = selmon ? selmon->nx_output : NULL)) {
		nx_layer_surface_v1_destroy(layer_surface);
		return;
	}

	l = layer_surface->data = ecalloc(1, sizeof(*l));
	l->type = LayerShell;
	LISTEN(&surface->events.commit, &l->surface_commit, commitlayersurfacenotify);
	LISTEN(&surface->events.unmap, &l->unmap, unmaplayersurfacenotify);
	LISTEN(&layer_surface->events.destroy, &l->destroy, destroylayersurfacenotify);

	l->layer_surface = layer_surface;
	l->mon = layer_surface->output->data;
	l->scene_layer = nx_scene_layer_surface_v1_create(scene_layer, layer_surface);
	l->scene = l->scene_layer->tree;
	l->popups = surface->data = nx_scene_tree_create(layer_surface->current.layer
			< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer);
	l->scene->node.data = l->popups->node.data = l;

	wl_list_insert(&l->mon->layers[layer_surface->pending.layer],&l->link);
	nx_surface_send_enter(surface, layer_surface->output);
}

void
createlocksurface(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, new_surface);
	struct nx_session_lock_surface_v1 *lock_surface = data;
	Monitor *m = lock_surface->output->data;
	struct nx_scene_tree *scene_tree = lock_surface->surface->data
			= nx_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
	m->lock_surface = lock_surface;

	nx_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
	nx_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

	LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface, destroylocksurface);

	if (m == selmon)
		client_notify_enter(lock_surface->surface, nx_seat_get_keyboard(seat));
}

void
createpopup(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client (either xdg-shell or layer-shell)
	 * creates a new popup. */
	struct nx_xdg_popup *popup = data;
	LISTEN_STATIC(&popup->base->surface->events.commit, commitpopup);
}

void
destroyidleinhibitor(struct wl_listener *listener, void *data)
{
	/* `data` is the nx_surface of the idle inhibitor being destroyed,
	 * at this point the idle inhibitor is still in the list of the manager */
	checkidleinhibitor(nx_surface_get_root_surface(data));
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroylayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, destroy);

	wl_list_remove(&l->link);
	wl_list_remove(&l->destroy.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->surface_commit.link);
	/* surface->data points at l->popups (createlayersurface); the
	 * wl_surface can outlive this handler and checkidleinhibitor
	 * reads surface->data as a scene tree — clear before destroy. */
	if (l->layer_surface->surface->data == l->popups)
		l->layer_surface->surface->data = NULL;
	nx_scene_node_destroy(&l->scene->node);
	nx_scene_node_destroy(&l->popups->node);
	free(l);
}

void
destroylock(SessionLock *lock, int unlock)
{
	nx_seat_keyboard_notify_clear_focus(seat);
	if ((locked = !unlock))
		goto destroy;

	nx_scene_node_set_enabled(&locked_bg->node, 0);

	/* Lock forced max refresh. */
	powersave_reassert();

	focusclient(focustop(selmon), 0);
	motionnotify(0, NULL, 0, 0, 0, 0);

destroy:
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);

	nx_scene_node_destroy(&lock->scene->node);
	cur_lock = NULL;
	free(lock);
}

void
destroylocksurface(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
	struct nx_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

	m->lock_surface = NULL;
	wl_list_remove(&m->destroy_lock_surface.link);

	if (lock_surface->surface != seat->keyboard_state.focused_surface)
		return;

	if (locked && cur_lock && !wl_list_empty(&cur_lock->surfaces)) {
		surface = wl_container_of(cur_lock->surfaces.next, surface, link);
		client_notify_enter(surface->surface, nx_seat_get_keyboard(seat));
	} else if (!locked) {
		focusclient(focustop(selmon), 1);
	} else {
		nx_seat_keyboard_clear_focus(seat);
	}
}

void
destroysessionlock(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, destroy);
	destroylock(lock, 0);
}

void
locksession(struct wl_listener *listener, void *data)
{
	struct nx_session_lock_v1 *session_lock = data;
	SessionLock *lock;
	nx_scene_node_set_enabled(&locked_bg->node, 1);
	if (cur_lock) {
		nx_session_lock_v1_destroy(session_lock);
		return;
	}
	lock = session_lock->data = ecalloc(1, sizeof(*lock));
	focusclient(NULL, 0);

	lock->scene = nx_scene_tree_create(layers[LyrBlock]);
	cur_lock = lock->lock = session_lock;
	locked = 1;

	LISTEN(&session_lock->events.new_surface, &lock->new_surface, createlocksurface);
	LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
	LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

	/* Lockscreen animates every vblank — run the panel at its maximum
	 * refresh for the whole lock, battery included. */
	output_lock_max_refresh();

	nx_session_lock_v1_send_locked(session_lock);
}

void
requestdecorationmode(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_decoration_mode);
	if (c->surface.xdg->initialized)
		nx_xdg_toplevel_decoration_v1_set_mode(c->decoration,
				NX_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
unlocksession(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, unlock);
	destroylock(lock, 1);
}

void
unmaplayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, unmap);

	l->mapped = 0;
	nx_scene_node_set_enabled(&l->scene->node, 0);
	if (l == exclusive_focus)
		exclusive_focus = NULL;
	if (l->layer_surface->output && (l->mon = l->layer_surface->output->data))
		arrangelayers(l->mon);
	if (l->layer_surface->surface == seat->keyboard_state.focused_surface)
		focusclient(focustop(selmon), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

