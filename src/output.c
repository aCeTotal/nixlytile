#include "nixlytile.h"
#include "client.h"

static int idle_heartbeat_cb(void *data);

void
cleanupmon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy);
	LayerSurface *l, *tmp;
	size_t i;

	modal_file_search_stop(m);

	/* If a laptop display is being removed, clear is_mirror on any output
	 * that was mirroring it so it becomes an independent display. */
	if (strncmp(m->wlr_output->name, "eDP", 3) == 0 ||
	    strncmp(m->wlr_output->name, "LVDS", 4) == 0) {
		Monitor *other;
		wl_list_for_each(other, &mons, link) {
			if (other->is_mirror)
				other->is_mirror = 0;
		}
	}

	/* m->layers[i] are intentionally not unlinked */
	for (i = 0; i < LENGTH(m->layers); i++) {
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->layer_surface);
	}

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->present.link);
	wl_list_remove(&m->link);
	wl_list_remove(&m->request_state.link);
	if (m->lock_surface)
		destroylocksurface(&m->destroy_lock_surface, NULL);
	m->wlr_output->data = NULL;
	wlr_output_layout_remove(output_layout, m->wlr_output);
	wlr_scene_output_destroy(m->scene_output);

	if (m->statusbar.tree)
		wlr_scene_node_destroy(&m->statusbar.tree->node);
	if (m->modal.tree)
		wlr_scene_node_destroy(&m->modal.tree->node);
	if (m->nixpkgs.tree)
		wlr_scene_node_destroy(&m->nixpkgs.tree->node);
	if (m->wifi_popup.tree)
		wlr_scene_node_destroy(&m->wifi_popup.tree->node);
	if (m->toast_overlay_buf) {
		cpu_cursor_buffer_destroy(m->toast_overlay_buf);
		m->toast_overlay_buf = NULL;
	}
	if (m->toast_overlay_layer) {
		wlr_output_layer_destroy(m->toast_overlay_layer);
		m->toast_overlay_layer = NULL;
	}
	destroy_tree(m);
	closemon(m);
	ll_cursor_cleanup(m);
	wl_event_source_remove(m->idle_heartbeat);
	wlr_scene_node_destroy(&m->fullscreen_bg->node);
	free(m);

	/* Re-arrange remaining monitors after removal */
	auto_arrange_monitors();
}

void
closemon(Monitor *m)
{
	/* update selmon if needed and
	 * move closed monitor's clients to the focused one */
	Client *c;
	int i = 0, nmons = wl_list_length(&mons);
	if (!nmons) {
		selmon = NULL;
	} else if (m == selmon) {
		do /* don't switch to disabled or mirror mons */
			selmon = wl_container_of(mons.next, selmon, link);
		while ((!selmon->wlr_output->enabled || selmon->is_mirror) && i++ < nmons);

		if (!selmon->wlr_output->enabled || selmon->is_mirror)
			selmon = NULL;
	}

	wl_list_for_each(c, &clients, link) {
		if (c->isfloating && c->geom.x > m->m.width)
			resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
					.width = c->geom.width, .height = c->geom.height}, 0);
		if (c->mon == m)
			setmon(c, selmon, c->tags);
	}
	focusclient(focustop(selmon), 1);
	printstatus();
}

struct wlr_output_mode *
bestmode(struct wlr_output *output)
{
	struct wlr_output_mode *mode, *best = NULL;
	int best_area = 0, best_refresh = 0;

	wl_list_for_each(mode, &output->modes, link) {
		/* Skip DCI 4K (4096x2160) - only use UHD (3840x2160) */
		if (mode->width > 3840 && mode->height == 2160)
			continue;
		int area = mode->width * mode->height;
		if (!best || area > best_area || (area == best_area && mode->refresh > best_refresh)) {
			best = mode;
			best_area = area;
			best_refresh = mode->refresh;
		}
	}

	if (!best)
		best = wlr_output_preferred_mode(output);

	return best;
}

struct wlr_output_mode *
find_mode(struct wlr_output *output, int width, int height, float refresh)
{
	struct wlr_output_mode *mode, *best = NULL;
	int best_refresh = 0;

	wl_list_for_each(mode, &output->modes, link) {
		if (mode->width == width && mode->height == height) {
			if (refresh > 0) {
				/* Find closest match to requested refresh rate */
				int mode_hz = mode->refresh / 1000;
				int target_hz = (int)refresh;
				if (!best || abs(mode_hz - target_hz) < abs(best_refresh - target_hz)) {
					best = mode;
					best_refresh = mode_hz;
				}
			} else {
				/* No refresh specified - pick highest */
				if (!best || mode->refresh > best->refresh) {
					best = mode;
					best_refresh = mode->refresh / 1000;
				}
			}
		}
	}
	return best;
}

static Monitor *
find_laptop_monitor(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (strncmp(m->wlr_output->name, "eDP", 3) == 0 ||
		    strncmp(m->wlr_output->name, "LVDS", 4) == 0)
			return m;
	}
	return NULL;
}

static int
is_external_connector(const char *name)
{
	return strncmp(name, "HDMI", 4) == 0 ||
	       strncmp(name, "DP", 2) == 0;
}

static void
restart_wallpaper(void)
{
	char expanded_wp[PATH_MAX];
	config_expand_path(wallpaper_path, expanded_wp, sizeof(expanded_wp));
	wlr_log(WLR_INFO, "Restarting wallpaper: %s", expanded_wp);

	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		(void)system("pkill -9 swaybg 2>/dev/null; sleep 0.3");
		execlp("swaybg", "swaybg", "-i", expanded_wp, "-m", "fill", (char *)NULL);
		_exit(127);
	}
}

/* ── auto-arrange helpers ──────────────────────────────────────────── */

static double
monitor_physical_diagonal(Monitor *m)
{
	int pw = m->wlr_output->phys_width;
	int ph = m->wlr_output->phys_height;
	if (pw <= 0 || ph <= 0)
		return 0.0;
	return sqrt((double)pw * pw + (double)ph * ph);
}

void
monitor_effective_size(Monitor *m, int *w, int *h)
{
	enum wl_output_transform t = m->wlr_output->transform;
	int mw = m->wlr_output->width;
	int mh = m->wlr_output->height;

	if (t == WL_OUTPUT_TRANSFORM_90 || t == WL_OUTPUT_TRANSFORM_270 ||
	    t == WL_OUTPUT_TRANSFORM_FLIPPED_90 || t == WL_OUTPUT_TRANSFORM_FLIPPED_270) {
		*w = mh;
		*h = mw;
	} else {
		*w = mw;
		*h = mh;
	}
}

typedef struct {
	Monitor *mon;
	int eff_w, eff_h;
	double phys_diag;
	int32_t phys_w, phys_h;
	char edid_key[256];
} MonitorProbe;

static void
build_edid_key(Monitor *m, char *buf, size_t len)
{
	const char *model = m->wlr_output->model ? m->wlr_output->model : "unknown";
	const char *serial = m->wlr_output->serial ? m->wlr_output->serial : "none";
	int w, h;
	monitor_effective_size(m, &w, &h);
	double diag = monitor_physical_diagonal(m);
	snprintf(buf, len, "%s|%s|%dx%d|%.0f", model, serial, w, h, diag);
}

static int
find_center_monitor(MonitorProbe *probes, int n)
{
	if (n < 2)
		return 0;

	int best = -1;
	double best_diag = 0.0;
	double sum = 0.0;
	int cnt = 0;

	for (int i = 0; i < n; i++) {
		if (probes[i].phys_diag > 0.0) {
			sum += probes[i].phys_diag;
			cnt++;
			if (probes[i].phys_diag > best_diag) {
				best_diag = probes[i].phys_diag;
				best = i;
			}
		}
	}

	if (best >= 0 && cnt >= 2) {
		double avg_others = (sum - best_diag) / (cnt - 1);
		if (best_diag > avg_others * 1.2)
			return best;
	}

	return n / 2;
}

static int arranging_monitors; /* re-entrancy guard */

void
auto_arrange_monitors(void)
{
	Monitor *m;
	MonitorProbe probes[MAX_MONITORS];
	int n = 0;

	if (arranging_monitors)
		return;
	arranging_monitors = 1;

	/* Skip if user has explicit monitor configuration */
	if (runtime_monitor_count > 0) {
		arranging_monitors = 0;
		return;
	}

	/* Phase 1: Collect enabled, non-mirror monitors */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled && !m->is_mirror && n < MAX_MONITORS) {
			MonitorProbe *p = &probes[n];
			p->mon = m;
			monitor_effective_size(m, &p->eff_w, &p->eff_h);
			p->phys_diag = monitor_physical_diagonal(m);
			p->phys_w = m->wlr_output->phys_width;
			p->phys_h = m->wlr_output->phys_height;
			build_edid_key(m, p->edid_key, sizeof(p->edid_key));
			n++;
		}
	}

	if (n == 0) {
		arranging_monitors = 0;
		return;
	}

	/* Single monitor: trivial case */
	if (n == 1) {
		wlr_output_layout_add(output_layout, probes[0].mon->wlr_output, 0, 0);
		wlr_log(WLR_INFO, "Auto-arrange: %s (%s) at (0, 0) [%dx%d] [CENTER]",
			probes[0].mon->wlr_output->name, probes[0].edid_key,
			probes[0].eff_w, probes[0].eff_h);
		selmon = probes[0].mon;
		wlr_cursor_warp_closest(cursor, NULL,
			probes[0].eff_w / 2, probes[0].eff_h / 2);
		arranging_monitors = 0;
		return;
	}

	/* Phase 2: Temporary auto-placement for probing */
	for (int i = 0; i < n; i++)
		wlr_output_layout_add_auto(output_layout, probes[i].mon->wlr_output);

	/* Find start/center monitor: biggest EDID physical diagonal */
	int start_idx = find_center_monitor(probes, n);

	/* Phase 3: Invisible cursor sweep — chain-walk left and right
	 * from center monitor to discover the spatial order.
	 * The probe coordinate (x,y) acts as an invisible cursor that
	 * sweeps across monitor edges without moving the real cursor. */
	Monitor *left_chain[MAX_MONITORS];
	int left_n = 0;
	Monitor *right_chain[MAX_MONITORS];
	int right_n = 0;

	/* Walk LEFT from start monitor */
	Monitor *cur = probes[start_idx].mon;
	while (left_n < n - 1) {
		struct wlr_box box;
		wlr_output_layout_get_box(output_layout, cur->wlr_output, &box);
		if (box.width <= 0)
			break;
		int mid_y = box.y + box.height / 2;
		struct wlr_output *o = wlr_output_layout_output_at(
			output_layout, box.x - 1, mid_y);
		if (!o || !o->data)
			break;
		Monitor *found = o->data;
		if (found == probes[start_idx].mon)
			break;
		int dup = 0;
		for (int i = 0; i < left_n; i++) {
			if (left_chain[i] == found) { dup = 1; break; }
		}
		if (dup)
			break;
		left_chain[left_n++] = found;
		cur = found;
	}

	/* Walk RIGHT from start monitor */
	cur = probes[start_idx].mon;
	while (right_n < n - 1) {
		struct wlr_box box;
		wlr_output_layout_get_box(output_layout, cur->wlr_output, &box);
		if (box.width <= 0)
			break;
		int mid_y = box.y + box.height / 2;
		struct wlr_output *o = wlr_output_layout_output_at(
			output_layout, box.x + box.width, mid_y);
		if (!o || !o->data)
			break;
		Monitor *found = o->data;
		if (found == probes[start_idx].mon)
			break;
		int dup = 0;
		for (int i = 0; i < right_n; i++) {
			if (right_chain[i] == found) { dup = 1; break; }
		}
		if (dup)
			break;
		right_chain[right_n++] = found;
		cur = found;
	}

	/* Build ordered array: [...left(reversed)] [CENTER] [...right] */
	Monitor *ordered[MAX_MONITORS];
	int ordered_n = 0;
	for (int i = left_n - 1; i >= 0; i--)
		ordered[ordered_n++] = left_chain[i];
	int center_pos = ordered_n;
	ordered[ordered_n++] = probes[start_idx].mon;
	for (int i = 0; i < right_n; i++)
		ordered[ordered_n++] = right_chain[i];

	/* Append any monitors the sweep missed (e.g. stacked vertically) */
	for (int i = 0; i < n; i++) {
		int found = 0;
		for (int j = 0; j < ordered_n; j++) {
			if (ordered[j] == probes[i].mon) { found = 1; break; }
		}
		if (!found && ordered_n < MAX_MONITORS)
			ordered[ordered_n++] = probes[i].mon;
	}

	wlr_log(WLR_INFO, "Auto-arrange: probe sweep from %s: "
		"%d left, %d right, %d total",
		probes[start_idx].mon->wlr_output->name,
		left_n, right_n, ordered_n);

	/* Phase 4: Compute positions */
	int eff_w[MAX_MONITORS], eff_h[MAX_MONITORS];
	int max_height = 0;
	for (int i = 0; i < ordered_n; i++) {
		monitor_effective_size(ordered[i], &eff_w[i], &eff_h[i]);
		if (eff_h[i] > max_height)
			max_height = eff_h[i];
	}

	int pos_x[MAX_MONITORS], pos_y[MAX_MONITORS];
	pos_x[center_pos] = 0;

	/* Left of center */
	int x = 0;
	for (int i = center_pos - 1; i >= 0; i--) {
		x -= eff_w[i];
		pos_x[i] = x;
	}

	/* Right of center */
	x = eff_w[center_pos];
	for (int i = center_pos + 1; i < ordered_n; i++) {
		pos_x[i] = x;
		x += eff_w[i];
	}

	/* Vertical centering */
	for (int i = 0; i < ordered_n; i++)
		pos_y[i] = (max_height - eff_h[i]) / 2;

	/* Phase 5: Apply positions */
	for (int i = 0; i < ordered_n; i++) {
		wlr_output_layout_add(output_layout, ordered[i]->wlr_output,
			pos_x[i], pos_y[i]);
		char key[256];
		build_edid_key(ordered[i], key, sizeof(key));
		wlr_log(WLR_INFO, "Auto-arrange: %s (%s) at (%d, %d) [%dx%d]%s",
			ordered[i]->wlr_output->name, key,
			pos_x[i], pos_y[i], eff_w[i], eff_h[i],
			i == center_pos ? " [CENTER]" : "");
	}

	/* Phase 6: Cursor + selmon */
	selmon = ordered[center_pos];
	int cx = pos_x[center_pos] + eff_w[center_pos] / 2;
	int cy = pos_y[center_pos] + eff_h[center_pos] / 2;
	wlr_cursor_warp_closest(cursor, NULL, cx, cy);

	arranging_monitors = 0;
}

void
warp_cursor_to_startup_monitor(void)
{
	/*
	 * Choose the best monitor for initial cursor placement:
	 *   - Odd monitor count → physically center monitor (by x position)
	 *   - Even count / 2 monitors → highest resolution (pixel count)
	 * Called once at startup after all monitors are positioned.
	 */
	Monitor *m, *best = NULL;
	Monitor *list[MAX_MONITORS];
	int n = 0;

	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled && !m->is_mirror && n < MAX_MONITORS)
			list[n++] = m;
	}

	if (n == 0)
		return;

	if (n == 1) {
		best = list[0];
	} else {
		/* Sort by x position (bubble sort, n is small) */
		for (int i = 0; i < n - 1; i++) {
			for (int j = i + 1; j < n; j++) {
				if (list[j]->m.x < list[i]->m.x) {
					Monitor *tmp = list[i];
					list[i] = list[j];
					list[j] = tmp;
				}
			}
		}

		if (n % 2 == 1) {
			/* Odd: pick the physically center monitor */
			best = list[n / 2];
		} else {
			/* Even: pick highest resolution */
			best = list[0];
			int best_px = best->m.width * best->m.height;
			for (int i = 1; i < n; i++) {
				int px = list[i]->m.width * list[i]->m.height;
				if (px > best_px) {
					best_px = px;
					best = list[i];
				}
			}
		}
	}

	selmon = best;
	wlr_cursor_warp_closest(cursor, NULL,
		best->m.x + best->m.width / 2,
		best->m.y + best->m.height / 2);
	wlr_log(WLR_INFO, "Startup cursor → monitor '%s' (%dx%d @ %d,%d)",
		best->wlr_output->name,
		best->m.width, best->m.height, best->m.x, best->m.y);
}

void
createmon(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct wlr_output *wlr_output = data;
	const MonitorRule *r;
	RuntimeMonitorConfig *rtcfg;
	size_t i;
	struct wlr_output_state state;
	struct wlr_output_mode *mode;
	Monitor *m;
	Client *c;
	int use_runtime_config = 0;

	/* Log multi-GPU topology for diagnostics */
	if (wlr_output_is_drm(wlr_output)) {
		struct wlr_backend *parent =
			wlr_drm_backend_get_parent(wlr_output->backend);
		if (parent) {
			wlr_log(WLR_INFO, "Output %s is on secondary GPU (multi-GPU / mgpu)",
				wlr_output->name);
		}
	}

	if (!wlr_output_init_render(wlr_output, alloc, drw)) {
		wlr_log(WLR_ERROR, "Failed to init render for output %s — "
			"allocator/renderer buffer caps mismatch", wlr_output->name);
		return;
	}

	m = wlr_output->data = ecalloc(1, sizeof(*m));
	m->wlr_output = wlr_output;

	for (i = 0; i < LENGTH(m->layers); i++)
	wl_list_init(&m->layers[i]);
	initstatusbar(m);

	wlr_output_state_init(&state);
	/* Initialize monitor state using configured rules */
	m->gaps = gaps;

	m->tagset[0] = m->tagset[1] = 1;

	/* First check runtime monitor configuration */
	rtcfg = find_monitor_config(wlr_output->name);
	if (rtcfg) {
		use_runtime_config = 1;
		wlr_log(WLR_INFO, "Using runtime config for monitor %s", wlr_output->name);

		/* Check if monitor is disabled */
		if (!rtcfg->enabled) {
			wlr_log(WLR_INFO, "Monitor %s is disabled in config", wlr_output->name);
			wlr_output_state_set_enabled(&state, 0);
			wlr_output_commit_state(wlr_output, &state);
			wlr_output_state_finish(&state);
			free(m);
			wlr_output->data = NULL;
			return;
		}

		/* Apply runtime config */
		m->mfact = rtcfg->mfact;
		m->nmaster = rtcfg->nmaster;
		m->lt[0] = &layouts[0];
		m->lt[1] = &layouts[nlayouts > 1 ? 1 : 0];
		strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));
		wlr_output_state_set_scale(&state, rtcfg->scale);
		wlr_output_state_set_transform(&state, rtcfg->transform);

		/* Position will be calculated after mode is set */
		m->m.x = -1;
		m->m.y = -1;

		/* Find mode if resolution is specified */
		if (rtcfg->width > 0 && rtcfg->height > 0) {
			mode = find_mode(wlr_output, rtcfg->width, rtcfg->height, rtcfg->refresh);
			if (mode) {
				wlr_output_state_set_mode(&state, mode);
				wlr_log(WLR_INFO, "Set mode %dx%d@%d for %s",
					mode->width, mode->height, mode->refresh / 1000, wlr_output->name);
			} else {
				wlr_log(WLR_ERROR, "Mode %dx%d not found for %s, using auto",
					rtcfg->width, rtcfg->height, wlr_output->name);
				if ((mode = bestmode(wlr_output)))
					wlr_output_state_set_mode(&state, mode);
			}
		} else {
			/* Auto mode - pick best */
			if ((mode = bestmode(wlr_output)))
				wlr_output_state_set_mode(&state, mode);
		}
	}

	/* Fall back to compile-time monrules if no runtime config */
	if (!use_runtime_config) {
		for (r = monrules; r < monrules + nmonrules; r++) {
			if (!r->name || strstr(wlr_output->name, r->name)) {
				m->m.x = r->x;
				m->m.y = r->y;
				m->mfact = r->mfact;
				m->nmaster = r->nmaster;
				m->lt[0] = r->lt;
				m->lt[1] = &layouts[nlayouts > 1 && r->lt != &layouts[1]];
				strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, LENGTH(m->ltsymbol));
				wlr_output_state_set_scale(&state, r->scale);
				wlr_output_state_set_transform(&state, r->rr);
				break;
			}
		}

		/* The mode is a tuple of (width, height, refresh rate), and each
		 * monitor supports only a specific set of modes. Pick the highest
		 * resolution and refresh rate available. */
		if ((mode = bestmode(wlr_output)))
			wlr_output_state_set_mode(&state, mode);
	}

	/* Set up event listeners */
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);
	LISTEN(&wlr_output->events.request_state, &m->request_state, requestmonstate);
	LISTEN(&wlr_output->events.present, &m->present, outputpresent);

	/* Find laptop monitor for mode fallback: if the best mode on an external
	 * display fails (bandwidth exceeded), try the laptop's resolution. */
	Monitor *laptop = NULL;
	if (!use_runtime_config && is_external_connector(wlr_output->name))
		laptop = find_laptop_monitor();

	wlr_output_state_set_enabled(&state, 1);

	/* Test the state before committing to avoid GPU hangs.
	 * If the best mode fails (bandwidth exceeded), fall back to
	 * the laptop's resolution, then the output's preferred mode. */
	if (!wlr_output_test_state(wlr_output, &state)) {
		wlr_log(WLR_INFO, "Mode test failed for %s, trying fallback modes",
			wlr_output->name);

		if (laptop && laptop->wlr_output->current_mode) {
			mode = find_mode(wlr_output,
				laptop->wlr_output->current_mode->width,
				laptop->wlr_output->current_mode->height, 0);
			if (mode) {
				wlr_output_state_set_mode(&state, mode);
				wlr_log(WLR_INFO, "Trying laptop resolution %dx%d for %s",
					mode->width, mode->height, wlr_output->name);
			}
		}

		if (!wlr_output_test_state(wlr_output, &state)) {
			mode = wlr_output_preferred_mode(wlr_output);
			if (mode) {
				wlr_output_state_set_mode(&state, mode);
				wlr_log(WLR_INFO, "Trying preferred mode for %s", wlr_output->name);
			}
		}
	}

	if (!wlr_output_commit_state(wlr_output, &state)) {
		wlr_log(WLR_ERROR, "Failed to commit output state for %s, disabling",
			wlr_output->name);
		wlr_output_state_finish(&state);
		wl_list_remove(&m->frame.link);
		wl_list_remove(&m->destroy.link);
		wl_list_remove(&m->request_state.link);
		wl_list_remove(&m->present.link);
		free(m);
		wlr_output->data = NULL;
		return;
	}
	wlr_output_state_finish(&state);

	/* Check VRR capability - try to enable adaptive sync to test support */
	m->vrr_capable = 0;
	m->vrr_active = 0;
	m->vrr_target_hz = 0.0f;
	/* Initialize game VRR fields */
	m->game_vrr_active = 0;
	m->game_vrr_target_fps = 0.0f;
	m->game_vrr_last_fps = 0.0f;
	m->game_vrr_last_change_ns = 0;
	m->game_vrr_stable_frames = 0;
	if (wlr_output_is_drm(wlr_output)) {
		struct wlr_output_state vrr_test;
		wlr_output_state_init(&vrr_test);
		wlr_output_state_set_adaptive_sync_enabled(&vrr_test, 1);
		if (wlr_output_test_state(wlr_output, &vrr_test)) {
			m->vrr_capable = 1;
			wlr_log(WLR_DEBUG, "Monitor %s supports VRR/Adaptive Sync", wlr_output->name);
		}
		wlr_output_state_finish(&vrr_test);
	}

	/* Low-latency cursor: discover cursor DRM plane for direct atomic commits */
	ll_cursor_init(m);

	/* Gaming capability profile: vendor, overlay planes, LFC, explicit sync.
	 * Populated once here; consumed by update_game_vrr (LFC warning),
	 * toast/OSD overlay promotion, and various diagnostic logs. */
	memset(&m->gcaps, 0, sizeof(m->gcaps));
	m->gcaps.explicit_sync_ready = g_explicit_sync_ok;
	if (discrete_gpu_idx >= 0) {
		m->gcaps.vendor = detected_gpus[discrete_gpu_idx].vendor;
	} else if (integrated_gpu_idx >= 0) {
		m->gcaps.vendor = detected_gpus[integrated_gpu_idx].vendor;
	} else {
		m->gcaps.vendor = GPU_VENDOR_UNKNOWN;
	}
	m->gcaps.has_hw_lfc = (m->gcaps.vendor == GPU_VENDOR_AMD ||
	                       m->gcaps.vendor == GPU_VENDOR_INTEL);
	m->gcaps.prefers_vulkan = (m->gcaps.vendor == GPU_VENDOR_NVIDIA);

	/* Overlay-plane probe: try to attach a dummy layer to a test state.
	 * NVIDIA's proprietary driver exposes only primary+cursor planes today,
	 * so this will reject; AMD/Intel typically accept the probe.
	 * Failure here is expected and non-fatal. */
	m->gcaps.overlay_planes_supported = 0;
	if (wlr_output_is_drm(wlr_output)) {
		const struct wlr_drm_format_set *primary_formats =
			wlr_output_get_primary_formats(wlr_output, WLR_BUFFER_CAP_DMABUF);
		const struct wlr_drm_format *probe_fmt = NULL;
		if (primary_formats) {
			probe_fmt = wlr_drm_format_set_get(primary_formats, DRM_FORMAT_ARGB8888);
			if (!probe_fmt)
				probe_fmt = wlr_drm_format_set_get(primary_formats, DRM_FORMAT_XRGB8888);
			if (!probe_fmt && primary_formats->len > 0)
				probe_fmt = &primary_formats->formats[0];
		}
		if (probe_fmt) {
			struct wlr_buffer *probe_buf =
				wlr_allocator_create_buffer(alloc, 64, 64, probe_fmt);
			if (probe_buf) {
				struct wlr_output_layer *probe_layer =
					wlr_output_layer_create(wlr_output);
				if (probe_layer) {
					struct wlr_output_state probe_state;
					struct wlr_output_layer_state layer_state = {
						.layer = probe_layer,
						.buffer = probe_buf,
						.src_box = { .x = 0, .y = 0, .width = 64, .height = 64 },
						.dst_box = { .x = 0, .y = 0, .width = 64, .height = 64 },
					};
					wlr_output_state_init(&probe_state);
					wlr_output_state_set_layers(&probe_state, &layer_state, 1);
					if (wlr_output_test_state(wlr_output, &probe_state) &&
					    layer_state.accepted) {
						m->gcaps.overlay_planes_supported = 1;
					}
					wlr_output_state_finish(&probe_state);
					wlr_output_layer_destroy(probe_layer);
				}
				wlr_buffer_drop(probe_buf);
			}
		}
	}

	{
		const char *vendor_str =
			m->gcaps.vendor == GPU_VENDOR_NVIDIA ? "NVIDIA" :
			m->gcaps.vendor == GPU_VENDOR_AMD ? "AMD" :
			m->gcaps.vendor == GPU_VENDOR_INTEL ? "Intel" : "unknown";
		wlr_log(WLR_INFO,
			"Monitor %s gaming caps: vendor=%s overlay_planes=%s "
			"esync=%s vulkan_pref=%s lfc=%s",
			wlr_output->name, vendor_str,
			m->gcaps.overlay_planes_supported ? "YES" : "NO",
			m->gcaps.explicit_sync_ready ? "YES" : "NO",
			m->gcaps.prefers_vulkan ? "YES" : "NO",
			m->gcaps.has_hw_lfc ? "HW" : "none");
	}

	wl_list_insert(&mons, &m->link);
	printstatus();
	init_tree(m);
	modal_prewarm(m);

	/* Idle monitor render throttle */
	m->idle_heartbeat = wl_event_loop_add_timer(event_loop, idle_heartbeat_cb, m);
	m->render_idle = 0;
	m->idle_frames = 0;

	/* The xdg-protocol specifies:
	 *
	 * If the fullscreened surface is not opaque, the compositor must make
	 * sure that other screen content not part of the same surface tree (made
	 * up of subsurfaces, popups or similarly coupled surfaces) are not
	 * visible below the fullscreened surface.
	 *
	 */
	/* updatemons() will resize and set correct position */
	m->fullscreen_bg = wlr_scene_rect_create(layers[LyrFS], 0, 0, fullscreen_bg);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

	/* Adds this to the output layout in the order it was configured.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	m->scene_output = wlr_scene_output_create(scene, wlr_output);

	/* Calculate position for runtime-configured monitors */
	if (use_runtime_config && rtcfg && rtcfg->position != MON_POS_AUTO) {
		int pos_x, pos_y;
		/* Get monitor dimensions from the committed mode */
		if (wlr_output->current_mode) {
			m->m.width = wlr_output->current_mode->width;
			m->m.height = wlr_output->current_mode->height;
		}
		calculate_monitor_position(m, rtcfg, &pos_x, &pos_y);
		if (pos_x != -1 && pos_y != -1) {
			m->m.x = pos_x;
			m->m.y = pos_y;
			wlr_log(WLR_INFO, "Positioning monitor %s at %d,%d (slot: %d)",
				wlr_output->name, pos_x, pos_y, rtcfg->position);
		}
	}

	/* If runtime config placed this monitor explicitly, use that position.
	 * Otherwise auto_arrange_monitors() will position all monitors. */
	if (m->m.x != -1 && m->m.y != -1)
		wlr_output_layout_add(output_layout, wlr_output, m->m.x, m->m.y);
	else
		auto_arrange_monitors();

	wl_list_for_each(c, &clients, link) {
		if (c->output && strcmp(wlr_output->name, c->output) == 0)
			c->mon = m;
	}

	/* Initialize HDR and 10-bit color settings.
	 * Skip on mirrored outputs - the legacy DRM ioctls (drmModeConnectorSetProperty)
	 * can conflict with wlroots' atomic modesetting and cause GPU hangs. */
	if (!m->is_mirror)
		init_monitor_color_settings(m);

	updatemons(NULL, NULL);

	/* Restart swaybg so it discovers the new output and shows wallpaper
	 * on all screens. swaybg sometimes fails to create a surface for
	 * outputs that appear after it has already started. */
	if (is_external_connector(wlr_output->name))
		restart_wallpaper();

	/* Auto-show monitor setup popup if multiple monitors and no config file */
	{
		int mon_count = 0;
		Monitor *om;
		wl_list_for_each(om, &mons, link) {
			if (om->wlr_output->enabled && !om->is_mirror)
				mon_count++;
		}
		if (mon_count > 1 && !monitors_conf_exists())
			schedule_monitor_setup_popup();
	}
}

struct wlr_box
fullscreen_mirror_geom(Monitor *m)
{
	Monitor *laptop, *other;

	/* If this monitor is the laptop and has an external mirror,
	 * use the external's geometry so fullscreen video fills the TV. */
	if (!m->is_mirror && (strncmp(m->wlr_output->name, "eDP", 3) == 0 ||
	    strncmp(m->wlr_output->name, "LVDS", 4) == 0)) {
		wl_list_for_each(other, &mons, link) {
			if (other->is_mirror && other->wlr_output->enabled)
				return other->m;
		}
	}

	/* If this IS the mirror, find the laptop and return the larger geometry */
	if (m->is_mirror) {
		laptop = find_laptop_monitor();
		if (laptop) {
			/* Return whichever is larger */
			if (m->m.width * m->m.height > laptop->m.width * laptop->m.height)
				return m->m;
		}
	}

	return m->m;
}

void
togglemirror(const Arg *arg)
{
	Monitor *laptop, *ext, *m;
	Client *c;

	laptop = find_laptop_monitor();
	if (!laptop)
		return;

	/* Find the external monitor */
	ext = NULL;
	wl_list_for_each(m, &mons, link) {
		if (m != laptop && m->wlr_output->enabled &&
		    is_external_connector(m->wlr_output->name)) {
			ext = m;
			break;
		}
	}
	if (!ext)
		return;

	if (ext->is_mirror) {
		/* Switch from mirror to independent */
		ext->is_mirror = 0;
		wlr_output_layout_remove(output_layout, ext->wlr_output);
		wlr_output_layout_add_auto(output_layout, ext->wlr_output);

		/* Restore clients that belong to this monitor */
		wl_list_for_each(c, &clients, link) {
			if (c->output && strcmp(ext->wlr_output->name, c->output) == 0
					&& c->mon != ext)
				setmon(c, ext, c->tags);
		}

		if (!selmon || selmon == laptop)
			selmon = ext;

		wlr_log(WLR_INFO, "Mirror off: %s is now independent",
			ext->wlr_output->name);
	} else {
		/* Switch from independent to mirror */
		struct wlr_box laptop_box;
		wlr_output_layout_get_box(output_layout, laptop->wlr_output, &laptop_box);

		/* Move clients from external to laptop */
		wl_list_for_each(c, &clients, link) {
			if (c->mon == ext)
				setmon(c, laptop, c->tags);
		}

		ext->is_mirror = 1;
		wlr_output_layout_remove(output_layout, ext->wlr_output);
		wlr_output_layout_add(output_layout, ext->wlr_output,
			laptop_box.x, laptop_box.y);
		ext->m.x = laptop_box.x;
		ext->m.y = laptop_box.y;

		if (selmon == ext)
			selmon = laptop;

		wlr_log(WLR_INFO, "Mirror on: %s now mirrors %s at (%d,%d)",
			ext->wlr_output->name, laptop->wlr_output->name,
			laptop_box.x, laptop_box.y);
	}

	updatemons(NULL, NULL);
	arrange(laptop);
	if (!ext->is_mirror)
		arrange(ext);
	focusclient(focustop(selmon), 1);
	printstatus();
	restart_wallpaper();
}

Monitor *
dirtomon(enum wlr_direction dir)
{
	struct wlr_output *next;
	if (!wlr_output_layout_get(output_layout, selmon->wlr_output))
		return selmon;
	if ((next = wlr_output_layout_adjacent_output(output_layout,
			dir, selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	if ((next = wlr_output_layout_farthest_output(output_layout,
			dir ^ (WLR_DIRECTION_LEFT|WLR_DIRECTION_RIGHT),
			selmon->wlr_output, selmon->m.x, selmon->m.y)))
		return next->data;
	return selmon;
}

void
gpureset(struct wl_listener *listener, void *data)
{
	struct wlr_renderer *old_drw = drw;
	struct wlr_allocator *old_alloc = alloc;
	struct Monitor *m;
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("couldn't recreate renderer");

	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't recreate allocator");

	wl_list_remove(&gpu_reset.link);
	wl_signal_add(&drw->events.lost, &gpu_reset);

	wlr_compositor_set_renderer(compositor, drw);

	wl_list_for_each(m, &mons, link) {
		wlr_output_init_render(m->wlr_output, alloc, drw);
	}

	wlr_allocator_destroy(old_alloc);
	wlr_renderer_destroy(old_drw);
}

void
outputmgrapply(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 0);
}

void
outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test)
{
	/*
	 * Called when a client such as wlr-randr requests a change in output
	 * configuration. This is only one way that the layout can be changed,
	 * so any Monitor information should be updated by updatemons() after an
	 * output_layout.change event, not here.
	 */
	struct wlr_output_configuration_head_v1 *config_head;
	int ok = 1;

	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		Monitor *m = wlr_output->data;
		struct wlr_output_state state;

		/* Ensure displays previously disabled by wlr-output-power-management-v1
		 * are properly handled*/
		m->asleep = 0;

		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, config_head->state.enabled);
		if (!config_head->state.enabled)
			goto apply_or_test;

		if (config_head->state.mode)
			wlr_output_state_set_mode(&state, config_head->state.mode);
		else
			wlr_output_state_set_custom_mode(&state,
					config_head->state.custom_mode.width,
					config_head->state.custom_mode.height,
					config_head->state.custom_mode.refresh);

		wlr_output_state_set_transform(&state, config_head->state.transform);
		wlr_output_state_set_scale(&state, config_head->state.scale);
		wlr_output_state_set_adaptive_sync_enabled(&state,
				config_head->state.adaptive_sync_enabled);

apply_or_test:
		ok &= test ? wlr_output_test_state(wlr_output, &state)
				: wlr_output_commit_state(wlr_output, &state);

		/* Don't move monitors if position wouldn't change. This avoids
		 * wlroots marking the output as manually configured.
		 * wlr_output_layout_add does not like disabled outputs */
		if (!test && wlr_output->enabled && (m->m.x != config_head->state.x || m->m.y != config_head->state.y))
			wlr_output_layout_add(output_layout, wlr_output,
					config_head->state.x, config_head->state.y);

		wlr_output_state_finish(&state);
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);

	/* https://codeberg.org/dwl/dwl/issues/577 */
	updatemons(NULL, NULL);
}

void
outputmgrtest(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 1);
}

void
powermgrsetmode(struct wl_listener *listener, void *data)
{
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct wlr_output_state state = {0};
	Monitor *m = event->output->data;

	if (!m)
		return;

	m->gamma_lut_changed = 1; /* Reapply gamma LUT when re-enabling the ouput */
	wlr_output_state_set_enabled(&state, event->mode);
	wlr_output_commit_state(m->wlr_output, &state);

	m->asleep = !event->mode;
	updatemons(NULL, NULL);
}

/* ── Idle monitor render throttle ─────────────────────────────────── */

/* Schedule a frame on this monitor, skipping redundant calls within a
 * single render cycle. Cleared at the top of rendermon(). */
static inline void
request_frame(Monitor *m)
{
	if (!m || !m->wlr_output || m->frame_scheduled)
		return;
	m->frame_scheduled = 1;
	wlr_output_schedule_frame(m->wlr_output);
}

static int
idle_heartbeat_cb(void *data)
{
	Monitor *m = data;
	if (m->render_idle)
		request_frame(m);
	return 0; /* one-shot; re-armed from rendermon */
}

static void
monitor_wake_internal(Monitor *m)
{
	m->render_idle = 0;
	m->idle_frames = 0;
	wl_event_source_timer_update(m->idle_heartbeat, 0); /* disarm */
	request_frame(m);
}

void
monitor_wake(Monitor *m)
{
	if (m && m->render_idle)
		monitor_wake_internal(m);
}

static void
present_videoplayer_frame(Monitor *m, uint64_t frame_start_ns)
{
	if (active_videoplayer &&
	    active_videoplayer->mon == m &&
	    (active_videoplayer->state == VP_STATE_PLAYING ||
	     active_videoplayer->state == VP_STATE_BUFFERING)) {
		videoplayer_present_frame(active_videoplayer, frame_start_ns);
		request_frame(m);
	}
}

static void
classify_fullscreen_content(Monitor *m, int *out_game, int *out_video, int *out_tearing)
{
	Client *c = focustop(m);

	/* Return cached result if the top client hasn't changed.
	 * Guard against NULL == NULL: after invalidate_video_pacing()
	 * clears the cache to NULL, a tag with no fullscreen client
	 * would match and return stale flags from the old tag. */
	if (c && c == m->classify_cache_client) {
		*out_game = m->classify_cache_game;
		*out_video = m->classify_cache_video;
		*out_tearing = m->classify_cache_tearing;
		return;
	}

	*out_game = 0;
	*out_video = 0;
	*out_tearing = 0;

	if (c && c->isfullscreen) {
		*out_game = client_wants_tearing(c) || is_game_content(c);
		*out_video = is_video_content(c) || c->detected_video_hz > 0.0f;
		if (*out_game && !*out_video)
			*out_tearing = 1;
	}

	m->classify_cache_client = c;
	m->classify_cache_game = *out_game;
	m->classify_cache_video = *out_video;
	m->classify_cache_tearing = *out_tearing;
}

static void
track_game_frame_pacing(Monitor *m, uint64_t frame_start_ns)
{
	/* Track game's frame submission interval */
	if (m->game_frame_submit_ns > 0 && frame_start_ns > m->game_frame_submit_ns) {
		uint64_t game_interval = frame_start_ns - m->game_frame_submit_ns;

		/* Only track reasonable intervals (8ms - 100ms = ~10-120fps) */
		if (game_interval > 8000000 && game_interval < 100000000) {
			m->game_frame_intervals[m->game_frame_interval_idx] = game_interval;
			m->game_frame_interval_idx = (m->game_frame_interval_idx + 1) % 16;
			if (m->game_frame_interval_count < 16)
				m->game_frame_interval_count++;

			/* Calculate estimated game FPS from rolling average */
			if (m->game_frame_interval_count >= 4) {
				uint64_t avg_interval = 0;
				uint64_t variance_sum = 0;
				for (int i = 0; i < m->game_frame_interval_count; i++)
					avg_interval += m->game_frame_intervals[i];
				avg_interval /= m->game_frame_interval_count;
				m->estimated_game_fps = 1000000000.0f / (float)avg_interval;

				for (int i = 0; i < m->game_frame_interval_count; i++) {
					int64_t diff = (int64_t)m->game_frame_intervals[i] - (int64_t)avg_interval;
					variance_sum += (uint64_t)(diff * diff);
				}
				m->frame_variance_ns = variance_sum / m->game_frame_interval_count;

				/* Predictive frame timing */
				m->predicted_next_frame_ns = frame_start_ns + avg_interval;

				/* Add margin based on variance (sqrt = stddev) */
				if (m->frame_variance_ns > 0) {
					uint64_t margin = (uint64_t)ceil(sqrt((double)m->frame_variance_ns));
					m->predicted_next_frame_ns += margin;
				}

				/* Track prediction accuracy */
				if (m->predicted_next_frame_ns > 0) {
					int64_t prediction_error = (int64_t)frame_start_ns - (int64_t)(m->predicted_next_frame_ns - avg_interval);
					if (prediction_error < 0) prediction_error = -prediction_error;

					if (prediction_error < 2000000) {
						m->prediction_accuracy = m->prediction_accuracy * 0.9f + 10.0f;
					} else if (prediction_error < (int64_t)avg_interval / 4) {
						m->prediction_accuracy = m->prediction_accuracy * 0.9f + 5.0f;
						if (frame_start_ns < m->predicted_next_frame_ns - avg_interval)
							m->frames_early++;
						else
							m->frames_late++;
					} else {
						m->prediction_accuracy = m->prediction_accuracy * 0.9f;
						m->frames_late++;
					}
					if (m->prediction_accuracy > 100.0f) m->prediction_accuracy = 100.0f;
				}

				/* Update dynamic game VRR */
				if (m->game_vrr_active)
					update_game_vrr(m, m->estimated_game_fps);
			}
		}
	}
	m->game_frame_submit_ns = frame_start_ns;
	m->pending_game_frame = 1;

	/* Frame hold/release decision */
	if (m->present_interval_ns > 0 && m->target_present_ns > 0) {
		uint64_t time_to_target = 0;
		if (frame_start_ns < m->target_present_ns)
			time_to_target = m->target_present_ns - frame_start_ns;

		if (time_to_target > 2000000)
			m->frames_held++;
	}
}

/*
 * Frame repeat: adapt game frame cadence to display refresh rate.
 *
 * For a game at X fps on a display at Y Hz, find integer N such that
 * Y/N is as close to X as possible.  Show each game frame for exactly
 * N vblanks, giving perfectly even frame times.
 *
 * Examples:
 *   45 fps on 144 Hz → N=3 (48 fps effective, game throttled smoothly)
 *   30 fps on 120 Hz → N=4 (30 fps effective, exact match)
 *   40 fps on 144 Hz → N=4 (36 fps effective, game throttled)
 *   55 fps on 144 Hz → N=3 (48 fps effective)
 *
 * The repeat count always picks the N whose effective FPS is closest
 * to and not dramatically above the game's natural FPS (within 35%).
 * If no N gives effective FPS within 35%, repeat is disabled.
 *
 * Hysteresis prevents rapid toggling: a new repeat count must be the
 * best candidate for 30 consecutive vblanks (~200ms at 144Hz) before
 * it's applied.  This makes the system stable with fluctuating FPS.
 */
#define FRAME_REPEAT_HYSTERESIS 30  /* vblanks before switching */
static void
calculate_frame_repeat(Monitor *m, int is_game, int allow_tearing)
{
	float display_hz = 0.0f;
	float game_fps = m->estimated_game_fps;
	float ratio;
	int best_n = 1;
	float best_error = 9999.0f;

	if (m->present_interval_ns > 0)
		display_hz = 1000000000.0f / (float)m->present_interval_ns;
	else if (m->wlr_output->current_mode)
		display_hz = (float)m->wlr_output->current_mode->refresh / 1000.0f;

	if (game_fps < 5.0f || display_hz < 30.0f)
		goto apply;

	ratio = display_hz / game_fps;

	/* If game FPS is close to display Hz, no repeat needed */
	if (ratio < 1.5f) {
		best_n = 1;
		goto apply;
	}

	/*
	 * Find the integer N whose effective FPS (display_hz / N) is
	 * closest to the game's natural FPS.  Prefer N where the
	 * effective FPS is at or above the game FPS (so the game can
	 * keep up without dropping frames), but allow slightly below.
	 */
	for (int n = 2; n <= 10; n++) {
		float effective_fps = display_hz / (float)n;
		float error = fabsf(effective_fps - game_fps) / game_fps;

		/* Effective FPS must be within 35% of game FPS */
		if (error > 0.35f)
			continue;

		/* Penalize N where effective FPS is BELOW game FPS
		 * (game produces frames faster than display shows them → drops).
		 * When effective >= game, the game paces itself naturally. */
		if (effective_fps < game_fps)
			error *= 1.1f;

		if (error < best_error) {
			best_error = error;
			best_n = n;
		}
	}

apply:
	/*
	 * Hysteresis: only apply a new repeat count after it has been
	 * the best candidate for FRAME_REPEAT_HYSTERESIS consecutive
	 * vblanks.  This prevents rapid toggling between repeat counts
	 * when the game FPS fluctuates near a boundary.
	 */
	if (best_n == m->frame_repeat_count) {
		/* Already active — reset candidate tracking */
		m->frame_repeat_candidate = best_n;
		m->frame_repeat_candidate_age = 0;
	} else if (best_n == m->frame_repeat_candidate) {
		/* Same candidate as last vblank — age it */
		m->frame_repeat_candidate_age++;
		/* Adaptive hysteresis: fast transitions for large FPS changes,
		 * slow transitions for small fluctuations near boundaries */
		int hysteresis_threshold = FRAME_REPEAT_HYSTERESIS;
		if (m->frame_repeat_count > 0 && best_n != m->frame_repeat_count) {
			float old_effective = display_hz / (float)m->frame_repeat_count;
			float new_effective = (best_n > 1) ? display_hz / (float)best_n : game_fps;
			float change_pct = fabsf(new_effective - old_effective) / old_effective;
			if (change_pct > 0.3f)
				hysteresis_threshold = 8;
		}
		if (m->frame_repeat_candidate_age >= hysteresis_threshold) {
			/* Stable long enough — apply the change.
			 * Let the current repeat cycle finish before switching
			 * to avoid a timing glitch mid-cycle. */
			if (best_n > 1 && !m->frame_repeat_enabled) {
				m->frame_repeat_enabled = 1;
				m->frame_repeat_interval_ns = m->present_interval_ns;
			} else if (best_n == 1 && m->frame_repeat_enabled) {
				m->frame_repeat_enabled = 0;
			}
			wlr_log(WLR_DEBUG, "Frame repeat %dx → %dx (%.1f FPS @ %.0f Hz)",
				m->frame_repeat_count, best_n, game_fps, display_hz);
			m->frame_repeat_count = best_n;
			m->frame_repeat_current = 0;
			m->frame_repeat_candidate_age = 0;
		}
	} else {
		/* New candidate — start tracking */
		m->frame_repeat_candidate = best_n;
		m->frame_repeat_candidate_age = 1;
	}

	/* Calculate judder score (0-100, lower is better) */
	if (game_fps > 0.0f && display_hz > 0.0f) {
		int active_n = m->frame_repeat_count;
		float ideal_ms = 1000.0f / game_fps;
		float actual_ms = 1000.0f / display_hz * (float)active_n;
		float dev = fabsf(ideal_ms - actual_ms) / ideal_ms * 100.0f;
		m->judder_score = (int)(dev * 10.0f);
		if (m->judder_score > 100) m->judder_score = 100;
	}

	m->adaptive_pacing_enabled = 1;
	m->target_frame_time_ms = 1000.0f / game_fps;
}

static void
commit_output_frame(Monitor *m, struct wlr_output_state *state, int allow_tearing,
	int use_frame_pacing, uint64_t frame_start_ns)
{
	uint64_t commit_end_ns;
	/* Overlay plane layer states — kept on the stack for the entire
	 * commit lifetime because wlr_output_state_set_layers stores a
	 * pointer into state->layers and the array must stay valid across
	 * the possible commit retry below. */
	struct wlr_output_layer_state overlay_layer_states[1];
	size_t overlay_layer_count = 0;

	m->frames_since_content_change = 0;

	/* Toast overlay-plane promotion.
	 *
	 * If the toast is flagged active on an overlay plane, build a
	 * layer state for it and attach to the commit. This runs on every
	 * frame the toast is alive; the buffer contents are static, so we
	 * just keep handing the backend the same buffer pointer. */
	if (m->toast_overlay_active && m->toast_overlay_layer &&
	    m->toast_overlay_buf) {
		overlay_layer_states[0] = (struct wlr_output_layer_state){
			.layer = m->toast_overlay_layer,
			.buffer = &m->toast_overlay_buf->base,
			.src_box = {
				.x = 0, .y = 0,
				.width = m->toast_overlay_dst.width,
				.height = m->toast_overlay_dst.height,
			},
			.dst_box = m->toast_overlay_dst,
		};
		overlay_layer_count = 1;
	} else if (m->toast_overlay_layer && !m->toast_overlay_active) {
		/* Toast was active but has been hidden — send a disable
		 * layer state (buffer = NULL) so the plane is torn down. */
		overlay_layer_states[0] = (struct wlr_output_layer_state){
			.layer = m->toast_overlay_layer,
			.buffer = NULL,
		};
		overlay_layer_count = 1;
	}
	if (overlay_layer_count > 0) {
		wlr_output_state_set_layers(state, overlay_layer_states,
			overlay_layer_count);
	}

	if (allow_tearing && wlr_output_is_drm(m->wlr_output))
		state->tearing_page_flip = true;

	/* Apply deferred VRR state change.  Piggybacking on the buffer
	 * commit keeps it non-blocking (the DRM backend sets NONBLOCK when
	 * WLR_OUTPUT_STATE_BUFFER is present). */
	if (m->vrr_pending == 1) {
		wlr_output_state_set_adaptive_sync_enabled(state, true);
	} else if (m->vrr_pending == -1) {
		wlr_output_state_set_adaptive_sync_enabled(state, false);
	}

	if (!wlr_output_commit_state(m->wlr_output, state)) {
		int committed = 0;

		/* If VRR was piggybacked and commit failed, the driver may
		 * have rejected the adaptive_sync property.  Strip VRR and
		 * retry buffer-only so the frame isn't lost. */
		if (m->vrr_pending && (state->committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED)) {
			state->committed &= ~WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
			wlr_log(WLR_DEBUG, "VRR commit rejected on %s, retrying buffer-only",
				m->wlr_output->name);
			m->vrr_pending = 0;
			committed = wlr_output_commit_state(m->wlr_output, state);
		}

		if (!committed && allow_tearing) {
			state->tearing_page_flip = false;
			committed = wlr_output_commit_state(m->wlr_output, state);
			if (!committed)
				wlr_log(WLR_ERROR, "Output commit failed on %s (non-tearing retry)",
					m->wlr_output->name);
		}

		if (!committed) {
			/* Force full damage — the failed commit's buffer was
			 * never displayed, so the swapchain's next buffer has
			 * stale content from 2+ frames ago. */
			wlr_damage_ring_add_whole(&m->scene_output->damage_ring);

			m->commit_failures++;

			if (m->commit_failures <= 3) {
				wlr_log(WLR_ERROR, "Output commit failed on %s (n=%u)",
					m->wlr_output->name, m->commit_failures);
				request_frame(m);
			} else if (m->commit_failures == 4) {
				/* Kernel is refusing this buffer format/modifier
				 * persistently (e.g. 10-bit Y-tiled CCS on i915
				 * without Resizable BAR). Blacklist direct
				 * scanout so the next frame goes through GPU
				 * composition, force XRGB8888 render format to
				 * get a fresh swapchain, and drop compositor
				 * RT-priority so user-space can still preempt. */
				wlr_log(WLR_ERROR,
					"%s: %u consecutive commit failures — "
					"disabling direct scanout, falling back "
					"to GPU composition + XRGB8888",
					m->wlr_output->name, m->commit_failures);
				m->scanout_blacklist = 1;

				struct wlr_output_state fb;
				wlr_output_state_init(&fb);
				wlr_output_state_set_render_format(&fb, DRM_FORMAT_XRGB8888);
				if (m->wlr_output->current_mode)
					wlr_output_state_set_mode(&fb, m->wlr_output->current_mode);
				if (wlr_output_test_state(m->wlr_output, &fb))
					wlr_output_commit_state(m->wlr_output, &fb);
				wlr_output_state_finish(&fb);

				if (compositor_rt_applied) {
					struct sched_param sp = { .sched_priority = 0 };
					sched_setscheduler(0, SCHED_OTHER, &sp);
					compositor_rt_applied = 0;
					wlr_log(WLR_ERROR,
						"RT scheduling dropped after %u commit failures",
						m->commit_failures);
				}

				struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
				nanosleep(&ts, NULL);
				m->last_commit_fail_ns = get_time_ns();
				request_frame(m);
			} else {
				/* Still failing after fallback — rate-limit
				 * frame scheduling to ~60 Hz and log once per
				 * second so we don't melt the event loop. */
				uint64_t now = get_time_ns();
				if (m->commit_failures % 60 == 0) {
					wlr_log(WLR_ERROR,
						"%s: %u commit failures (still retrying)",
						m->wlr_output->name, m->commit_failures);
				}
				if (now - m->last_commit_fail_ns > 16000000ULL) {
					m->last_commit_fail_ns = now;
					request_frame(m);
				}
			}
		} else {
			m->commit_failures = 0;
			m->scanout_blacklist = 0;
		}
	} else {
		m->commit_failures = 0;
		m->scanout_blacklist = 0;
	}

	/* Finalize deferred VRR state after commit.  Check actual hardware
	 * status to confirm the change took effect. */
	if (m->vrr_pending == 1) {
		if (m->wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED) {
			struct wlr_output_configuration_v1 *vrr_config;
			struct wlr_output_configuration_head_v1 *vrr_head;

			m->vrr_active = 1;
			m->vrr_target_hz = m->vrr_pending_hz;
			m->vrr_pending = 0;

			vrr_config = wlr_output_configuration_v1_create();
			vrr_head = wlr_output_configuration_head_v1_create(vrr_config, m->wlr_output);
			vrr_head->state.adaptive_sync_enabled = 1;
			wlr_output_manager_v1_set_configuration(output_mgr, vrr_config);

			wlr_log(WLR_DEBUG, "VRR enabled for %.3f Hz video on %s",
					m->vrr_pending_hz, m->wlr_output->name);
		} else {
			wlr_log(WLR_ERROR, "VRR enable failed on %s (hw rejected)",
					m->wlr_output->name);
			m->vrr_pending = 0;
		}
	} else if (m->vrr_pending == -1) {
		if (m->wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_DISABLED) {
			struct wlr_output_configuration_v1 *vrr_config;
			struct wlr_output_configuration_head_v1 *vrr_head;

			m->vrr_active = 0;
			m->vrr_target_hz = 0.0f;
			m->vrr_pending = 0;

			vrr_config = wlr_output_configuration_v1_create();
			vrr_head = wlr_output_configuration_head_v1_create(vrr_config, m->wlr_output);
			vrr_head->state.adaptive_sync_enabled = 0;
			wlr_output_manager_v1_set_configuration(output_mgr, vrr_config);

			wlr_log(WLR_DEBUG, "VRR disabled on %s", m->wlr_output->name);
		} else {
			wlr_log(WLR_ERROR, "VRR disable failed on %s (hw rejected)",
					m->wlr_output->name);
			m->vrr_pending = 0;
		}
	}

	commit_end_ns = get_time_ns();
	m->last_commit_duration_ns = commit_end_ns - frame_start_ns;

	/* Input latency tracking */
	if (game_mode_ultra && m->last_input_ns > 0) {
		uint64_t input_latency = commit_end_ns - m->last_input_ns;
		if (input_latency < 500000000ULL) {
			m->input_to_frame_ns = input_latency;
			if (m->min_input_latency_ns == 0 || input_latency < m->min_input_latency_ns)
				m->min_input_latency_ns = input_latency;
			if (input_latency > m->max_input_latency_ns)
				m->max_input_latency_ns = input_latency;
		}
	}

	/* Rolling average of commit time */
	if (m->rolling_commit_time_ns == 0) {
		m->rolling_commit_time_ns = m->last_commit_duration_ns;
	} else {
		m->rolling_commit_time_ns =
			(m->rolling_commit_time_ns * 98 + m->last_commit_duration_ns * 2) / 100;
		if (m->last_commit_duration_ns > m->rolling_commit_time_ns)
			m->rolling_commit_time_ns = m->last_commit_duration_ns;
	}
}

void
rendermon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, frame);
	struct wlr_scene_output_state_options opts = {0};
	struct wlr_output_state state;
	struct timespec now;
	uint64_t frame_start_ns;
	int needs_frame = 0;
	int allow_tearing = 0;
	int is_video = 0;
	int is_game = 0;
	int is_direct_scanout = 0;
	int use_frame_pacing = 0;
	int state_built = 0;

	m->frame_scheduled = 0;

	frame_start_ns = get_time_ns();
	now.tv_sec = frame_start_ns / 1000000000ULL;
	now.tv_nsec = frame_start_ns % 1000000000ULL;

	present_videoplayer_frame(m, frame_start_ns);

	classify_fullscreen_content(m, &is_game, &is_video, &allow_tearing);

	/*
	 * Video frame pacing for external video players (VLC, mpv, etc).
	 *
	 * When a video player is fullscreen, skip rendering on vblanks
	 * where the player hasn't submitted a new frame.  This avoids
	 * unnecessary GPU work and lets the player's own PTS-based clock
	 * drive the frame cadence naturally.
	 *
	 * For 24fps @ 60Hz this produces natural 3:2 pulldown.
	 * For 24fps @ 144Hz the player submits every 6th vblank.
	 */
	if (is_video && !is_game && m->video_cadence_active && !m->vrr_active) {
		/*
		 * Content-driven pacing for external video players.
		 *
		 * Instead of a Bresenham cadence clock (which runs out of phase
		 * with the player's frame submissions and drops every other
		 * frame), we present whenever the player has submitted a new
		 * frame.  The player's own PTS-based timing creates the natural
		 * cadence (e.g., 3:2 pulldown for 24fps@60Hz).
		 *
		 * On hold vblanks (no new content): skip rendering entirely.
		 * The display keeps showing the old frame.  Send frame_done
		 * so the player can continue its decode pipeline.
		 *
		 * On present vblanks (new content): render normally, commit
		 * the new frame to the display.
		 */
		wlr_output_state_init(&state);
		if (m->tag_switch_debug > 0)
			write(STDERR_FILENO, "TS:cadence-build>\n", 18);
		needs_frame = wlr_scene_output_build_state(m->scene_output, &state, &opts);
		if (m->tag_switch_debug > 0)
			write(STDERR_FILENO, "TS:cadence-build<\n", 18);
		if (!needs_frame) {
			/* No new video frame - hold */
			wlr_output_state_finish(&state);
			request_frame(m);
			wlr_scene_output_send_frame_done(m->scene_output, &now);
			if (m->tag_switch_debug > 0)
				m->tag_switch_debug--;
			return;
		}
		state_built = 1;
		/* New frame available - fall through to commit */
	} else if (!is_video && m->video_cadence_active) {
		/* Video exited fullscreen - deactivate cadence.
		 * Clear all cadence state so rendermon doesn't enter the
		 * cadence hold path on subsequent vblanks. */
		m->video_cadence_active = 0;
		m->video_cadence_counter = 0;
		m->video_cadence_accum = 0.0f;
		if (!is_game) {
			m->frame_pacing_active = 0;
			m->estimated_game_fps = 0.0f;
		}
		/* Force full damage so the desktop renders immediately
		 * instead of potentially getting an empty build_state */
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);
		wlr_log(WLR_DEBUG, "Video cadence deactivated");
		/* VRR disable is deferred via vrr_pending — it will
		 * piggyback on the next frame commit (non-blocking). */
	}

	/*
	 * Build output state. This determines if we actually need to render.
	 * wlr_scene handles damage tracking and direct scanout optimization.
	 *
	 * When a fullscreen window covers the entire output and its buffer
	 * is compatible (DMA-BUF, correct size/format), wlroots will
	 * automatically use direct scanout - bypassing GPU composition entirely.
	 * This gives optimal frame pacing for games and video.
	 */
	if (!state_built) {
		wlr_output_state_init(&state);
		if (m->tag_switch_debug > 0)
			write(STDERR_FILENO, "TS:scene-build>\n", 16);
		needs_frame = wlr_scene_output_build_state(m->scene_output, &state, &opts);
		if (m->tag_switch_debug > 0)
			write(STDERR_FILENO, "TS:scene-build<\n", 16);

		/* If build_state failed and 10-bit is active, the backend may not
		 * support the 10-bit render format at composition time (e.g. NVIDIA).
		 * Fall back to 8-bit and retry. */
		if (!needs_frame && m->render_10bit_active) {
			struct wlr_output_state fb;
			wlr_output_state_finish(&state);
			wlr_log(WLR_INFO, "Scene build failed with 10-bit on %s, falling back to 8-bit",
				m->wlr_output->name);
			wlr_output_state_init(&fb);
			wlr_output_state_set_render_format(&fb, DRM_FORMAT_XRGB8888);
			if (m->wlr_output->current_mode)
				wlr_output_state_set_mode(&fb, m->wlr_output->current_mode);
			if (wlr_output_test_state(m->wlr_output, &fb) &&
			    wlr_output_commit_state(m->wlr_output, &fb)) {
				m->render_10bit_active = 0;
				wlr_log(WLR_INFO, "Switched to 8-bit rendering on %s", m->wlr_output->name);
			}
			wlr_output_state_finish(&fb);
			/* Retry scene build with new format */
			wlr_output_state_init(&state);
			needs_frame = wlr_scene_output_build_state(m->scene_output, &state, &opts);
		}

		/*
		 * Scene build failed (swapchain or render pass error).
		 * This happens on NVIDIA-only systems where buffer allocation
		 * may fail.  Force XRGB8888 render
		 * format (destroys the current swapchain so a fresh one is
		 * negotiated) and retry.  Schedule a frame so we keep retrying
		 * instead of stalling with a black screen.
		 */
		if (!needs_frame) {
			wlr_output_state_finish(&state);

			m->scene_build_failures++;
			if (m->scene_build_failures <= 3) {
				wlr_log(WLR_ERROR,
					"Scene build failed on %s (attempt %d), "
					"forcing XRGB8888 and retrying",
					m->wlr_output->name, m->scene_build_failures);

				/* Force a fresh swapchain with XRGB8888 */
				struct wlr_output_state fb;
				wlr_output_state_init(&fb);
				wlr_output_state_set_render_format(&fb, DRM_FORMAT_XRGB8888);
				if (m->wlr_output->current_mode)
					wlr_output_state_set_mode(&fb, m->wlr_output->current_mode);
				if (m->tag_switch_debug > 0)
					write(STDERR_FILENO, "TS:recovery-modeset>\n", 21);
				if (wlr_output_test_state(m->wlr_output, &fb))
					wlr_output_commit_state(m->wlr_output, &fb);
				if (m->tag_switch_debug > 0)
					write(STDERR_FILENO, "TS:recovery-modeset<\n", 21);
				wlr_output_state_finish(&fb);
			} else if (m->scene_build_failures == 4) {
				wlr_log(WLR_ERROR,
					"Scene build persistently failing on %s — "
					"check GPU driver and Vulkan/GPU libraries",
					m->wlr_output->name);
			}

			request_frame(m);
			if (m->tag_switch_debug > 0)
				m->tag_switch_debug--;
			return;
		}

		/* Scene build succeeded — reset failure counter */
		m->scene_build_failures = 0;
	}

	/* Screenshot frame capture — grab pixels before commit */
	if (screenshot_mode == SCREENSHOT_PENDING && m == selmon && needs_frame && state.buffer) {
		screenshot_capture_frame(m, state.buffer);
	}

	/*
	 * Detect direct scanout: if the output buffer is a client buffer
	 * (not from the compositor's swapchain), direct scanout is active.
	 * wlr_client_buffer_get() returns non-NULL only for client buffers.
	 */
	if (needs_frame && state.buffer && wlr_client_buffer_get(state.buffer) &&
	    !m->scanout_blacklist) {
		is_direct_scanout = 1;
	}

	/* Track scanout state transitions.
	 * NOTE: Do NOT show OSD here — the OSD scene node would immediately
	 * break scanout on the next frame, creating a ping-pong loop where
	 * scanout activates, OSD appears, scanout deactivates (3 sec), OSD
	 * hides, scanout re-activates, repeat forever. Log only. */
	if (is_direct_scanout && !m->direct_scanout_active) {
		m->direct_scanout_active = 1;
		m->frame_pacing_active = 1; /* Enable frame pacing for direct scanout */
		wlr_log(WLR_DEBUG, "Direct scanout activated on %s - frame pacing enabled",
			m->wlr_output->name);
	} else if (!is_direct_scanout && m->direct_scanout_active) {
		m->direct_scanout_active = 0;
		wlr_log(WLR_INFO, "Direct scanout deactivated on %s - stats: %lu presented, %lu dropped, %lu held",
			m->wlr_output->name, m->frames_presented, m->frames_dropped, m->frames_held);
		/* Keep frame_pacing_active if game or video cadence is active */
		if (!is_game && !m->video_cadence_active) {
			m->frame_pacing_active = 0;
			m->pending_game_frame = 0;
			m->estimated_game_fps = 0.0f;
		}
	}

	/* Scanout failure diagnostic.
	 *
	 * When a fullscreen game is visible but direct scanout isn't
	 * activating, enumerate the most common blockers so the user/dev
	 * can correlate the failure with visible UI elements. Only fires
	 * when we're confident the game wants scanout (fullscreen, not
	 * tearing — tearing goes through a different path).
	 *
	 * Rate-limited to once per 5s per monitor. */
	if (is_game && !is_direct_scanout && !allow_tearing && needs_frame) {
		if (frame_start_ns - m->scanout_diag_warn_ns > 5ULL * 1000000000ULL) {
			int blocker_count = 0;
			char blockers[256] = {0};
			size_t off = 0;

			#define ADD_BLOCKER(name) do { \
				blocker_count++; \
				off += snprintf(blockers + off, sizeof(blockers) - off, \
					"%s%s", off > 0 ? ", " : "", name); \
			} while (0)

			if (m->statusbar.tree && m->statusbar.tree->node.enabled)
				ADD_BLOCKER("statusbar");
			if (m->toast_tree && m->toast_visible)
				ADD_BLOCKER("toast");
			if (m->hz_osd_tree && m->hz_osd_visible)
				ADD_BLOCKER("hz_osd");
			if (m->render_10bit_active)
				ADD_BLOCKER("10bit_render");

			#undef ADD_BLOCKER

			if (blocker_count > 0) {
				wlr_log(WLR_INFO,
					"Direct scanout blocked on %s by: %s "
					"(fullscreen game detected — expect GPU composition cost)",
					m->wlr_output->name, blockers);
			} else {
				wlr_log(WLR_INFO,
					"Direct scanout blocked on %s — no nixlytile-owned blockers. "
					"Possible cause: buffer format/size mismatch, color transform, "
					"or hardware rejection. Check client buffer parameters.",
					m->wlr_output->name);
			}
			m->scanout_diag_warn_ns = frame_start_ns;
		}
	}

	/* Frame pacing for fullscreen games.
	 * Enable even without direct scanout — many Proton/Wine games
	 * don't get direct scanout due to format or size mismatch, but
	 * still benefit from even frame cadence. */
	if (is_game && !allow_tearing) {
		if (!m->frame_pacing_active) {
			m->frame_pacing_active = 1;
			wlr_log(WLR_DEBUG, "Frame pacing enabled for fullscreen game on %s",
				m->wlr_output->name);
		}
		use_frame_pacing = 1;
		if (needs_frame)
			track_game_frame_pacing(m, frame_start_ns);
	}

	/* Frame doubling/tripling for smooth low-FPS playback (non-VRR).
	 * For video: use detected_video_hz as the fps source.
	 * For games: use estimated_game_fps from real-time tracking. */
	if (is_video && !m->vrr_active && m->frame_pacing_active) {
		Client *vc = focustop(m);
		if (vc && vc->detected_video_hz > 0.0f)
			m->estimated_game_fps = vc->detected_video_hz;
		/* Video players manage their own frame timing based on PTS.
		 * Don't use frame_repeat (delays frame_done) for video -
		 * the Bresenham cadence handles pacing when active,
		 * otherwise let the player run freely. */
	} else if (is_game && m->frame_pacing_active && !m->game_vrr_active && !allow_tearing) {
		/* Skip recalculation if FPS hasn't changed by more than 2% */
		float fps_delta = m->estimated_game_fps - m->frame_repeat_last_fps;
		if (fps_delta < 0) fps_delta = -fps_delta;
		if (m->frame_repeat_last_fps <= 0.0f ||
		    fps_delta > m->frame_repeat_last_fps * 0.02f) {
			m->frame_repeat_last_fps = m->estimated_game_fps;
			calculate_frame_repeat(m, is_game, allow_tearing);
		}
	} else if (m->frame_repeat_enabled) {
		m->frame_repeat_enabled = 0;
		m->frame_repeat_count = 1;
		m->frame_repeat_current = 0;
		m->frame_repeat_candidate = 0;
		m->frame_repeat_candidate_age = 0;
		m->adaptive_pacing_enabled = 0;
		wlr_log(WLR_DEBUG, "Frame repeat disabled - VRR active or tearing enabled");
	}

	if (is_game)
		wlr_output_state_set_content_type(&state, WP_CONTENT_TYPE_V1_TYPE_GAME);
	else if (is_video)
		wlr_output_state_set_content_type(&state, WP_CONTENT_TYPE_V1_TYPE_VIDEO);

	if (needs_frame) {
		if (m->tag_switch_debug > 0)
			write(STDERR_FILENO, "TS:commit>\n", 11);
		commit_output_frame(m, &state, allow_tearing, use_frame_pacing, frame_start_ns);
		if (m->tag_switch_debug > 0)
			write(STDERR_FILENO, "TS:commit<\n", 11);
	} else {
		m->frames_since_content_change++;
		if (use_frame_pacing && m->pending_game_frame) {
			m->frames_dropped++;
			m->pending_game_frame = 0;
		}
	}

	wlr_output_state_finish(&state);
	m->last_frame_ns = frame_start_ns;

	if (m->tag_switch_debug > 0)
		m->tag_switch_debug--;

	/*
	 * FPS Limiter - controls when we send frame_done to clients.
	 *
	 * By delaying frame_done, we effectively limit how fast games can
	 * render. The game waits for frame_done before starting the next
	 * frame, so controlling this signal controls the framerate.
	 */
	if (fps_limit_enabled && fps_limit_value > 0 && is_game) {
		uint64_t target_interval_ns = 1000000000ULL / (uint64_t)fps_limit_value;
		uint64_t now_ns = frame_start_ns;
		uint64_t elapsed_ns = 0;

		if (m->fps_limit_last_frame_ns > 0)
			elapsed_ns = now_ns - m->fps_limit_last_frame_ns;

		m->fps_limit_interval_ns = target_interval_ns;

		/*
		 * If not enough time has passed since last frame_done,
		 * skip sending frame_done this vblank. The game will
		 * continue to wait, effectively limiting its framerate.
		 */
		if (elapsed_ns < target_interval_ns) {
			/* Don't send frame_done yet - limiter is active.
			 * Schedule next vblank so rendermon keeps firing. */
			request_frame(m);
			return;
		}

		m->fps_limit_last_frame_ns = now_ns;
	}

	/*
	 * ============ FRAME REPEAT MECHANISM ============
	 *
	 * When frame doubling/tripling is active, we control the frame_done
	 * signal to make the game render at a rate that divides evenly into
	 * the display refresh rate.
	 *
	 * For example, on a 120Hz display with a game targeting 30 FPS:
	 * - We want frame_done sent every 4th vblank (120/4=30)
	 * - Each game frame is displayed for exactly 4 vblanks
	 * - Result: perfectly even 33.33ms frame times, zero judder
	 *
	 * This is the KEY to smooth low-FPS gaming without VRR:
	 * Instead of uneven frame times (16-17-16-17-16ms), we get
	 * perfectly consistent frame times (33-33-33-33-33ms for 30fps).
	 */
	if (m->frame_repeat_enabled && m->frame_repeat_count > 1 && is_game) {
		m->frame_repeat_current++;

		/*
		 * Only send frame_done when we've shown the current frame
		 * for the required number of vblanks.
		 */
		if (m->frame_repeat_current < m->frame_repeat_count) {
			/*
			 * Not time for a new frame yet - skip frame_done.
			 * The client will wait, and the display shows the same
			 * frame again on the next vblank (frame "doubling" etc).
			 *
			 * CRITICAL: schedule the next vblank so rendermon keeps
			 * firing.  Without this, the counter gets stuck because
			 * no pageflip or damage occurs to trigger the next call,
			 * and frame_done is never sent — the client falls back to
			 * its own timer, producing uncontrolled frame cadence.
			 */
			m->frames_repeated++;
			request_frame(m);
			return;
		}

		/* Time for new frame - reset counter and continue to frame_done */
		m->frame_repeat_current = 0;
	}

	/* --- Idle monitor throttle --- */
	{
		Monitor *cursor_mon = xytomon(cursor->x, cursor->y);
		int cursor_here = (cursor_mon == m);
		int video_here = (active_videoplayer && active_videoplayer->mon == m &&
		                  (active_videoplayer->state == VP_STATE_PLAYING ||
		                   active_videoplayer->state == VP_STATE_BUFFERING));

		if (cursor_here || is_game || is_video || video_here) {
			if (m->render_idle)
				monitor_wake_internal(m);
			m->idle_frames = 0;
		} else {
			m->idle_frames++;
			if (m->idle_frames > 120) {
				if (!m->render_idle) {
					m->render_idle = 1;
					wl_event_source_timer_update(m->idle_heartbeat, 1000);
				}
				/* Send frame_done for this heartbeat cycle but don't
				 * schedule another frame — heartbeat fires in ~1s */
				wlr_scene_output_send_frame_done(m->scene_output, &now);
				return;
			}
		}
	}

	/*
	 * Send frame_done to clients.
	 *
	 * This is critical for proper frame pacing:
	 * - Video players use this to time their next frame submission
	 * - Games use this to know when to start rendering the next frame
	 * - The timestamp should reflect when the frame opportunity occurred
	 *
	 * We always send this, even if we didn't commit, because clients
	 * need to know a vblank occurred for proper timing.
	 */
	wlr_scene_output_send_frame_done(m->scene_output, &now);
}

void
outputpresent(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, present);
	struct wlr_output_event_present *event = data;
	uint64_t present_ns;

	if (!event || !event->presented)
		return;

	/* Convert timespec to nanoseconds */
	present_ns = (uint64_t)event->when.tv_sec * 1000000000ULL +
		     (uint64_t)event->when.tv_nsec;

	/* Calculate interval between presents (vblank interval).
	 * Skip when video cadence is active - presents happen every 2-3
	 * vblanks during cadence, which would contaminate the measurement
	 * and give wrong display_hz for future calculations. */
	if (m->last_present_ns > 0 && present_ns > m->last_present_ns &&
	    !m->video_cadence_active) {
		uint64_t interval = present_ns - m->last_present_ns;
		/* Sanity check: interval should be reasonable (1ms - 100ms) */
		if (interval > 1000000 && interval < 100000000) {
			/* Rolling average with 90% old, 10% new */
			if (m->present_interval_ns == 0) {
				m->present_interval_ns = interval;
			} else {
				m->present_interval_ns =
					(m->present_interval_ns * 90 + interval * 10) / 100;
			}
		}
	}

	m->last_present_ns = present_ns;
	m->frames_presented++;

	/*
	 * If we're in frame pacing mode and have a pending game frame,
	 * calculate latency statistics.
	 */
	if (m->frame_pacing_active && m->pending_game_frame && m->game_frame_submit_ns > 0) {
		uint64_t latency = present_ns - m->game_frame_submit_ns;
		m->total_latency_ns += latency;
		m->pending_game_frame = 0;
	}

	/*
	 * Calculate target time for next frame presentation.
	 * We want to present at the next vblank, minus an adaptive margin
	 * derived from the rolling (max-biased) commit-time EMA so slow
	 * commits don't overshoot vblank, while fast commits don't waste
	 * headroom.
	 */
	if (m->present_interval_ns > 0) {
		uint64_t margin = m->rolling_commit_time_ns + 500000ULL; /* +0.5ms headroom */
		if (margin < 1000000ULL)        /* floor: 1.0ms — matches old behaviour */
			margin = 1000000ULL;
		if (margin > 2500000ULL)        /* ceiling: 2.5ms */
			margin = 2500000ULL;
		if (margin >= m->present_interval_ns) /* defensive: never exceed interval */
			margin = m->present_interval_ns / 2;
		m->target_present_ns = present_ns + m->present_interval_ns - margin;
	}
}

void
requestmonstate(struct wl_listener *listener, void *data)
{
	struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(event->output, event->state);
	updatemons(NULL, NULL);
}

/*
 * Commit an adaptive-sync state change on the given output.
 *
 * Performs the full verification chain:
 *   1. test_state() — backend reports whether the state would commit
 *   2. commit_state() — backend applies the state to the kernel
 *   3. post-commit check — wlr_output->adaptive_sync_status reflects reality
 *
 * Returns 1 iff all three steps succeeded AND the hardware now reports the
 * requested state. Failure at any stage is logged with the specific reason.
 *
 * This is the foundation for all VRR enable/disable operations. Callers
 * should update their internal state (m->vrr_active etc.) ONLY when this
 * returns 1.
 */
static int
commit_adaptive_sync(Monitor *m, int enable)
{
	struct wlr_output_state state;
	enum wlr_output_adaptive_sync_status expected;
	int ok;

	if (!m || !m->wlr_output || !m->wlr_output->enabled) {
		wlr_log(WLR_ERROR, "VRR: commit refused — invalid monitor state");
		return 0;
	}

	wlr_output_state_init(&state);
	wlr_output_state_set_adaptive_sync_enabled(&state, enable ? true : false);

	/* Phase 1: test before commit to catch capability mismatch early. */
	if (!wlr_output_test_state(m->wlr_output, &state)) {
		wlr_log(WLR_ERROR,
			"VRR: test_state rejected adaptive_sync=%d on %s "
			"(hardware or mode incompatible)",
			enable, m->wlr_output->name);
		wlr_output_state_finish(&state);
		return 0;
	}

	/* Phase 2: commit. Can still fail on race (display hot-unplug,
	 * kernel rejection after test passed, etc.) */
	ok = wlr_output_commit_state(m->wlr_output, &state);
	wlr_output_state_finish(&state);
	if (!ok) {
		wlr_log(WLR_ERROR,
			"VRR: commit_state failed adaptive_sync=%d on %s "
			"(test passed but kernel rejected — possible race)",
			enable, m->wlr_output->name);
		return 0;
	}

	/* Phase 3: post-commit verification. wlroots updates
	 * adaptive_sync_status after successful commit, so this confirms
	 * the kernel-level property change actually took effect.
	 * Guards against silent "commit succeeded but hardware didn't
	 * apply the property" failure mode. */
	expected = enable
		? WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED
		: WLR_OUTPUT_ADAPTIVE_SYNC_DISABLED;
	if (m->wlr_output->adaptive_sync_status != expected) {
		wlr_log(WLR_ERROR,
			"VRR: commit succeeded on %s but adaptive_sync_status=%d "
			"(expected %d) — backend silently dropped the property",
			m->wlr_output->name,
			(int)m->wlr_output->adaptive_sync_status,
			(int)expected);
		return 0;
	}

	wlr_log(WLR_INFO, "VRR: adaptive_sync=%d verified active on %s",
		enable, m->wlr_output->name);
	return 1;
}

void
set_adaptive_sync(Monitor *m, int enable)
{
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;

	if (!m || !m->wlr_output || !m->wlr_output->enabled
			|| !fullscreen_adaptive_sync_enabled
			|| !m->vrr_capable)
		return;

	/* Use the verified commit helper. If it fails, we do NOT broadcast
	 * the state change — output_mgr would otherwise advertise VRR as
	 * active to clients while the hardware silently ignored the request. */
	if (!commit_adaptive_sync(m, enable))
		return;

	/* Broadcast the adaptive sync state change to output_mgr only after
	 * verified success. */
	config = wlr_output_configuration_v1_create();
	config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
	config_head->state.adaptive_sync_enabled = enable;
	wlr_output_manager_v1_set_configuration(output_mgr, config);
}

void
enable_game_vrr(Monitor *m)
{
	if (!m || !m->vrr_capable || m->game_vrr_active)
		return;

	if (!fullscreen_adaptive_sync_enabled) {
		wlr_log(WLR_DEBUG, "Game VRR: disabled by user setting");
		return;
	}

	if (commit_adaptive_sync(m, 1)) {
		m->game_vrr_active = 1;
		m->game_vrr_target_fps = 0.0f;
		m->game_vrr_last_fps = 0.0f;
		m->game_vrr_last_change_ns = get_time_ns();
		m->game_vrr_stable_frames = 0;

		show_hz_osd(m, "Game VRR Enabled");
		wlr_log(WLR_DEBUG, "Game VRR enabled on %s", m->wlr_output->name);
	}
}

void
disable_game_vrr(Monitor *m)
{
	if (!m || !m->game_vrr_active)
		return;

	if (commit_adaptive_sync(m, 0)) {
		wlr_log(WLR_DEBUG, "Game VRR disabled on %s (was targeting %.1f FPS)",
			m->wlr_output->name, m->game_vrr_target_fps);
	}

	m->game_vrr_active = 0;
	m->game_vrr_target_fps = 0.0f;
	m->game_vrr_last_fps = 0.0f;
	m->game_vrr_stable_frames = 0;
}

void
update_game_vrr(Monitor *m, float current_fps)
{
	uint64_t now_ns;
	float fps_diff;
	float display_max_hz;
	char osd_msg[64];

	if (!m || !m->game_vrr_active)
		return;

	now_ns = get_time_ns();

	/* LFC (Low Framerate Compensation) diagnostic.
	 *
	 * Most VRR displays have a minimum refresh rate of 48 Hz (some 40,
	 * some 30). Below that the display needs LFC: the driver doubles
	 * or triples frames to stay within the VRR range. AMD and Intel
	 * implement LFC in their kernel/mesa stacks — gcaps.has_hw_lfc is
	 * set for those, and we skip the warning entirely there. NVIDIA's
	 * proprietary driver does NOT implement userspace-visible LFC on
	 * Wayland (as of 555+), so the warning is meaningful only on
	 * NVIDIA and unknown-vendor paths.
	 *
	 * Rate-limited to once per 10s per monitor. */
	if (!m->gcaps.has_hw_lfc && current_fps > 0.0f && current_fps < 40.0f) {
		if (now_ns - m->game_vrr_lfc_warn_ns > 10ULL * 1000000000ULL) {
			wlr_log(WLR_INFO,
				"VRR/LFC: game FPS %.1f on %s is below typical VRR "
				"minimum (~48 Hz) and vendor has no hardware LFC. "
				"Display may flicker or fall out of VRR range.",
				current_fps, m->wlr_output->name);
			m->game_vrr_lfc_warn_ns = now_ns;
		}
	}

	/* Sanity check FPS range */
	if (current_fps < GAME_VRR_MIN_FPS || current_fps > GAME_VRR_MAX_FPS)
		return;

	/* Get display's maximum refresh rate */
	if (m->wlr_output->current_mode) {
		display_max_hz = (float)m->wlr_output->current_mode->refresh / 1000.0f;
	} else {
		display_max_hz = 60.0f;
	}

	/* If game is running at or above display refresh, no adjustment needed */
	if (current_fps >= display_max_hz - 2.0f) {
		if (m->game_vrr_target_fps > 0.0f && m->game_vrr_target_fps < display_max_hz - 2.0f) {
			/* Was running slower, now at full speed */
			m->game_vrr_target_fps = display_max_hz;
			m->game_vrr_stable_frames = 0;
			snprintf(osd_msg, sizeof(osd_msg), "VRR: %.0f Hz (full)", display_max_hz);
			show_hz_osd(m, osd_msg);
			wlr_log(WLR_DEBUG, "Game VRR: back to full refresh %.1f Hz", display_max_hz);
		}
		return;
	}

	/* Calculate difference from current target */
	fps_diff = fabsf(current_fps - m->game_vrr_last_fps);

	/* Check if FPS is stable (within deadband of last reading) */
	if (fps_diff < GAME_VRR_FPS_DEADBAND) {
		m->game_vrr_stable_frames++;
	} else {
		/* FPS changed significantly, reset stability counter */
		m->game_vrr_stable_frames = 0;
		m->game_vrr_last_fps = current_fps;
	}

	/* Only adjust if FPS has been stable for enough frames */
	if (m->game_vrr_stable_frames < GAME_VRR_STABLE_FRAMES)
		return;

	/* Rate limit: don't adjust more than once per interval */
	if (now_ns - m->game_vrr_last_change_ns < GAME_VRR_MIN_INTERVAL_NS)
		return;

	/* Check if we actually need to change the target */
	fps_diff = fabsf(current_fps - m->game_vrr_target_fps);
	if (fps_diff < GAME_VRR_FPS_DEADBAND)
		return;

	/* Update target FPS */
	m->game_vrr_target_fps = current_fps;
	m->game_vrr_last_change_ns = now_ns;
	m->game_vrr_stable_frames = 0;

	/*
	 * With VRR/FreeSync/G-Sync, the display automatically syncs to
	 * the incoming frame rate. We don't need to do anything special
	 * here - just having adaptive sync enabled is enough.
	 *
	 * The key insight is that VRR displays wait for each frame,
	 * so if the game is running at 45 FPS, the display will show
	 * frames at 45 Hz without judder.
	 *
	 * We track the target FPS for:
	 * 1. OSD display feedback to the user
	 * 2. Statistics and debugging
	 * 3. Detecting when to disable VRR (e.g., game exits)
	 */

	snprintf(osd_msg, sizeof(osd_msg), "VRR: %.0f Hz", current_fps);
	show_hz_osd(m, osd_msg);
	wlr_log(WLR_DEBUG, "Game VRR: adjusted to %.1f FPS on %s",
		current_fps, m->wlr_output->name);
}

int
is_video_content(Client *c)
{
	struct wlr_surface *surface;
	enum wp_content_type_v1_type content_type;

	if (!c || !content_type_mgr)
		return 0;

	surface = client_surface(c);
	if (!surface)
		return 0;

	content_type = wlr_surface_get_content_type_v1(content_type_mgr, surface);

	/* Debug: log content type for fullscreen clients */
	static int last_logged_type = -1;
	if (c->isfullscreen && (int)content_type != last_logged_type) {
		const char *type_str = "unknown";
		switch (content_type) {
		case WP_CONTENT_TYPE_V1_TYPE_NONE: type_str = "none"; break;
		case WP_CONTENT_TYPE_V1_TYPE_PHOTO: type_str = "photo"; break;
		case WP_CONTENT_TYPE_V1_TYPE_VIDEO: type_str = "video"; break;
		case WP_CONTENT_TYPE_V1_TYPE_GAME: type_str = "game"; break;
		}
		wlr_log(WLR_DEBUG, "Fullscreen client content-type: %s", type_str);
		last_logged_type = (int)content_type;
	}

	return content_type == WP_CONTENT_TYPE_V1_TYPE_VIDEO;
}

int
is_game_content(Client *c)
{
	struct wlr_surface *surface;
	enum wp_content_type_v1_type content_type;

	if (!c || !content_type_mgr)
		return 0;

	surface = client_surface(c);
	if (!surface)
		return 0;

	content_type = wlr_surface_get_content_type_v1(content_type_mgr, surface);
	return content_type == WP_CONTENT_TYPE_V1_TYPE_GAME;
}

int
client_wants_tearing(Client *c)
{
	struct wlr_surface *surface;
	enum wp_tearing_control_v1_presentation_hint hint;

	if (!c || !tearing_control_mgr)
		return 0;

	surface = client_surface(c);
	if (!surface)
		return 0;

	hint = wlr_tearing_control_manager_v1_surface_hint_from_surface(
			tearing_control_mgr, surface);

	return hint == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
}

void
set_video_refresh_rate(Monitor *m, Client *c)
{
	if (!m || !m->wlr_output || !m->wlr_output->enabled || !c)
		return;

	/* Don't re-detect if already active */
	if (m->video_mode_active || m->vrr_active || c->detected_video_hz > 0.0f)
		return;

	/* Check if content type is video */
	if (!is_video_content(c))
		return;

	/* Reset frame tracking to detect actual video Hz */
	c->frame_time_idx = 0;
	c->frame_time_count = 0;
	c->detected_video_hz = 0.0f;

	wlr_log(WLR_DEBUG, "Video content detected on %s, starting frame rate detection",
			m->wlr_output->name);

	/* Schedule video check to detect fps and set appropriate mode */
	schedule_video_check(500);
}

void
restore_max_refresh_rate(Monitor *m)
{
	struct wlr_output_mode *max_mode;
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;

	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return;

	/* Disable VRR if active */
	if (m->vrr_active)
		disable_vrr_video_mode(m);

	/* Only restore mode if we're in video mode */
	if (!m->video_mode_active)
		return;

	/* Find best (highest refresh) mode */
	max_mode = bestmode(m->wlr_output);
	if (!max_mode)
		return;

	wlr_log(WLR_DEBUG, "Restoring %s to max mode: %dx%d@%dmHz",
			m->wlr_output->name, max_mode->width, max_mode->height,
			max_mode->refresh);

	/* Apply the mode */
	wlr_output_state_init(&state);
	wlr_output_state_set_mode(&state, max_mode);
	if (wlr_output_commit_state(m->wlr_output, &state)) {
		m->video_mode_active = 0;
		m->original_mode = NULL;

		/* Broadcast change to output manager */
		config = wlr_output_configuration_v1_create();
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
		config_head->state.mode = max_mode;
		wlr_output_manager_v1_set_configuration(output_mgr, config);

		/* Show OSD notification */
		{
			char osd_msg[64];
			snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz",
					max_mode->refresh / 1000, max_mode->refresh % 1000);
			show_hz_osd(m, osd_msg);
		}
	}
	wlr_output_state_finish(&state);
}

int
detect_10bit_support(Monitor *m)
{
	const struct wlr_drm_format_set *formats;
	const struct wlr_drm_format *fmt;
	int has_10bit = 0;

	if (!m || !m->wlr_output)
		return 0;

	/* Check primary plane formats for 10-bit support */
	formats = wlr_output_get_primary_formats(m->wlr_output, WLR_BUFFER_CAP_DMABUF);
	if (formats) {
		/* Look for XRGB2101010 or XBGR2101010 (10-bit RGB formats) */
		fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_XRGB2101010);
		if (fmt) {
			has_10bit = 1;
			wlr_log(WLR_INFO, "Monitor %s supports DRM_FORMAT_XRGB2101010 (10-bit)",
				m->wlr_output->name);
		}

		fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_XBGR2101010);
		if (fmt) {
			has_10bit = 1;
			wlr_log(WLR_INFO, "Monitor %s supports DRM_FORMAT_XBGR2101010 (10-bit)",
				m->wlr_output->name);
		}

		/* Also check for ARGB2101010 (10-bit with alpha) */
		fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB2101010);
		if (fmt) {
			has_10bit = 1;
			wlr_log(WLR_INFO, "Monitor %s supports DRM_FORMAT_ARGB2101010 (10-bit+alpha)",
				m->wlr_output->name);
		}
	}

	return has_10bit;
}

/* ── Low-latency cursor plane ─────────────────────────────────────── */

static uint32_t
ll_find_plane_prop(int fd, uint32_t plane_id, const char *name)
{
	drmModeObjectPropertiesPtr props;
	uint32_t prop_id = 0;
	unsigned int i;

	props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
	if (!props)
		return 0;

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
		if (!p)
			continue;
		if (strcmp(p->name, name) == 0)
			prop_id = p->prop_id;
		drmModeFreeProperty(p);
		if (prop_id)
			break;
	}
	drmModeFreeObjectProperties(props);
	return prop_id;
}

void
ll_cursor_init(Monitor *m)
{
	int drm_fd;
	uint32_t conn_id, crtc_id = 0;
	drmModeConnectorPtr conn;
	drmModeEncoderPtr enc;
	drmModePlaneResPtr planes;
	unsigned int i;

	m->ll_cursor_fd = -1;
	m->ll_cursor_plane_id = 0;
	m->ll_cursor_crtc_id = 0;
	m->ll_cursor_prop_x = 0;
	m->ll_cursor_prop_y = 0;
	m->ll_cursor_active = 0;

	if (!m->wlr_output || !wlr_output_is_drm(m->wlr_output))
		return;

	drm_fd = wlr_backend_get_drm_fd(m->wlr_output->backend);
	if (drm_fd < 0)
		return;

	conn_id = wlr_drm_connector_get_id(m->wlr_output);
	if (conn_id == 0)
		return;

	/* Connector → Encoder → CRTC */
	conn = drmModeGetConnector(drm_fd, conn_id);
	if (!conn)
		return;
	if (conn->encoder_id == 0) {
		drmModeFreeConnector(conn);
		return;
	}
	enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
	drmModeFreeConnector(conn);
	if (!enc)
		return;
	crtc_id = enc->crtc_id;
	drmModeFreeEncoder(enc);
	if (crtc_id == 0)
		return;

	/* Get CRTC index from resources to check possible_crtcs bitmask */
	int crtc_index = -1;
	drmModeResPtr res = drmModeGetResources(drm_fd);
	if (res) {
		for (int k = 0; k < res->count_crtcs; k++) {
			if (res->crtcs[k] == crtc_id) {
				crtc_index = k;
				break;
			}
		}
		drmModeFreeResources(res);
	}
	if (crtc_index < 0)
		return;

	/* Find cursor plane for this CRTC */
	planes = drmModeGetPlaneResources(drm_fd);
	if (!planes)
		return;

	for (i = 0; i < planes->count_planes; i++) {
		drmModePlanePtr plane = drmModeGetPlane(drm_fd, planes->planes[i]);
		if (!plane)
			continue;

		/* Skip planes that can't drive our CRTC */
		if (!(plane->possible_crtcs & (1u << crtc_index))) {
			drmModeFreePlane(plane);
			continue;
		}

		/* Check plane type property */
		uint32_t type_prop = ll_find_plane_prop(drm_fd, plane->plane_id, "type");
		if (type_prop) {
			drmModeObjectPropertiesPtr pprops;
			pprops = drmModeObjectGetProperties(drm_fd, plane->plane_id,
				DRM_MODE_OBJECT_PLANE);
			if (pprops) {
				unsigned int j;
				for (j = 0; j < pprops->count_props; j++) {
					if (pprops->props[j] == type_prop &&
					    pprops->prop_values[j] == DRM_PLANE_TYPE_CURSOR) {
						/* Verify this plane is attached to our CRTC */
						if (plane->crtc_id == crtc_id ||
						    plane->crtc_id == 0) {
							m->ll_cursor_plane_id = plane->plane_id;
						}
					}
				}
				drmModeFreeObjectProperties(pprops);
			}
		}
		drmModeFreePlane(plane);
		if (m->ll_cursor_plane_id)
			break;
	}
	drmModeFreePlaneResources(planes);

	if (!m->ll_cursor_plane_id) {
		wlr_log(WLR_DEBUG, "No cursor plane found for %s", m->wlr_output->name);
		return;
	}

	/* Look up CRTC_X and CRTC_Y property IDs on the cursor plane */
	m->ll_cursor_prop_x = ll_find_plane_prop(drm_fd, m->ll_cursor_plane_id, "CRTC_X");
	m->ll_cursor_prop_y = ll_find_plane_prop(drm_fd, m->ll_cursor_plane_id, "CRTC_Y");

	if (!m->ll_cursor_prop_x || !m->ll_cursor_prop_y) {
		wlr_log(WLR_DEBUG, "Cursor plane %u missing CRTC_X/Y properties",
			m->ll_cursor_plane_id);
		m->ll_cursor_plane_id = 0;
		return;
	}

	m->ll_cursor_fd = drm_fd;
	m->ll_cursor_crtc_id = crtc_id;
	m->ll_cursor_active = 1;

	wlr_log(WLR_INFO, "Low-latency cursor enabled for %s (plane %u, crtc %u)",
		m->wlr_output->name, m->ll_cursor_plane_id, m->ll_cursor_crtc_id);
}

void
ll_cursor_move(Monitor *m, int x, int y)
{
	drmModeAtomicReqPtr req;

	if (!m->ll_cursor_active || m->ll_cursor_fd < 0)
		return;

	/* Only update if HW cursor is actually engaged */
	if (!m->wlr_output->hardware_cursor)
		return;

	req = drmModeAtomicAlloc();
	if (!req)
		return;

	drmModeAtomicAddProperty(req, m->ll_cursor_plane_id,
		m->ll_cursor_prop_x, (uint64_t)(int64_t)x);
	drmModeAtomicAddProperty(req, m->ll_cursor_plane_id,
		m->ll_cursor_prop_y, (uint64_t)(int64_t)y);

	/* NONBLOCK, no PAGE_FLIP_EVENT = immediate cursor update, VRR-safe */
	drmModeAtomicCommit(m->ll_cursor_fd, req,
		DRM_MODE_ATOMIC_NONBLOCK, NULL);

	drmModeAtomicFree(req);
}

void
ll_cursor_cleanup(Monitor *m)
{
	m->ll_cursor_fd = -1;
	m->ll_cursor_plane_id = 0;
	m->ll_cursor_crtc_id = 0;
	m->ll_cursor_prop_x = 0;
	m->ll_cursor_prop_y = 0;
	m->ll_cursor_active = 0;
}

int
set_drm_color_properties(Monitor *m, int max_bpc)
{
	int drm_fd;
	uint32_t conn_id;
	drmModeConnector *conn = NULL;
	drmModePropertyRes *prop = NULL;
	int i;
	int success = 0;

	if (!m || !m->wlr_output || !wlr_output_is_drm(m->wlr_output))
		return 0;

	/* Get DRM fd from renderer */
	drm_fd = wlr_renderer_get_drm_fd(drw);
	if (drm_fd < 0) {
		wlr_log(WLR_DEBUG, "Cannot get DRM fd for color properties");
		return 0;
	}

	/* Get connector ID from wlroots */
	conn_id = wlr_drm_connector_get_id(m->wlr_output);
	if (conn_id == 0) {
		wlr_log(WLR_DEBUG, "Cannot get DRM connector ID for %s", m->wlr_output->name);
		return 0;
	}

	/* Get connector to access properties */
	conn = drmModeGetConnector(drm_fd, conn_id);
	if (!conn) {
		wlr_log(WLR_DEBUG, "Cannot get DRM connector %u", conn_id);
		return 0;
	}

	/* Find and set max bpc property */
	for (i = 0; i < conn->count_props; i++) {
		prop = drmModeGetProperty(drm_fd, conn->props[i]);
		if (!prop)
			continue;

		if (strcmp(prop->name, "max bpc") == 0) {
			/* Check if requested bpc is within range */
			if (prop->flags & DRM_MODE_PROP_RANGE) {
				uint64_t min_bpc = prop->values[0];
				uint64_t max_supported_bpc = prop->values[1];

				if (max_bpc < (int)min_bpc)
					max_bpc = (int)min_bpc;
				if (max_bpc > (int)max_supported_bpc)
					max_bpc = (int)max_supported_bpc;

				if (drmModeConnectorSetProperty(drm_fd, conn_id,
						conn->props[i], max_bpc) == 0) {
					wlr_log(WLR_INFO, "Set max bpc to %d for %s (supports %lu-%lu)",
						max_bpc, m->wlr_output->name, min_bpc, max_supported_bpc);
					m->max_bpc = max_bpc;
					success = 1;
				} else {
					wlr_log(WLR_ERROR, "Failed to set max bpc to %d for %s",
						max_bpc, m->wlr_output->name);
				}
			}
			drmModeFreeProperty(prop);
			break;
		}
		drmModeFreeProperty(prop);
	}

	/* Also look for Colorspace or output_color_format property for RGB full */
	for (i = 0; i < conn->count_props; i++) {
		prop = drmModeGetProperty(drm_fd, conn->props[i]);
		if (!prop)
			continue;

		/* AMD uses "Colorspace", Intel uses "output_colorspace" */
		if (strcmp(prop->name, "Colorspace") == 0 ||
		    strcmp(prop->name, "output_colorspace") == 0) {
			/* Try to find RGB_Wide_Fixed or Default (RGB full range) */
			if (prop->flags & DRM_MODE_PROP_ENUM) {
				int j;
				uint64_t rgb_value = 0; /* Default is usually 0 */

				for (j = 0; j < prop->count_enums; j++) {
					/* Prefer Default or RGB_Wide_Fixed for best color */
					if (strcmp(prop->enums[j].name, "Default") == 0 ||
					    strcmp(prop->enums[j].name, "RGB_Wide_Fixed") == 0) {
						rgb_value = prop->enums[j].value;
						break;
					}
				}

				if (drmModeConnectorSetProperty(drm_fd, conn_id,
						conn->props[i], rgb_value) == 0) {
					wlr_log(WLR_INFO, "Set colorspace to RGB for %s",
						m->wlr_output->name);
				}
			}
			drmModeFreeProperty(prop);
			break;
		}
		drmModeFreeProperty(prop);
	}

	drmModeFreeConnector(conn);
	return success;
}

int
enable_10bit_rendering(Monitor *m)
{
	struct wlr_output_state state;
	int success = 0;

	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return 0;

	if (!m->supports_10bit) {
		wlr_log(WLR_DEBUG, "Monitor %s does not support 10-bit", m->wlr_output->name);
		return 0;
	}

	if (m->render_10bit_active) {
		wlr_log(WLR_DEBUG, "10-bit already active on %s", m->wlr_output->name);
		return 1;
	}

	wlr_output_state_init(&state);
	wlr_output_state_set_render_format(&state, DRM_FORMAT_XRGB2101010);

	/* Preserve current mode so we don't reset to preferred/default mode */
	if (m->wlr_output->current_mode)
		wlr_output_state_set_mode(&state, m->wlr_output->current_mode);

	if (wlr_output_test_state(m->wlr_output, &state)) {
		if (wlr_output_commit_state(m->wlr_output, &state)) {
			m->render_10bit_active = 1;
			success = 1;
			wlr_log(WLR_INFO, "Enabled 10-bit rendering on %s", m->wlr_output->name);

			/* wlroots 0.20 sets max_bpc automatically via atomic commits
			 * based on the render format (pick_max_bpc in atomic.c) */

			/* Show notification */
			toast_show(m, "10-bit color", 1500);
		} else {
			wlr_log(WLR_ERROR, "Failed to commit 10-bit state on %s, falling back to 8-bit",
				m->wlr_output->name);
		}
	} else {
		wlr_log(WLR_INFO, "10-bit render format not supported by backend on %s, using 8-bit",
			m->wlr_output->name);
	}

	wlr_output_state_finish(&state);

	/* If 10-bit failed, explicitly ensure 8-bit format is active */
	if (!success) {
		struct wlr_output_state fallback;
		wlr_output_state_init(&fallback);
		wlr_output_state_set_render_format(&fallback, DRM_FORMAT_XRGB8888);
		if (m->wlr_output->current_mode)
			wlr_output_state_set_mode(&fallback, m->wlr_output->current_mode);
		if (wlr_output_test_state(m->wlr_output, &fallback))
			wlr_output_commit_state(m->wlr_output, &fallback);
		wlr_output_state_finish(&fallback);
		m->render_10bit_active = 0;
	}

	return success;
}

void
init_monitor_color_settings(Monitor *m)
{
	if (!m || !m->wlr_output)
		return;

	/* Detect capabilities */
	m->supports_10bit = detect_10bit_support(m);
	m->render_10bit_active = 0;
	m->max_bpc = 8;  /* Default */
	m->hdr_capable = 0;  /* Will be set when wlroots gets HDR support */
	m->hdr_active = 0;

	wlr_log(WLR_INFO, "Monitor %s: 10-bit=%s, HDR=%s",
		m->wlr_output->name,
		m->supports_10bit ? "yes" : "no",
		m->hdr_capable ? "yes" : "no");

	/*
	 * Automatically enable 10-bit rendering if supported.
	 * This provides smoother gradients and better color accuracy.
	 *
	 * Skip on secondary GPU outputs (multi-GPU / mgpu): the render format
	 * must survive cross-GPU format negotiation (mgpu_formats intersection).
	 * XRGB2101010 modifiers rarely intersect between Intel and Nvidia,
	 * causing swapchain creation to fail → permanent black screen.
	 *
	 * The old set_drm_color_properties() legacy DRM calls that used to
	 * follow this have been removed: wlroots 0.20 manages "max bpc"
	 * automatically via pick_max_bpc() in atomic commits.  The legacy
	 * calls corrupted wlroots' internal atomic state and caused EPERM
	 * on subsequent mode changes.
	 */
	if (m->supports_10bit) {
		int skip_10bit = 0;
		if (wlr_output_is_drm(m->wlr_output)) {
			struct wlr_backend *parent =
				wlr_drm_backend_get_parent(m->wlr_output->backend);
			if (parent) {
				skip_10bit = 1;
				wlr_log(WLR_INFO, "Monitor %s is on secondary GPU, "
					"skipping 10-bit to avoid cross-GPU format issues",
					m->wlr_output->name);
			}
		}
		/* Nvidia EGL/GBM may not properly handle XRGB2101010 render
		 * targets, causing swapchain format failures and black screen.
		 * Skip auto-enable on Nvidia-primary systems; users can still
		 * enable 10-bit manually if their setup supports it. */
		if (!skip_10bit && discrete_gpu_idx >= 0 &&
		    detected_gpus[discrete_gpu_idx].vendor == GPU_VENDOR_NVIDIA &&
		    integrated_gpu_idx < 0) {
			skip_10bit = 1;
			wlr_log(WLR_INFO, "Monitor %s: skipping auto 10-bit on "
				"Nvidia-primary GPU to avoid format issues",
				m->wlr_output->name);
		}
		if (!skip_10bit)
			enable_10bit_rendering(m);
	}

	/*
	 * wlroots 0.20 manages "max bpc" and "Colorspace" DRM connector
	 * properties automatically via atomic commits (see atomic.c
	 * pick_max_bpc / colorspace handling). Setting them behind its back
	 * with legacy drmModeConnectorSetProperty corrupts the internal
	 * state and causes subsequent mode changes to fail.
	 */
}

void
track_client_frame(Client *c)
{
	struct wlr_surface *surface;
	struct wlr_buffer *current_buffer;
	uint64_t now;
	static int log_count = 0;

	if (!c)
		return;

	surface = client_surface(c);
	if (!surface)
		return;

	/*
	 * Only track when the buffer actually changes. On high refresh rate
	 * monitors, compositors may re-commit the same buffer multiple times
	 * per video frame. By tracking buffer changes, we measure actual
	 * content updates rather than compositor refresh rate.
	 *
	 * We use the wlr_client_buffer pointer as an opaque identifier - when
	 * a new buffer is attached, the pointer changes.
	 */
	current_buffer = (struct wlr_buffer *)surface->buffer;
	if (current_buffer == c->last_buffer)
		return;

	c->last_buffer = current_buffer;
	now = monotonic_msec();

	/* Log interval for debugging */
	if (c->frame_time_count > 0 && log_count < 20) {
		int prev_idx = (c->frame_time_idx - 1 + 32) % 32;
		uint64_t interval = now - c->frame_times[prev_idx];
		wlr_log(WLR_INFO, "Frame buffer change: interval=%lums (%.1f fps)",
				(unsigned long)interval, 1000.0 / interval);
		log_count++;
	}

	c->frame_times[c->frame_time_idx] = now;
	c->frame_time_idx = (c->frame_time_idx + 1) % 32;
	if (c->frame_time_count < 32)
		c->frame_time_count++;
}

float
detect_video_framerate(Client *c)
{
	int i, count, valid_intervals;
	uint64_t intervals[31];
	double avg_interval;
	uint64_t sum = 0;
	uint64_t variance_sum = 0;
	double variance, stddev;
	double exact_hz;

	if (!c || c->frame_time_count < 12)
		return 0.0f;

	count = c->frame_time_count;
	valid_intervals = 0;

	/* Calculate intervals between frames */
	for (i = 1; i < count; i++) {
		int prev_idx = (c->frame_time_idx - count + i - 1 + 32) % 32;
		int curr_idx = (c->frame_time_idx - count + i + 32) % 32;
		uint64_t interval = c->frame_times[curr_idx] - c->frame_times[prev_idx];

		/* Filter out unreasonable intervals (< 5ms or > 200ms) */
		if (interval >= 5 && interval <= 200) {
			intervals[valid_intervals++] = interval;
			sum += interval;
		}
	}

	if (valid_intervals < 8)
		return 0.0f;

	avg_interval = (double)sum / valid_intervals;

	/* Calculate variance to check for stable frame rate */
	for (i = 0; i < valid_intervals; i++) {
		int64_t diff = (int64_t)intervals[i] - (int64_t)(avg_interval + 0.5);
		variance_sum += (uint64_t)(diff * diff);
	}
	variance = (double)variance_sum / valid_intervals;
	stddev = sqrt(variance);

	/* If standard deviation is more than 30% of mean, not stable enough */
	if (stddev > avg_interval * 0.30) {
		wlr_log(WLR_DEBUG, "Frame rate detection: stddev=%.2f (%.1f%%) too high for avg_interval=%.2fms",
				stddev, (stddev / avg_interval) * 100.0, avg_interval);
		return 0.0f;
	}

	/* Calculate exact Hz from average interval */
	exact_hz = 1000.0 / avg_interval;

	wlr_log(WLR_DEBUG, "Frame rate detection: avg_interval=%.3fms -> %.3f Hz (stddev=%.2f)",
			avg_interval, exact_hz, stddev);

	/*
	 * Map to exact standard framerates if within tolerance.
	 * This accounts for the 1000/1001 pulldown used in NTSC standards.
	 */
	/* 23.976 Hz (24000/1001) - Film NTSC pulldown */
	if (exact_hz >= 23.5 && exact_hz <= 24.5) {
		if (exact_hz < 24.0)
			return 23.976f;
		return 24.0f;
	}
	/* 25 Hz - PAL */
	if (exact_hz >= 24.5 && exact_hz <= 25.5)
		return 25.0f;
	/* 29.97 Hz (30000/1001) - NTSC */
	if (exact_hz >= 29.5 && exact_hz <= 30.5) {
		if (exact_hz < 30.0)
			return 29.97f;
		return 30.0f;
	}
	/* 47.952 Hz (48000/1001) - 2x Film NTSC */
	if (exact_hz >= 47.5 && exact_hz <= 48.5) {
		if (exact_hz < 48.0)
			return 47.952f;
		return 48.0f;
	}
	/* 50 Hz - PAL high frame rate */
	if (exact_hz >= 49.5 && exact_hz <= 50.5)
		return 50.0f;
	/* 59.94 Hz (60000/1001) - NTSC high frame rate */
	if (exact_hz >= 59.0 && exact_hz <= 60.5) {
		if (exact_hz < 60.0)
			return 59.94f;
		return 60.0f;
	}
	/* 119.88 Hz (120000/1001) */
	if (exact_hz >= 119.0 && exact_hz <= 120.5) {
		if (exact_hz < 120.0)
			return 119.88f;
		return 120.0f;
	}

	/* If not a standard rate but stable, return the exact measured value */
	if (exact_hz >= 10.0 && exact_hz <= 240.0) {
		wlr_log(WLR_DEBUG, "Non-standard framerate detected: %.3f Hz", exact_hz);
		return (float)exact_hz;
	}

	return 0.0f;
}

float
calculate_judder_ms(float video_hz, float display_hz)
{
	if (video_hz <= 0.0f || display_hz <= 0.0f)
		return 999.0f;

	float ratio = display_hz / video_hz;
	int nearest_multiple = (int)(ratio + 0.5f);

	if (nearest_multiple < 1)
		nearest_multiple = 1;

	float perfect_hz = video_hz * nearest_multiple;
	float hz_error = fabsf(display_hz - perfect_hz);

	/* Convert Hz error to timing error per frame */
	float frame_period_ms = 1000.0f / video_hz;
	float timing_error_ratio = hz_error / display_hz;

	return frame_period_ms * timing_error_ratio;
}

float
score_video_mode(int method, float video_hz, float display_hz, int multiplier)
{
	float score = 0.0f;
	float judder = calculate_judder_ms(video_hz, display_hz);

	switch (method) {
	case 3: /* VRR - best possible */
		score = 150.0f;
		break;
	case 1: /* Existing mode - good if it's a perfect multiple */
	case 2: /* Custom CVT mode */
		if (judder < 0.1f) {
			/* Near-perfect sync */
			score = 100.0f;
			/* Bonus for lower multiplier (less frame repeats) */
			score += (10 - multiplier) * 2.0f;
		} else {
			/* Imperfect sync - penalize heavily */
			score = 50.0f - judder * 10.0f;
		}
		break;
	default:
		score = 0.0f;
	}

	return score;
}

struct wlr_output_mode *
find_video_friendly_mode(Monitor *m, float target_hz)
{
	struct wlr_output_mode *mode, *best = NULL;
	int width, height;
	float best_diff = 999.0f;

	if (!m || !m->wlr_output || !m->wlr_output->current_mode || target_hz <= 0.0f)
		return NULL;

	width = m->wlr_output->current_mode->width;
	height = m->wlr_output->current_mode->height;

	wl_list_for_each(mode, &m->wlr_output->modes, link) {
		/* Must match current resolution */
		if (mode->width != width || mode->height != height)
			continue;

		float mode_hz = mode->refresh / 1000.0f;

		/* Check if this refresh rate is an integer multiple of target Hz */
		float ratio = mode_hz / target_hz;
		int multiple = (int)(ratio + 0.5f);

		if (multiple >= 1 && multiple <= 8) {
			float expected_hz = target_hz * multiple;
			float diff = fabsf(mode_hz - expected_hz);

			/* Allow 0.5% tolerance for fractional rates */
			if (diff < expected_hz * 0.005f && diff < best_diff) {
				best = mode;
				best_diff = diff;
			}
		}
	}

	return best;
}

int
enable_vrr_video_mode(Monitor *m, float video_hz)
{
	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return 0;

	if (!m->vrr_capable) {
		wlr_log(WLR_DEBUG, "Monitor %s does not support VRR", m->wlr_output->name);
		return 0;
	}

	if (!fullscreen_adaptive_sync_enabled) {
		wlr_log(WLR_DEBUG, "Adaptive sync disabled by user");
		return 0;
	}

	/* Save original mode before first switch. */
	if (!m->video_mode_active && !m->vrr_active)
		m->original_mode = m->wlr_output->current_mode;

	/* Defer the actual DRM commit to the next rendermon() frame commit.
	 * A standalone adaptive_sync commit (no buffer) is BLOCKING in the
	 * DRM backend, freezing the compositor for up to seconds on HDMI.
	 * By piggybacking on the next buffer commit, the change becomes
	 * non-blocking (DRM_MODE_ATOMIC_NONBLOCK). */
	m->vrr_pending = 1;
	m->vrr_pending_hz = video_hz;
	request_frame(m);

	wlr_log(WLR_DEBUG, "VRR enable deferred for %.3f Hz video on %s",
			video_hz, m->wlr_output->name);
	return 1;
}

void
disable_vrr_video_mode(Monitor *m)
{
	if (!m || !m->wlr_output || !m->wlr_output->enabled)
		return;

	if (!m->vrr_active && m->vrr_pending != 1) {
		/* Nothing to disable — neither active nor pending enable */
		return;
	}

	/* Defer the actual DRM commit to the next rendermon() frame commit.
	 * Same rationale as enable: standalone VRR commit is blocking. */
	m->vrr_pending = -1;
	m->vrr_pending_hz = 0.0f;
	request_frame(m);

	wlr_log(WLR_DEBUG, "VRR disable deferred on %s", m->wlr_output->name);
}

int
apply_best_video_mode(Monitor *m, float video_hz)
{
	VideoModeCandidate best;
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;
	drmModeModeInfo drm_mode;
	struct wlr_output_mode *new_mode;
	char osd_msg[64];
	int success = 0;

	if (!m || !m->wlr_output || !m->wlr_output->enabled || video_hz <= 0.0f)
		return 0;

	/* Find the best option using scoring system */
	best = find_best_video_mode(m, video_hz);

	if (best.score <= 0) {
		wlr_log(WLR_ERROR, "No suitable video mode found for %.3f Hz", video_hz);
		show_hz_osd(m, "No compatible mode");
		return 0;
	}

	/* Save original mode before first switch */
	if (!m->video_mode_active && !m->vrr_active)
		m->original_mode = m->wlr_output->current_mode;

	switch (best.method) {
	case 3: /* VRR - best option */
		success = enable_vrr_video_mode(m, video_hz);
		break;

	case 1: /* Existing mode */
		if (!best.mode) {
			wlr_log(WLR_ERROR, "Best method is existing mode but no mode set");
			break;
		}

		wlr_output_state_init(&state);
		wlr_output_state_set_mode(&state, best.mode);

		if (wlr_output_test_state(m->wlr_output, &state) &&
		    wlr_output_commit_state(m->wlr_output, &state)) {
			m->video_mode_active = 1;
			success = 1;

			config = wlr_output_configuration_v1_create();
			config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
			config_head->state.mode = best.mode;
			wlr_output_manager_v1_set_configuration(output_mgr, config);

			/* Show OSD */
			if (best.multiplier > 1) {
				snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz (%dx%.0f)",
						best.mode->refresh / 1000, best.mode->refresh % 1000,
						best.multiplier, video_hz);
			} else {
				snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz",
						best.mode->refresh / 1000, best.mode->refresh % 1000);
			}
			show_hz_osd(m, osd_msg);

			wlr_log(WLR_DEBUG, "Applied existing mode %d.%03d Hz for %.3f Hz video",
					best.mode->refresh / 1000, best.mode->refresh % 1000, video_hz);
		}
		wlr_output_state_finish(&state);
		break;

	case 2: /* Custom CVT mode */
		if (!wlr_output_is_drm(m->wlr_output)) {
			wlr_log(WLR_ERROR, "CVT mode requires DRM output");
			break;
		}

		/* Generate CVT mode */
		generate_cvt_mode(&drm_mode,
				m->wlr_output->current_mode->width,
				m->wlr_output->current_mode->height,
				best.actual_hz);

		new_mode = wlr_drm_connector_add_mode(m->wlr_output, &drm_mode);
		if (!new_mode) {
			wlr_log(WLR_ERROR, "Failed to add CVT mode for %.3f Hz", best.actual_hz);
			break;
		}

		wlr_output_state_init(&state);
		wlr_output_state_set_mode(&state, new_mode);

		if (wlr_output_test_state(m->wlr_output, &state) &&
		    wlr_output_commit_state(m->wlr_output, &state)) {
			m->video_mode_active = 1;
			success = 1;

			config = wlr_output_configuration_v1_create();
			config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
			config_head->state.mode = new_mode;
			wlr_output_manager_v1_set_configuration(output_mgr, config);

			/* Show OSD */
			int actual_mhz = (int)(best.actual_hz * 1000.0f + 0.5f);
			int source_mhz = (int)(video_hz * 1000.0f + 0.5f);
			if (best.multiplier > 1) {
				snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz (%dx %d.%03d)",
						actual_mhz / 1000, actual_mhz % 1000,
						best.multiplier,
						source_mhz / 1000, source_mhz % 1000);
			} else {
				snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz",
						actual_mhz / 1000, actual_mhz % 1000);
			}
			show_hz_osd(m, osd_msg);

			wlr_log(WLR_DEBUG, "Applied CVT mode %.3f Hz for %.3f Hz video",
					best.actual_hz, video_hz);
		}
		wlr_output_state_finish(&state);
		break;
	}

	if (!success) {
		wlr_log(WLR_ERROR, "Failed to apply best video mode (method=%d, hz=%.3f)",
				best.method, best.actual_hz);
	}

	return success;
}

int
set_custom_video_mode(Monitor *m, float exact_hz)
{
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;
	struct wlr_output_mode *new_mode, *friendly_mode;
	drmModeModeInfo drm_mode;
	int width, height;
	float actual_hz;
	int multiplier;
	int success = 0;
	char osd_msg[64];

	if (!m || !m->wlr_output || !m->wlr_output->enabled || !m->wlr_output->current_mode)
		return 0;

	if (exact_hz <= 0.0f)
		return 0;

	/*
	 * PRIORITY 1: Try VRR first - the best solution for judder-free video.
	 * With VRR, the display adapts to each frame, so we don't need to match
	 * refresh rates at all. This is how high-end displays handle video.
	 */
	if (m->vrr_capable && enable_vrr_video_mode(m, exact_hz)) {
		wlr_log(WLR_DEBUG, "Using VRR for %.3f Hz video - optimal solution", exact_hz);
		return 1;
	}

	/* Check if this is a DRM output - needed for mode switching */
	if (!wlr_output_is_drm(m->wlr_output)) {
		wlr_log(WLR_INFO, "Output is not DRM, cannot change mode");
		return 0;
	}

	/* Save original mode before first switch */
	if (!m->video_mode_active)
		m->original_mode = m->wlr_output->current_mode;

	width = m->wlr_output->current_mode->width;
	height = m->wlr_output->current_mode->height;

	/*
	 * PRIORITY 2: Check if there's an existing mode that's a multiple of video Hz.
	 * Many displays have modes like 120Hz which work perfectly for 24Hz content.
	 */
	friendly_mode = find_video_friendly_mode(m, exact_hz);
	if (friendly_mode) {
		wlr_output_state_init(&state);
		wlr_output_state_set_mode(&state, friendly_mode);

		if (wlr_output_test_state(m->wlr_output, &state)) {
			if (wlr_output_commit_state(m->wlr_output, &state)) {
				success = 1;
				m->video_mode_active = 1;
				actual_hz = friendly_mode->refresh / 1000.0f;
				multiplier = (int)(actual_hz / exact_hz + 0.5f);

				config = wlr_output_configuration_v1_create();
				config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
				config_head->state.mode = friendly_mode;
				wlr_output_manager_v1_set_configuration(output_mgr, config);

				wlr_log(WLR_DEBUG, "Using existing mode %d.%03d Hz for %.3f Hz video (%dx)",
						friendly_mode->refresh / 1000, friendly_mode->refresh % 1000,
						exact_hz, multiplier);

				/* Show OSD */
				snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz (%dx%.0f)",
						friendly_mode->refresh / 1000, friendly_mode->refresh % 1000,
						multiplier, exact_hz);
				show_hz_osd(m, osd_msg);

				wlr_output_state_finish(&state);
				return 1;
			}
		}
		wlr_output_state_finish(&state);
	}

	/*
	 * PRIORITY 3: Generate custom CVT mode.
	 * Multiply Hz to stay above panel minimum (typically 48 Hz).
	 */
	multiplier = 1;
	if (exact_hz < 48.0f) {
		multiplier = (int)ceilf(48.0f / exact_hz);
	}

	wlr_log(WLR_DEBUG, "Video mode: no VRR, no friendly mode, trying CVT at %dx multiplier",
			multiplier);

	for (; multiplier <= 8 && !success; multiplier++) {
		actual_hz = exact_hz * multiplier;

		/* Skip if above typical maximum */
		if (actual_hz > 300.0f)
			break;

		wlr_log(WLR_DEBUG, "Video mode: trying %.3f Hz -> %.3f Hz (%dx multiplier)",
				exact_hz, actual_hz, multiplier);

		/* Generate CVT mode with exact timing parameters */
		generate_cvt_mode(&drm_mode, width, height, actual_hz);

		/* Add custom mode to DRM connector */
		new_mode = wlr_drm_connector_add_mode(m->wlr_output, &drm_mode);
		if (!new_mode) {
			wlr_log(WLR_DEBUG, "wlr_drm_connector_add_mode failed for %.3f Hz", actual_hz);
			continue;
		}

		wlr_log(WLR_DEBUG, "Added custom mode: %dx%d@%d mHz",
				new_mode->width, new_mode->height, new_mode->refresh);

		/* Try to apply the new mode */
		wlr_output_state_init(&state);
		wlr_output_state_set_mode(&state, new_mode);

		if (wlr_output_test_state(m->wlr_output, &state)) {
			if (wlr_output_commit_state(m->wlr_output, &state)) {
				success = 1;
				m->video_mode_active = 1;

				config = wlr_output_configuration_v1_create();
				config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
				config_head->state.mode = new_mode;
				wlr_output_manager_v1_set_configuration(output_mgr, config);

				wlr_log(WLR_DEBUG, "Custom mode %.3f Hz applied for %.3f Hz video (%dx)",
						actual_hz, exact_hz, multiplier);
			} else {
				wlr_log(WLR_DEBUG, "wlr_output_commit_state failed for %.3f Hz", actual_hz);
			}
		} else {
			wlr_log(WLR_DEBUG, "wlr_output_test_state failed for %.3f Hz", actual_hz);
		}
		wlr_output_state_finish(&state);
	}

	/* Show OSD notification */
	if (success) {
		int actual_mhz = (int)(actual_hz * 1000.0f + 0.5f);
		int source_mhz = (int)(exact_hz * 1000.0f + 0.5f);
		if (multiplier > 1) {
			snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz (%dx %d.%03d)",
					actual_mhz / 1000, actual_mhz % 1000,
					multiplier,
					source_mhz / 1000, source_mhz % 1000);
		} else {
			snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz",
					actual_mhz / 1000, actual_mhz % 1000);
		}
		show_hz_osd(m, osd_msg);
	} else {
		wlr_log(WLR_ERROR, "Failed to set video mode for %.3f Hz (VRR: %s, no friendly modes)",
				exact_hz, m->vrr_capable ? "unavailable" : "not supported");
	}

	return success;
}

int
video_check_timeout(void *data)
{
	(void)data;
	check_fullscreen_video();
	return 0;
}

/*
 * Immediately invalidate all video pacing state on a monitor.
 * Called from tag-switch functions (view, toggleview, tag, toggletag)
 * so that rendermon never sees stale cadence/VRR state from the
 * previous tag's fullscreen video.  Without this, the first rendermon
 * after a tag switch can enter the cadence hold path or keep VRR at
 * video Hz, freezing the display until the 50ms game-mode debounce
 * fires.
 */
void
invalidate_video_pacing(Monitor *m)
{
	if (!m)
		return;

	/* Enable diagnostic output for first 5 frames after tag switch.
	 * Uses write(STDERR_FILENO) for unbuffered, immediate output
	 * that survives compositor freezes. */
	m->tag_switch_debug = 5;

	/* Fully invalidate the classify cache — clear BOTH the client
	 * pointer AND the cached result flags.  Setting only the pointer
	 * to NULL is not enough: if the new tag has no fullscreen client,
	 * focustop() returns NULL which matches the NULL cache key,
	 * returning stale is_video=1 / is_game values from the old tag.
	 * That stale is_video poisons idle-throttle, content-type hints,
	 * and can keep the monitor spinning at full refresh forever. */
	m->classify_cache_client = NULL;
	m->classify_cache_video = 0;
	m->classify_cache_game = 0;
	m->classify_cache_tearing = 0;

	/* Cancel any pending VRR enable — tag switch means the video that
	 * requested VRR is no longer visible.  If VRR is already active,
	 * schedule a deferred disable so it piggybacks on the next frame
	 * commit (non-blocking). */
	if (m->vrr_pending == 1) {
		m->vrr_pending = 0;
		wlr_log(WLR_DEBUG, "Pending VRR enable cancelled (tag switch)");
	}
	if (m->vrr_active) {
		m->vrr_pending = -1;
		wlr_log(WLR_DEBUG, "VRR disable scheduled (tag switch)");
	}

	/* Deactivate Bresenham video cadence immediately */
	if (m->video_cadence_active) {
		m->video_cadence_active = 0;
		m->video_cadence_counter = 0;
		m->video_cadence_accum = 0.0f;
		wlr_log(WLR_DEBUG, "Video cadence invalidated (tag switch)");
	}

	/* Clear video-specific frame pacing (but not game pacing) */
	if (m->frame_pacing_active && !m->game_vrr_active) {
		m->frame_pacing_active = 0;
		m->estimated_game_fps = 0.0f;
	}

	/* Force full damage so the first rendermon after tag switch
	 * builds a complete frame (desktop content) instead of
	 * potentially finding no damage and skipping the commit. */
	if (m->scene_output)
		wlr_damage_ring_add_whole(&m->scene_output->damage_ring);

	/* Ensure rendermon fires promptly even if no pageflip is
	 * pending (e.g. last rendermon was a cadence hold that
	 * returned without committing). */
	request_frame(m);

	/* Schedule a video check so cadence/VRR is re-established
	 * when switching back to a tag with a fullscreen video.
	 * check_fullscreen_video handles phase==2 re-establishment. */
	schedule_video_check(200);
}

void
schedule_video_check(uint32_t ms)
{
	if (!event_loop)
		return;
	if (!video_check_timer)
		video_check_timer = wl_event_loop_add_timer(event_loop,
				video_check_timeout, NULL);
	if (video_check_timer)
		wl_event_source_timer_update(video_check_timer, ms);
}

int
hz_osd_timeout(void *data)
{
	Monitor *m;
	(void)data;

	wl_list_for_each(m, &mons, link) {
		if (m->hz_osd_visible)
			hide_hz_osd(m);
	}
	return 0;
}

void
hide_hz_osd(Monitor *m)
{
	if (!m || !m->hz_osd_tree)
		return;

	m->hz_osd_visible = 0;
	wlr_scene_node_set_enabled(&m->hz_osd_tree->node, 0);
}

void
show_hz_osd(Monitor *m, const char *msg)
{
	tll(struct GlyphRun) glyphs = tll_init();
	const struct fcft_glyph *glyph;
	struct wlr_scene_buffer *scene_buf;
	struct wlr_buffer *buffer;
	uint32_t prev_cp = 0;
	int pen_x = 0;
	int min_y = INT_MAX, max_y = INT_MIN;
	int padding = 20;
	int box_width, box_height;
	int osd_x, osd_y;
	static const float osd_bg[4] = {0.1f, 0.1f, 0.1f, 0.85f};

	if (!m || !statusfont.font || !msg || !*msg)
		return;

	/* Don't render OSD if direct scanout is active on this monitor.
	 * Any visible scene node in LyrOverlay would immediately break
	 * scanout, forcing GPU composition for the 3-second OSD duration.
	 * Log the message instead so it's still visible in debug output. */
	if (m->direct_scanout_active) {
		wlr_log(WLR_INFO, "OSD suppressed (scanout active on %s): %s",
			m->wlr_output->name, msg);
		return;
	}

	/* Create tree if needed */
	if (!m->hz_osd_tree) {
		m->hz_osd_tree = wlr_scene_tree_create(layers[LyrOverlay]);
		if (!m->hz_osd_tree)
			return;
		m->hz_osd_bg = NULL;
		m->hz_osd_visible = 0;
	}

	/* Build glyph list and measure text */
	for (size_t i = 0; msg[i]; i++) {
		long kern_x = 0, kern_y = 0;
		uint32_t cp = (unsigned char)msg[i];

		if (prev_cp)
			fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
		pen_x += (int)kern_x;

		glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
		if (glyph && glyph->pix) {
			tll_push_back(glyphs, ((struct GlyphRun){
				.glyph = glyph,
				.pen_x = pen_x,
				.codepoint = cp,
			}));
			if (-glyph->y < min_y)
				min_y = -glyph->y;
			if (-glyph->y + (int)glyph->height > max_y)
				max_y = -glyph->y + (int)glyph->height;
			pen_x += glyph->advance.x;
		}
		prev_cp = cp;
	}

	if (tll_length(glyphs) == 0) {
		tll_free(glyphs);
		return;
	}

	box_width = pen_x + padding * 2;
	box_height = (max_y - min_y) + padding * 2;

	/* Center on monitor */
	osd_x = m->m.x + (m->m.width - box_width) / 2;
	osd_y = m->m.y + (m->m.height - box_height) / 2;

	/* Clear old children */
	struct wlr_scene_node *node, *tmp;
	wl_list_for_each_safe(node, tmp, &m->hz_osd_tree->children, link)
		wlr_scene_node_destroy(node);
	m->hz_osd_bg = NULL;

	/* Create background */
	m->hz_osd_bg = wlr_scene_tree_create(m->hz_osd_tree);
	if (m->hz_osd_bg)
		drawrect(m->hz_osd_bg, 0, 0, box_width, box_height, osd_bg);

	/* Draw glyphs */
	{
		int origin_y = padding - min_y;

		tll_foreach(glyphs, it) {
			glyph = it->item.glyph;
			buffer = statusbar_buffer_from_glyph(glyph);
			if (!buffer)
				continue;

			scene_buf = wlr_scene_buffer_create(m->hz_osd_tree, NULL);
			if (scene_buf) {
				wlr_scene_buffer_set_buffer(scene_buf, buffer);
				wlr_scene_node_set_position(&scene_buf->node,
						padding + it->item.pen_x + glyph->x,
						origin_y - glyph->y);
			}
			wlr_buffer_drop(buffer);
		}
	}

	tll_free(glyphs);

	/* Position and show */
	wlr_scene_node_set_position(&m->hz_osd_tree->node, osd_x, osd_y);
	wlr_scene_node_set_enabled(&m->hz_osd_tree->node, 1);
	m->hz_osd_visible = 1;

	/* Schedule auto-hide after 3 seconds */
	if (!hz_osd_timer)
		hz_osd_timer = wl_event_loop_add_timer(event_loop, hz_osd_timeout, NULL);
	if (hz_osd_timer)
		wl_event_source_timer_update(hz_osd_timer, 3000);
}

void
testhzosd(const Arg *arg)
{
	Monitor *m = selmon;
	char osd_msg[64];
	int refresh_mhz;

	if (!m || !m->wlr_output)
		return;

	/* Use current_mode->refresh for accurate active refresh rate */
	if (m->wlr_output->current_mode)
		refresh_mhz = m->wlr_output->current_mode->refresh;
	else
		refresh_mhz = m->wlr_output->refresh;

	snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz",
			refresh_mhz / 1000, refresh_mhz % 1000);
	show_hz_osd(m, osd_msg);
}

int
playback_osd_timeout(void *data)
{
	(void)data;
	if (playback_state == PLAYBACK_PLAYING &&
	    active_videoplayer && active_videoplayer->state == VP_STATE_PLAYING) {
		hide_playback_osd();
	}
	return 0;
}

void
hide_playback_osd(void)
{
	if (playback_osd_tree)
		wlr_scene_node_set_enabled(&playback_osd_tree->node, 0);
	osd_visible = 0;
}

void
render_playback_osd(void)
{
	Monitor *m = selmon;
	struct wlr_scene_node *node, *tmp;
	struct wlr_scene_tree *text_tree;
	StatusModule mod = {0};
	int bar_h = 50;
	int bar_y, bar_w;
	int padding = 15;
	int btn_w = 100;
	int btn_h = 36;
	int menu_w = 180;
	int menu_item_h = 32;
	float bar_bg[4] = {0.08f, 0.08f, 0.12f, 0.92f};
	float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float dim_text[4] = {0.7f, 0.7f, 0.7f, 1.0f};
	float btn_bg[4] = {0.15f, 0.15f, 0.22f, 0.95f};
	float btn_selected[4] = {0.2f, 0.5f, 1.0f, 0.95f};
	float menu_bg[4] = {0.1f, 0.1f, 0.15f, 0.98f};
	float menu_selected[4] = {0.2f, 0.45f, 0.85f, 0.95f};

	if (!m || !statusfont.font || playback_state != PLAYBACK_PLAYING)
		return;

	bar_w = m->m.width;
	bar_y = m->m.height - bar_h;

	/* Create tree if needed - must be on LyrBlock (same as video player)
	 * so it renders above the video. Later siblings render on top. */
	if (!playback_osd_tree) {
		playback_osd_tree = wlr_scene_tree_create(layers[LyrBlock]);
		if (!playback_osd_tree)
			return;
	}

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &playback_osd_tree->children, link) {
		wlr_scene_node_destroy(node);
	}

	/* Bar background */
	drawrect(playback_osd_tree, 0, 0, bar_w, bar_h, bar_bg);

	/* Left side: Playing/Paused status */
	{
		int is_paused = active_videoplayer && active_videoplayer->state == VP_STATE_PAUSED;
		const char *status = is_paused ? "Paused" : "Playing";
		float status_box_bg[4] = {0.12f, 0.12f, 0.18f, 0.95f};
		int status_w = 90;

		drawrect(playback_osd_tree, padding, (bar_h - btn_h) / 2, status_w, btn_h, status_box_bg);

		text_tree = wlr_scene_tree_create(playback_osd_tree);
		if (text_tree) {
			wlr_scene_node_set_position(&text_tree->node, padding + 12, (bar_h - 16) / 2);
			mod.tree = text_tree;
			tray_render_label(&mod, status, 0, 16, text_color);
		}
	}

	/* Right side: Sound and Subtitles buttons */
	int sub_btn_w = 130;
	int right_x = bar_w - padding - btn_w - 10 - sub_btn_w;  /* Sound + gap + Subtitles */

	/* Progress bar and time display */
	if (active_videoplayer && active_videoplayer->duration_us > 0) {
		int64_t pos = (active_videoplayer->seek_requested || active_videoplayer->control_bar.seek_hold_active)
			? active_videoplayer->seek_target_us
			: active_videoplayer->position_us;
		int64_t dur = active_videoplayer->duration_us;
		float progress_bg[4] = {0.25f, 0.25f, 0.3f, 1.0f};
		float progress_fg[4] = {0.3f, 0.6f, 1.0f, 1.0f};

		/* Time text: "pos / dur" */
		char pos_str[16], dur_str[16], time_str[40];
		videoplayer_format_time(pos_str, sizeof(pos_str), pos);
		videoplayer_format_time(dur_str, sizeof(dur_str), dur);
		snprintf(time_str, sizeof(time_str), "%s / %s", pos_str, dur_str);

		int time_w = 140;
		int time_x = right_x - padding - time_w;

		/* Progress bar fills space between status and time */
		int prog_x = padding + 90 + padding;
		int prog_w = time_x - padding - prog_x;
		int prog_h = 6;
		int prog_y = (bar_h - prog_h) / 2;

		if (prog_w > 50) {
			/* Progress bar background */
			drawrect(playback_osd_tree, prog_x, prog_y, prog_w, prog_h, progress_bg);

			/* Progress bar fill */
			float frac = (float)pos / dur;
			if (frac < 0.0f) frac = 0.0f;
			if (frac > 1.0f) frac = 1.0f;
			int fill_w = (int)(prog_w * frac);
			if (fill_w > 0)
				drawrect(playback_osd_tree, prog_x, prog_y, fill_w, prog_h, progress_fg);
		}

		/* Time text */
		text_tree = wlr_scene_tree_create(playback_osd_tree);
		if (text_tree) {
			wlr_scene_node_set_position(&text_tree->node, time_x, (bar_h - 14) / 2);
			mod.tree = text_tree;
			tray_render_label(&mod, time_str, 0, 14, dim_text);
		}
	}

	/* Sound button */
	{
		float *bg = (osd_menu_open == OSD_MENU_SOUND) ? btn_selected : btn_bg;
		drawrect(playback_osd_tree, right_x, (bar_h - btn_h) / 2, btn_w, btn_h, bg);

		text_tree = wlr_scene_tree_create(playback_osd_tree);
		if (text_tree) {
			wlr_scene_node_set_position(&text_tree->node, right_x + 25, (bar_h - 16) / 2);
			mod.tree = text_tree;
			tray_render_label(&mod, "Sound", 0, 16, text_color);
		}
	}

	/* Subtitles button (wider to fit text) */
	{
		int sub_x = right_x + btn_w + 10;
		float *bg = (osd_menu_open == OSD_MENU_SUBTITLES) ? btn_selected : btn_bg;
		drawrect(playback_osd_tree, sub_x, (bar_h - btn_h) / 2, sub_btn_w, btn_h, bg);

		text_tree = wlr_scene_tree_create(playback_osd_tree);
		if (text_tree) {
			wlr_scene_node_set_position(&text_tree->node, sub_x + 20, (bar_h - 16) / 2);
			mod.tree = text_tree;
			tray_render_label(&mod, "Subtitles", 0, 16, text_color);
		}
	}

	/* Sound menu (opens upward from button) */
	if (osd_menu_open == OSD_MENU_SOUND && audio_track_count > 0) {
		int menu_h = audio_track_count * menu_item_h + 8;
		int menu_x = right_x;
		int menu_y = -menu_h - 5;  /* Above the bar */

		drawrect(playback_osd_tree, menu_x, menu_y, menu_w, menu_h, menu_bg);

		int item_y = menu_y + 4;
		for (int i = 0; i < audio_track_count; i++) {
			float *item_bg = (i == osd_menu_selection) ? menu_selected : menu_bg;
			if (i == osd_menu_selection) {
				drawrect(playback_osd_tree, menu_x + 2, item_y, menu_w - 4, menu_item_h - 2, item_bg);
			}

			char label[160];
			if (audio_tracks[i].lang[0] && audio_tracks[i].title[0])
				snprintf(label, sizeof(label), "%s (%s)", audio_tracks[i].lang, audio_tracks[i].title);
			else if (audio_tracks[i].lang[0])
				snprintf(label, sizeof(label), "%s", audio_tracks[i].lang);
			else if (audio_tracks[i].title[0])
				snprintf(label, sizeof(label), "%s", audio_tracks[i].title);
			else
				snprintf(label, sizeof(label), "Track %d", audio_tracks[i].id);

			text_tree = wlr_scene_tree_create(playback_osd_tree);
			if (text_tree) {
				wlr_scene_node_set_position(&text_tree->node, menu_x + 10, item_y + 8);
				mod.tree = text_tree;
				float *tc = (i == osd_menu_selection) ? text_color : dim_text;
				tray_render_label(&mod, label, 0, 14, tc);
			}
			item_y += menu_item_h;
		}
	}

	/* Subtitles menu (opens upward from button) */
	if (osd_menu_open == OSD_MENU_SUBTITLES) {
		int items = subtitle_track_count + 1;  /* +1 for "Off" */
		int menu_h = items * menu_item_h + 8;
		int menu_x = right_x + btn_w + 10;
		int menu_y = -menu_h - 5;  /* Above the bar */

		drawrect(playback_osd_tree, menu_x, menu_y, menu_w, menu_h, menu_bg);

		int item_y = menu_y + 4;

		/* "Off" option */
		{
			float *item_bg = (0 == osd_menu_selection) ? menu_selected : menu_bg;
			if (0 == osd_menu_selection) {
				drawrect(playback_osd_tree, menu_x + 2, item_y, menu_w - 4, menu_item_h - 2, item_bg);
			}

			text_tree = wlr_scene_tree_create(playback_osd_tree);
			if (text_tree) {
				wlr_scene_node_set_position(&text_tree->node, menu_x + 10, item_y + 8);
				mod.tree = text_tree;
				float *tc = (0 == osd_menu_selection) ? text_color : dim_text;
				tray_render_label(&mod, "Off", 0, 14, tc);
			}
			item_y += menu_item_h;
		}

		/* Subtitle tracks */
		for (int i = 0; i < subtitle_track_count; i++) {
			int sel_idx = i + 1;  /* +1 because "Off" is index 0 */
			float *item_bg = (sel_idx == osd_menu_selection) ? menu_selected : menu_bg;
			if (sel_idx == osd_menu_selection) {
				drawrect(playback_osd_tree, menu_x + 2, item_y, menu_w - 4, menu_item_h - 2, item_bg);
			}

			char label[160];
			if (subtitle_tracks[i].lang[0] && subtitle_tracks[i].title[0])
				snprintf(label, sizeof(label), "%s (%s)", subtitle_tracks[i].lang, subtitle_tracks[i].title);
			else if (subtitle_tracks[i].lang[0])
				snprintf(label, sizeof(label), "%s", subtitle_tracks[i].lang);
			else if (subtitle_tracks[i].title[0])
				snprintf(label, sizeof(label), "%s", subtitle_tracks[i].title);
			else
				snprintf(label, sizeof(label), "Track %d", subtitle_tracks[i].id);

			text_tree = wlr_scene_tree_create(playback_osd_tree);
			if (text_tree) {
				wlr_scene_node_set_position(&text_tree->node, menu_x + 10, item_y + 8);
				mod.tree = text_tree;
				float *tc = (sel_idx == osd_menu_selection) ? text_color : dim_text;
				tray_render_label(&mod, label, 0, 14, tc);
			}
			item_y += menu_item_h;
		}
	}

	/* Position and show - raise to top so OSD is always above video player */
	wlr_scene_node_raise_to_top(&playback_osd_tree->node);
	wlr_scene_node_set_position(&playback_osd_tree->node, m->m.x, m->m.y + bar_y);
	wlr_scene_node_set_enabled(&playback_osd_tree->node, 1);
	osd_visible = 1;

	/* Schedule auto-hide (2 seconds) when playing, not paused */
	int is_playing = active_videoplayer && active_videoplayer->state == VP_STATE_PLAYING;
	if (is_playing && osd_menu_open == OSD_MENU_NONE) {
		if (!playback_osd_timer)
			playback_osd_timer = wl_event_loop_add_timer(event_loop, playback_osd_timeout, NULL);
		if (playback_osd_timer)
			wl_event_source_timer_update(playback_osd_timer, 2000);
	}
}

void
generate_cvt_mode(drmModeModeInfo *mode, int hdisplay, int vdisplay, float vrefresh)
{
	/* CVT-RB (Reduced Blanking) constants - better for modern displays */
	const int RB_H_BLANK = 160;       /* Horizontal blanking pixels */
	const int RB_H_SYNC = 32;         /* Horizontal sync width */
	const int RB_V_FPORCH = 3;        /* Vertical front porch lines */
	const int RB_MIN_V_BLANK = 460;   /* Minimum vertical blank time (us) */

	int h_sync_start, h_sync_end, h_total;
	int v_sync_start, v_sync_end, v_total;
	int v_sync, v_back_porch, v_blank;
	float h_period, pixel_clock;

	memset(mode, 0, sizeof(*mode));

	/* Round hdisplay to multiple of 8 (CVT granularity) */
	hdisplay = (hdisplay / 8) * 8;

	/* Determine vsync lines based on aspect ratio */
	float aspect = (float)hdisplay / vdisplay;
	if (fabsf(aspect - 4.0f/3.0f) < 0.01f)
		v_sync = 4;
	else if (fabsf(aspect - 16.0f/9.0f) < 0.01f)
		v_sync = 5;
	else if (fabsf(aspect - 16.0f/10.0f) < 0.01f)
		v_sync = 6;
	else if (fabsf(aspect - 5.0f/4.0f) < 0.01f)
		v_sync = 7;
	else if (fabsf(aspect - 15.0f/9.0f) < 0.01f)
		v_sync = 7;
	else
		v_sync = 10; /* Default for other ratios */

	/* CVT-RB horizontal timings */
	h_total = hdisplay + RB_H_BLANK;
	h_sync_start = hdisplay + 48; /* Front porch = 48 pixels */
	h_sync_end = h_sync_start + RB_H_SYNC;

	/* Calculate horizontal period and vertical blanking */
	h_period = (1000000.0f / vrefresh - RB_MIN_V_BLANK) / vdisplay;
	v_blank = (int)(RB_MIN_V_BLANK / h_period + 0.5f);
	if (v_blank < RB_V_FPORCH + v_sync + 1)
		v_blank = RB_V_FPORCH + v_sync + 1;

	v_back_porch = v_blank - RB_V_FPORCH - v_sync;
	v_total = vdisplay + v_blank;

	/* CVT-RB vertical timings */
	v_sync_start = vdisplay + RB_V_FPORCH;
	v_sync_end = v_sync_start + v_sync;

	/* Calculate pixel clock in kHz */
	pixel_clock = vrefresh * h_total * v_total / 1000.0f;

	/* Fill in drmModeModeInfo */
	mode->clock = (uint32_t)(pixel_clock + 0.5f);
	mode->hdisplay = hdisplay;
	mode->hsync_start = h_sync_start;
	mode->hsync_end = h_sync_end;
	mode->htotal = h_total;
	mode->vdisplay = vdisplay;
	mode->vsync_start = v_sync_start;
	mode->vsync_end = v_sync_end;
	mode->vtotal = v_total;
	mode->vrefresh = (uint32_t)(vrefresh + 0.5f);
	mode->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC;
	mode->type = DRM_MODE_TYPE_USERDEF;

	snprintf(mode->name, sizeof(mode->name), "%dx%d@%.2f",
			hdisplay, vdisplay, vrefresh);

	wlr_log(WLR_DEBUG, "CVT-RB mode: %s clock=%d htotal=%d vtotal=%d",
			mode->name, mode->clock, h_total, v_total);
	(void)v_back_porch;
}

void
generate_fixed_mode(drmModeModeInfo *mode, const drmModeModeInfo *base, int vrefresh)
{
	*mode = *base;
	if (!vrefresh)
		vrefresh = 60;

	/* Calculate new clock to achieve target refresh rate:
	 * clock (kHz) = htotal * vtotal * vrefresh / 1000
	 * Round up to avoid going below target refresh */
	mode->clock = ((mode->htotal * mode->vtotal * vrefresh) + 999) / 1000;

	/* Recalculate actual refresh rate from the clock we set */
	mode->vrefresh = (1000 * mode->clock) / (mode->htotal * mode->vtotal);

	snprintf(mode->name, sizeof(mode->name), "%dx%d@%d.00",
			mode->hdisplay, mode->vdisplay, vrefresh);
}

void
setcustomhz(const Arg *arg)
{
	Monitor *m = selmon;
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;
	struct wlr_output_mode *new_mode, *current;
	drmModeModeInfo drm_mode, base_drm_mode;
	int width, height;
	float actual_hz, target_hz;
	int multiplier;
	char osd_msg[64];
	int success = 0;

	if (!m || !m->wlr_output || !m->wlr_output->enabled || !m->wlr_output->current_mode)
		return;

	if (arg->f <= 0)
		return;

	/* Check if this is a DRM output - only DRM supports custom modes */
	if (!wlr_output_is_drm(m->wlr_output)) {
		wlr_log(WLR_DEBUG, "Output is not DRM, cannot add custom mode");
		snprintf(osd_msg, sizeof(osd_msg), "Not DRM output");
		show_hz_osd(m, osd_msg);
		return;
	}

	/* Save original mode before first switch */
	if (!m->video_mode_active)
		m->original_mode = m->wlr_output->current_mode;

	current = m->wlr_output->current_mode;
	width = current->width;
	height = current->height;
	target_hz = arg->f;

	/*
	 * Build base DRM mode from current wlr_output_mode.
	 * This preserves the timing parameters that the display already accepts.
	 */
	memset(&base_drm_mode, 0, sizeof(base_drm_mode));
	base_drm_mode.hdisplay = width;
	base_drm_mode.vdisplay = height;
	base_drm_mode.vrefresh = (current->refresh + 500) / 1000;
	base_drm_mode.clock = current->refresh * width * height / 1000000;

	/* Try to get actual DRM timings from preferred mode */
	struct wlr_output_mode *pref;
	wl_list_for_each(pref, &m->wlr_output->modes, link) {
		if (pref->width == width && pref->height == height && pref->preferred) {
			/* Use preferred mode timings as base - these are EDID-verified */
			base_drm_mode.clock = pref->refresh * width * height / 1000000;
			/* Estimate htotal/vtotal from clock and refresh */
			int total_pixels = (pref->refresh > 0) ? (base_drm_mode.clock * 1000000 / pref->refresh) : (width * height);
			/* Assume typical blanking ratio for htotal/vtotal estimation */
			base_drm_mode.htotal = width + 160; /* CVT-RB typical H blank */
			base_drm_mode.vtotal = total_pixels / base_drm_mode.htotal;
			if (base_drm_mode.vtotal < height)
				base_drm_mode.vtotal = height + 50; /* minimum V blank */
			/* Recalculate clock based on actual totals */
			base_drm_mode.clock = (base_drm_mode.htotal * base_drm_mode.vtotal * base_drm_mode.vrefresh + 999) / 1000;
			/* Standard sync positions */
			base_drm_mode.hsync_start = width + 48;
			base_drm_mode.hsync_end = base_drm_mode.hsync_start + 32;
			base_drm_mode.vsync_start = height + 3;
			base_drm_mode.vsync_end = base_drm_mode.vsync_start + 5;
			base_drm_mode.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC;
			wlr_log(WLR_INFO, "Using preferred mode as base: %dx%d htotal=%d vtotal=%d",
					width, height, base_drm_mode.htotal, base_drm_mode.vtotal);
			break;
		}
	}

	/* If no preferred mode found, generate CVT timings as base */
	if (base_drm_mode.htotal == 0) {
		generate_cvt_mode(&base_drm_mode, width, height, (float)base_drm_mode.vrefresh);
		wlr_log(WLR_DEBUG, "Generated CVT base mode: %dx%d htotal=%d vtotal=%d",
				width, height, base_drm_mode.htotal, base_drm_mode.vtotal);
	}

	/*
	 * Calculate starting multiplier to stay above panel minimum (48 Hz).
	 */
	multiplier = 1;
	if (target_hz < 48.0f) {
		multiplier = (int)ceilf(48.0f / target_hz);
	}

	/*
	 * Try FIXED mode generation first (Gamescope method).
	 * This preserves timings and only changes clock - much more likely to work.
	 */
	for (; multiplier <= 8 && !success; multiplier++) {
		int target_vrefresh = (int)(target_hz * multiplier + 0.5f);
		actual_hz = target_hz * multiplier;

		/* Skip if above typical max refresh */
		if (actual_hz > 300.0f)
			break;

		wlr_log(WLR_DEBUG, "Fixed mode: trying %.3f Hz -> %d Hz (%dx multiplier)",
				target_hz, target_vrefresh, multiplier);

		/* Generate fixed mode - only changes clock, preserves all timings */
		generate_fixed_mode(&drm_mode, &base_drm_mode, target_vrefresh);

		wlr_log(WLR_DEBUG, "Fixed mode params: clock=%d htotal=%d vtotal=%d vrefresh=%d",
				drm_mode.clock, drm_mode.htotal, drm_mode.vtotal, drm_mode.vrefresh);

		/* Add custom mode to DRM connector */
		new_mode = wlr_drm_connector_add_mode(m->wlr_output, &drm_mode);
		if (!new_mode) {
			wlr_log(WLR_DEBUG, "wlr_drm_connector_add_mode failed for fixed %d Hz", target_vrefresh);
			continue;
		}

		wlr_log(WLR_DEBUG, "Added fixed mode: %dx%d@%d mHz",
				new_mode->width, new_mode->height, new_mode->refresh);

		/* Try to apply the new mode */
		wlr_output_state_init(&state);
		wlr_output_state_set_mode(&state, new_mode);

		if (wlr_output_test_state(m->wlr_output, &state)) {
			if (wlr_output_commit_state(m->wlr_output, &state)) {
				success = 1;
				m->video_mode_active = 1;

				config = wlr_output_configuration_v1_create();
				config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
				config_head->state.mode = new_mode;
				wlr_output_manager_v1_set_configuration(output_mgr, config);

				wlr_log(WLR_DEBUG, "Fixed mode %d Hz applied (%dx %.3f fps)",
						target_vrefresh, multiplier, target_hz);
			} else {
				wlr_log(WLR_DEBUG, "wlr_output_commit_state failed for fixed %d Hz", target_vrefresh);
			}
		} else {
			wlr_log(WLR_DEBUG, "wlr_output_test_state failed for fixed %d Hz", target_vrefresh);
		}
		wlr_output_state_finish(&state);
	}

	/*
	 * Fallback: Try CVT mode generation if fixed mode didn't work.
	 */
	if (!success) {
		multiplier = 1;
		if (target_hz < 48.0f) {
			multiplier = (int)ceilf(48.0f / target_hz);
		}

		for (; multiplier <= 8 && !success; multiplier++) {
			actual_hz = target_hz * multiplier;

			if (actual_hz > 300.0f)
				break;

			wlr_log(WLR_DEBUG, "CVT fallback: trying %.3f Hz -> %.3f Hz (%dx multiplier)",
					target_hz, actual_hz, multiplier);

			generate_cvt_mode(&drm_mode, width, height, actual_hz);

			new_mode = wlr_drm_connector_add_mode(m->wlr_output, &drm_mode);
			if (!new_mode) {
				wlr_log(WLR_DEBUG, "wlr_drm_connector_add_mode failed for CVT %.3f Hz", actual_hz);
				continue;
			}

			wlr_output_state_init(&state);
			wlr_output_state_set_mode(&state, new_mode);

			if (wlr_output_test_state(m->wlr_output, &state)) {
				if (wlr_output_commit_state(m->wlr_output, &state)) {
					success = 1;
					m->video_mode_active = 1;

					config = wlr_output_configuration_v1_create();
					config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
					config_head->state.mode = new_mode;
					wlr_output_manager_v1_set_configuration(output_mgr, config);

					wlr_log(WLR_DEBUG, "CVT mode %.3f Hz applied (%dx %.3f fps)",
							actual_hz, multiplier, target_hz);
				}
			}
			wlr_output_state_finish(&state);
		}
	}

	/* Fallback: Find existing mode that's a multiple of target Hz */
	if (!success) {
		struct wlr_output_mode *video_mode = find_video_friendly_mode(m, target_hz);

		if (video_mode && video_mode != m->wlr_output->current_mode) {
			wlr_output_state_init(&state);
			wlr_output_state_set_mode(&state, video_mode);

			if (wlr_output_commit_state(m->wlr_output, &state)) {
				success = 1;
				m->video_mode_active = 1;
				actual_hz = video_mode->refresh / 1000.0f;
				multiplier = (int)(actual_hz / target_hz + 0.5f);

				config = wlr_output_configuration_v1_create();
				config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
				config_head->state.mode = video_mode;
				wlr_output_manager_v1_set_configuration(output_mgr, config);

				wlr_log(WLR_DEBUG, "Fallback mode %d.%03d Hz applied (%dx %.3f Hz)",
						video_mode->refresh / 1000, video_mode->refresh % 1000,
						multiplier, target_hz);
			}
			wlr_output_state_finish(&state);
		}
	}

	/* Show result */
	if (success) {
		int actual_mhz = (int)(actual_hz * 1000.0f + 0.5f);
		if (multiplier > 1) {
			snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz (%dx%.0f)",
					actual_mhz / 1000, actual_mhz % 1000, multiplier, target_hz);
		} else {
			snprintf(osd_msg, sizeof(osd_msg), "%d.%03d Hz",
					actual_mhz / 1000, actual_mhz % 1000);
		}
	} else {
		wlr_log(WLR_ERROR, "Failed to set video Hz for %.3f fps (all methods failed)", target_hz);

		/* Restore original mode */
		if (m->original_mode) {
			wlr_output_state_init(&state);
			wlr_output_state_set_mode(&state, m->original_mode);
			wlr_output_commit_state(m->wlr_output, &state);
			wlr_output_state_finish(&state);
		}

		snprintf(osd_msg, sizeof(osd_msg), "FAILED: %.0f fps", target_hz);
	}

	show_hz_osd(m, osd_msg);
}

void
check_fullscreen_video(void)
{
	Client *c;
	Monitor *m;
	int any_fullscreen_active = 0;

	wl_list_for_each(m, &mons, link) {
		c = focustop(m);
		if (!c || !c->isfullscreen)
			continue;

		any_fullscreen_active = 1;

		/* Check content-type hint from client */
		int is_video = is_video_content(c);

		/* Detect exact video Hz from frame timing (use 8 samples for speed) */
		float hz = 0.0f;
		if (c->frame_time_count >= 8)
			hz = detect_video_framerate(c);

		wlr_log(WLR_DEBUG, "check_fullscreen_video: monitor=%s is_video=%d "
				"frame_count=%d hz=%.3f detected_hz=%.3f phase=%d retries=%d "
				"video_mode_active=%d vrr_active=%d",
				m->wlr_output->name, is_video, c->frame_time_count, hz,
				c->detected_video_hz, c->video_detect_phase, c->video_detect_retries,
				m->video_mode_active, m->vrr_active);

		/* Already successfully detected */
		if (c->video_detect_phase == 2 && (m->vrr_active || c->detected_video_hz > 0.0f)) {
			/* Re-establish cadence if it was cleared (tag switch
			 * calls invalidate_video_pacing which zeroes cadence
			 * and VRR state).  Without this, switching away and
			 * back to a video tag loses frame pacing forever
			 * because phase=2 skips re-detection. */
			if (!m->vrr_active && !m->video_cadence_active &&
			    c->detected_video_hz > 0.0f) {
				float display_hz = 0.0f;
				if (m->present_interval_ns > 0)
					display_hz = 1000000000.0f / (float)m->present_interval_ns;
				else if (m->wlr_output->current_mode)
					display_hz = m->wlr_output->current_mode->refresh / 1000.0f;
				if (display_hz > 0.0f) {
					float ratio = display_hz / c->detected_video_hz;
					if (ratio >= 1.5f) {
						m->estimated_game_fps = c->detected_video_hz;
						m->frame_pacing_active = 1;
						m->video_cadence_base = (int)ratio;
						if (m->video_cadence_base < 1)
							m->video_cadence_base = 1;
						m->video_cadence_frac = ratio - (float)m->video_cadence_base;
						m->video_cadence_accum = 0.0f;
						m->video_cadence_current_n = m->video_cadence_base;
						m->video_cadence_counter = 0;
						m->video_cadence_active = 1;
						wlr_log(WLR_DEBUG,
							"Video cadence re-established: %.0f fps base=%d frac=%.3f @ %.0f Hz",
							c->detected_video_hz, m->video_cadence_base,
							m->video_cadence_frac, display_hz);
					}
				}
			}
			continue;
		}

		/* Phase 0: Scanning - collecting frame samples (silent, no OSD) */
		if (c->video_detect_phase == 0) {
			/* Use fewer samples for faster detection (8 instead of 16) */
			if (c->frame_time_count < 8)
				continue;
			/* Enough samples collected, move to analysis */
			c->video_detect_phase = 1;
		}

		/* Phase 1: Analysis - try to detect video framerate */
		if (c->video_detect_phase == 1) {
			float use_hz = 0.0f;

			if (hz > 0.0f) {
				use_hz = hz;
			} else if (!is_video) {
				/* Try to estimate from raw measurements */
				double avg_interval = 0;
				int valid = 0;
				for (int i = 1; i < c->frame_time_count; i++) {
					int prev_idx = (c->frame_time_idx - c->frame_time_count + i - 1 + 32) % 32;
					int curr_idx = (c->frame_time_idx - c->frame_time_count + i + 32) % 32;
					uint64_t interval = c->frame_times[curr_idx] - c->frame_times[prev_idx];
					if (interval >= 5 && interval <= 200) {
						avg_interval += interval;
						valid++;
					}
				}

				if (valid > 0) {
					avg_interval /= valid;
					double raw_hz = 1000.0 / avg_interval;

					/* If in video range (20-120 Hz), use rounded standard framerate */
					if (raw_hz >= 20.0 && raw_hz <= 120.0) {
						if (raw_hz < 24.5)
							use_hz = (raw_hz < 24.0) ? 23.976f : 24.0f;
						else if (raw_hz < 27.5)
							use_hz = 25.0f;
						else if (raw_hz < 35.0)
							use_hz = (raw_hz < 30.0) ? 29.97f : 30.0f;
						else if (raw_hz < 55.0)
							use_hz = 50.0f;
						else if (raw_hz < 65.0)
							use_hz = (raw_hz < 60.0) ? 59.94f : 60.0f;
						else
							use_hz = (float)raw_hz;

						wlr_log(WLR_DEBUG, "Estimated framerate %.3f Hz (raw ~%.1f Hz) on %s",
								use_hz, raw_hz, m->wlr_output->name);
					} else {
						wlr_log(WLR_DEBUG, "Measured Hz (~%.1f) outside video range", raw_hz);
					}
				}
			}

			if (use_hz > 0.0f) {
				c->detected_video_hz = use_hz;
				c->video_detect_phase = 2;
				c->video_detect_retries = 0;

				/* Try VRR first - best possible solution */
				if (m->vrr_capable && enable_vrr_video_mode(m, use_hz)) {
					wlr_log(WLR_DEBUG, "Video %.3f Hz on %s: using VRR",
							use_hz, m->wlr_output->name);
				} else {
					/*
					 * Bresenham cadence for compositor-side pacing.
					 * Instead of delaying frame_done (which couples
					 * compositor and player timing), we let the player
					 * run freely and use a cadence clock in rendermon()
					 * to decide when to present vs hold frames.
					 *
					 * Integer ratios (24@144=6x): fixed hold count.
					 * Non-integer (24@60=2.5x): alternate 2 and 3.
					 */
					m->estimated_game_fps = use_hz;
					m->frame_pacing_active = 1;

					float display_hz = 0.0f;
					if (m->present_interval_ns > 0)
						display_hz = 1000000000.0f / (float)m->present_interval_ns;
					else if (m->wlr_output->current_mode)
						display_hz = m->wlr_output->current_mode->refresh / 1000.0f;

					if (display_hz > 0.0f && use_hz > 0.0f) {
						float ratio = display_hz / use_hz;

						if (ratio >= 1.5f) {
							m->video_cadence_base = (int)ratio;
							if (m->video_cadence_base < 1)
								m->video_cadence_base = 1;
							m->video_cadence_frac = ratio - (float)m->video_cadence_base;
							m->video_cadence_accum = 0.0f;
							m->video_cadence_current_n = m->video_cadence_base;
							m->video_cadence_counter = 0;
							m->video_cadence_active = 1;
						}
					}

					wlr_log(WLR_DEBUG, "Video %.3f Hz on %s: cadence base=%d frac=%.3f @ %.0f Hz",
							use_hz, m->wlr_output->name,
							m->video_cadence_base, m->video_cadence_frac, display_hz);
				}
				continue;
			}

			/* No video detected - silent retry (1 retry only for speed) */
			c->video_detect_retries++;

			if (c->video_detect_retries < 2) {
				/* Clear frame data for fresh scan - no OSD */
				c->frame_time_idx = 0;
				c->frame_time_count = 0;
				c->last_buffer = NULL;
				c->video_detect_phase = 0;
			} else {
				/* Done - no video content, stay silent */
				c->video_detect_phase = 2;
			}
		}
	}

	/* Continue checking only while detection is still in progress.
	 * Once all fullscreen clients reach phase 2 (detected or gave up),
	 * stop the timer — pacing is set up once and not re-adjusted. */
	if (any_fullscreen_active) {
		int all_stable = 1;
		wl_list_for_each(m, &mons, link) {
			c = focustop(m);
			if (c && c->isfullscreen && c->video_detect_phase < 2) {
				all_stable = 0;
				break;
			}
		}
		if (!all_stable)
			schedule_video_check(200);
	}
}

void
updatemons(struct wl_listener *listener, void *data)
{
	/*
	 * Called whenever the output layout changes: adding or removing a
	 * monitor, changing an output's mode or position, etc. This is where
	 * the change officially happens and we update geometry, window
	 * positions, focus, and the stored configuration in wlroots'
	 * output-manager implementation.
	 */
	struct wlr_output_configuration_v1 *config
			= wlr_output_configuration_v1_create();
	Client *c;
	struct wlr_output_configuration_head_v1 *config_head;
	Monitor *m;

	/* First remove from the layout the disabled monitors */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled || m->asleep)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
		config_head->state.enabled = 0;
		/* Remove this output from the layout to avoid cursor enter inside it */
		wlr_output_layout_remove(output_layout, m->wlr_output);
		closemon(m);
		m->m = m->w = (struct wlr_box){0};
	}
	/* Insert outputs that need to */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled
				&& !wlr_output_layout_get(output_layout, m->wlr_output))
			wlr_output_layout_add_auto(output_layout, m->wlr_output);
	}

	/*
	 * Restore clients to their original monitor after suspend/resume.
	 * When a monitor is disabled (e.g., laptop suspend), closemon() moves
	 * all clients to another monitor. But c->output still remembers which
	 * monitor the client originally belonged to. When the monitor wakes up,
	 * we need to move clients back to their original monitor.
	 */
	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled || m->is_mirror)
			continue;
		wl_list_for_each(c, &clients, link) {
			if (c->output && c->mon != m
					&& strcmp(m->wlr_output->name, c->output) == 0) {
				wlr_log(WLR_INFO,
					"GAME_TRACE: updatemons direct c->mon reassign "
					"appid='%s' old='%s' → new='%s' isfullscreen=%d "
					"geom=%dx%d@%d,%d c->output='%s'",
					client_get_appid(c) ? client_get_appid(c) : "(null)",
					c->mon && c->mon->wlr_output ? c->mon->wlr_output->name : "(null)",
					m->wlr_output->name,
					c->isfullscreen,
					c->geom.width, c->geom.height, c->geom.x, c->geom.y,
					c->output);
				c->mon = m;
			}
		}
	}

	/* Now that we update the output layout we can get its box */
	wlr_output_layout_get_box(output_layout, NULL, &sgeom);

	wlr_scene_node_set_position(&root_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(root_bg, sgeom.width, sgeom.height);

	/* Make sure the clients are hidden when dwl is locked */
	wlr_scene_node_set_position(&locked_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(locked_bg, sgeom.width, sgeom.height);

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

		/* Get the effective monitor geometry to use for surfaces */
		wlr_output_layout_get_box(output_layout, m->wlr_output, &m->m);
		m->w = m->m;
		wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

		wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
		wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

		if (m->lock_surface) {
			struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
			wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
			wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width, m->m.height);
		}

		/* Calculate the effective monitor geometry to use for clients */
		arrangelayers(m);
		/* Don't move clients to the left output when plugging monitors */
		arrange(m);
		/*
		 * Make sure fullscreen clients have the right size.
		 *
		 * We iterate over ALL fullscreen clients on this monitor, not just
		 * focustop(m), because the direct c->mon reassignment higher up in
		 * this function (based on c->output match) can move a fullscreen
		 * client onto m without it being the top of the focus stack. If we
		 * only resized focustop, the moved client would keep its old
		 * geometry — causing arrange() to draw fullscreen_bg around a
		 * smaller rectangle on the new monitor. This was the "game shown
		 * in a small box with black around it after the game moved to the
		 * correct screen" symptom observed when Proton/Vulkan games enable
		 * VRR shortly after map.
		 */
		wl_list_for_each(c, &clients, link) {
			if (c->mon == m && c->isfullscreen) {
				struct wlr_box fsgeom = fullscreen_mirror_geom(m);
				if (c->geom.width != fsgeom.width ||
				    c->geom.height != fsgeom.height ||
				    c->geom.x != fsgeom.x ||
				    c->geom.y != fsgeom.y) {
					wlr_log(WLR_INFO,
						"GAME_TRACE: updatemons fullscreen resize "
						"appid='%s' mon='%s' %dx%d@%d,%d → %dx%d@%d,%d",
						client_get_appid(c) ? client_get_appid(c) : "(null)",
						m->wlr_output->name,
						c->geom.width, c->geom.height, c->geom.x, c->geom.y,
						fsgeom.width, fsgeom.height, fsgeom.x, fsgeom.y);
					resize(c, fsgeom, 0);
				}
			}
		}

		/* Try to re-set the gamma LUT when updating monitors,
		 * it's only really needed when enabling a disabled output, but meh. */
		m->gamma_lut_changed = 1;

		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;

		if (!selmon && !m->is_mirror) {
			selmon = m;
		}
	}

	if (selmon && selmon->wlr_output->enabled) {
		wl_list_for_each(c, &clients, link) {
			if (!c->mon && client_surface(c)->mapped)
				setmon(c, selmon, c->tags);
		}
		focusclient(focustop(selmon), 1);
		if (selmon->lock_surface) {
			client_notify_enter(selmon->lock_surface->surface,
					wlr_seat_get_keyboard(seat));
			client_activate_surface(selmon->lock_surface->surface, 1);
		}
	}

	/* FIXME: figure out why the cursor image is at 0,0 after turning all
	 * the monitors on.
	 * Move the cursor image where it used to be. It does not generate a
	 * wl_pointer.motion event for the clients, it's only the image what it's
	 * at the wrong position after all. */
	wlr_cursor_move(cursor, NULL, 0, 0);

	/* Enter HTPC mode once selmon is available (deferred from startup) */
	if (nixlytile_mode == 2 && !htpc_mode_active && selmon)
		htpc_mode_enter();

	wlr_output_manager_v1_set_configuration(output_mgr, config);
}

Monitor *
xytomon(double x, double y)
{
	struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
	return o ? o->data : NULL;
}

void
xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny)
{
	struct wlr_scene_node *node, *pnode;
	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	LayerSurface *l = NULL;
	int layer;

	for (layer = NUM_LAYERS - 1; !surface && layer >= 0; layer--) {
		if (!(node = wlr_scene_node_at(&layers[layer]->node, x, y, nx, ny)))
			continue;

		if (node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_surface *scene_surface =
				wlr_scene_surface_try_from_buffer(
						wlr_scene_buffer_from_node(node));
			if (scene_surface)
				surface = scene_surface->surface;
		}
		/* Walk the tree to find a node that knows the client */
		for (pnode = node; pnode && !c; pnode = &pnode->parent->node)
			c = pnode->data;
		if (c && c->type == LayerShell) {
			c = NULL;
			l = pnode->data;
		}
	}

	if (psurface) *psurface = surface;
	if (pc) *pc = c;
	if (pl) *pl = l;
}

