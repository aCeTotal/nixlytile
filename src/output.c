/* output.c - Auto-extracted from nixlytile.c */
#include "nixlytile.h"
#include "client.h"

void
cleanupmon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy);
	LayerSurface *l, *tmp;
	size_t i;

	modal_file_search_stop(m);

	/* m->layers[i] are intentionally not unlinked */
	for (i = 0; i < LENGTH(m->layers); i++) {
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->layer_surface);
	}

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
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
	destroy_tree(m);
	closemon(m);
	wlr_scene_node_destroy(&m->fullscreen_bg->node);
	free(m);
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
		do /* don't switch to disabled mons */
			selmon = wl_container_of(mons.next, selmon, link);
		while (!selmon->wlr_output->enabled && i++ < nmons);

		if (!selmon->wlr_output->enabled)
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

	if (!wlr_output_init_render(wlr_output, alloc, drw))
		return;

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
		m->lt[1] = &layouts[LENGTH(layouts) > 1 ? 1 : 0];
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
		for (r = monrules; r < END(monrules); r++) {
			if (!r->name || strstr(wlr_output->name, r->name)) {
				m->m.x = r->x;
				m->m.y = r->y;
				m->mfact = r->mfact;
				m->nmaster = r->nmaster;
				m->lt[0] = r->lt;
				m->lt[1] = &layouts[LENGTH(layouts) > 1 && r->lt != &layouts[1]];
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

	wlr_output_state_set_enabled(&state, 1);
	wlr_output_commit_state(wlr_output, &state);
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
			wlr_log(WLR_INFO, "Monitor %s supports VRR/Adaptive Sync", wlr_output->name);
		}
		wlr_output_state_finish(&vrr_test);
	}

	wl_list_insert(&mons, &m->link);
	printstatus();
	init_tree(m);
	modal_prewarm(m);

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

	if (m->m.x == -1 && m->m.y == -1)
		wlr_output_layout_add_auto(output_layout, wlr_output);
	else
		wlr_output_layout_add(output_layout, wlr_output, m->m.x, m->m.y);

	wl_list_for_each(c, &clients, link) {
		if (c->output && strcmp(wlr_output->name, c->output) == 0)
			c->mon = m;
	}

	/* Initialize HDR and 10-bit color settings */
	init_monitor_color_settings(m);

	updatemons(NULL, NULL);
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

void
rendermon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, frame);
	struct wlr_scene_output_state_options opts = {0};
	struct wlr_output_state state;
	struct timespec now;
	uint64_t frame_start_ns, commit_end_ns;
	int needs_frame = 0;
	Client *fullscreen_client = NULL;
	int allow_tearing = 0;
	int is_video = 0;
	int is_game = 0;
	int is_direct_scanout = 0;
	int use_frame_pacing = 0;

	frame_start_ns = get_time_ns();

	/* Update integrated video player frame if playing
	 * Use last vsync time if available for precise frame pacing,
	 * otherwise fall back to current time */
	if (active_videoplayer && active_videoplayer->state == VP_STATE_PLAYING) {
		uint64_t vsync_time = (m->last_present_ns > 0) ? m->last_present_ns : frame_start_ns;
		videoplayer_present_frame(active_videoplayer, vsync_time);
	}

	/*
	 * Check for fullscreen client and determine content type for optimal handling.
	 */
	fullscreen_client = focustop(m);
	if (fullscreen_client && fullscreen_client->isfullscreen) {
		is_game = client_wants_tearing(fullscreen_client) || is_game_content(fullscreen_client);
		is_video = is_video_content(fullscreen_client) || fullscreen_client->detected_video_hz > 0.0f;

		/* Games get tearing for lowest latency, unless it's video content */
		if (is_game && !is_video) {
			allow_tearing = 1;
		}
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
	wlr_output_state_init(&state);
	needs_frame = wlr_scene_output_build_state(m->scene_output, &state, &opts);

	/*
	 * Detect direct scanout: if the output buffer is a client buffer
	 * (not from the compositor's swapchain), direct scanout is active.
	 * wlr_client_buffer_get() returns non-NULL only for client buffers.
	 */
	if (needs_frame && state.buffer && wlr_client_buffer_get(state.buffer)) {
		is_direct_scanout = 1;
	}

	/* Track scanout state transitions and show notification */
	if (is_direct_scanout && !m->direct_scanout_active) {
		m->direct_scanout_active = 1;
		m->frame_pacing_active = 1; /* Enable frame pacing for direct scanout */
		if (!m->direct_scanout_notified) {
			show_hz_osd(m, "Direct Scanout");
			m->direct_scanout_notified = 1;
		}
		wlr_log(WLR_INFO, "Direct scanout activated on %s - frame pacing enabled",
			m->wlr_output->name);
	} else if (!is_direct_scanout && m->direct_scanout_active) {
		m->direct_scanout_active = 0;
		m->frame_pacing_active = 0;
		m->direct_scanout_notified = 0; /* Reset so next scanout shows OSD again */
		/* Reset frame pacing statistics on exit */
		m->pending_game_frame = 0;
		m->estimated_game_fps = 0.0f;
		wlr_log(WLR_INFO, "Direct scanout deactivated on %s - stats: %lu presented, %lu dropped, %lu held",
			m->wlr_output->name, m->frames_presented, m->frames_dropped, m->frames_held);
	}

	/*
	 * Frame pacing logic for games in direct scanout mode.
	 *
	 * When we have direct scanout with a game, we can optimize frame timing:
	 * 1. Track the game's frame submission rate
	 * 2. Hold frames that arrive too early (would cause judder)
	 * 3. Release frames at optimal vblank timing
	 *
	 * This is similar to Gamescope's frame pacing but integrated into
	 * the compositor for seamless operation.
	 */
	if (is_direct_scanout && is_game && !allow_tearing && m->frame_pacing_active) {
		use_frame_pacing = 1;

		/* Track game's frame submission interval */
		if (m->game_frame_submit_ns > 0 && frame_start_ns > m->game_frame_submit_ns) {
			uint64_t game_interval = frame_start_ns - m->game_frame_submit_ns;

			/* Only track reasonable intervals (8ms - 100ms = ~10-120fps) */
			if (game_interval > 8000000 && game_interval < 100000000) {
				m->game_frame_intervals[m->game_frame_interval_idx] = game_interval;
				m->game_frame_interval_idx = (m->game_frame_interval_idx + 1) % 8;
				if (m->game_frame_interval_count < 8)
					m->game_frame_interval_count++;

				/* Calculate estimated game FPS from rolling average */
				if (m->game_frame_interval_count >= 4) {
					uint64_t avg_interval = 0;
					uint64_t variance_sum = 0;
					for (int i = 0; i < m->game_frame_interval_count; i++)
						avg_interval += m->game_frame_intervals[i];
					avg_interval /= m->game_frame_interval_count;
					m->estimated_game_fps = 1000000000.0f / (float)avg_interval;

					/*
					 * Calculate frame time variance (jitter).
					 * Low variance = consistent frame times = smooth gameplay.
					 * High variance = inconsistent = potential stutter.
					 */
					for (int i = 0; i < m->game_frame_interval_count; i++) {
						int64_t diff = (int64_t)m->game_frame_intervals[i] - (int64_t)avg_interval;
						variance_sum += (uint64_t)(diff * diff);
					}
					m->frame_variance_ns = variance_sum / m->game_frame_interval_count;

					/*
					 * PREDICTIVE FRAME TIMING
					 *
					 * Predict when the next frame will arrive based on:
					 * 1. Average frame interval
					 * 2. Recent trend (acceleration/deceleration)
					 * 3. Variance (add margin for high-jitter games)
					 *
					 * This allows the compositor to prepare for the frame
					 * and commit at optimal vblank timing.
					 */
					m->predicted_next_frame_ns = frame_start_ns + avg_interval;

					/* Add margin based on variance (higher jitter = more margin) */
					if (m->frame_variance_ns > 0) {
						/* sqrt approximation for margin: variance^0.5 */
						uint64_t margin = 0;
						uint64_t v = m->frame_variance_ns;
						while (v > margin * margin)
							margin += 100000; /* 0.1ms steps */
						m->predicted_next_frame_ns += margin;
					}

					/* Track prediction accuracy */
					if (m->predicted_next_frame_ns > 0) {
						int64_t prediction_error = (int64_t)frame_start_ns - (int64_t)(m->predicted_next_frame_ns - avg_interval);
						if (prediction_error < 0) prediction_error = -prediction_error;

						/* Frame is "on time" if within 2ms of prediction */
						if (prediction_error < 2000000) {
							/* Good prediction */
							m->prediction_accuracy = m->prediction_accuracy * 0.9f + 10.0f;
						} else if (prediction_error < (int64_t)avg_interval / 4) {
							/* Acceptable prediction */
							m->prediction_accuracy = m->prediction_accuracy * 0.9f + 5.0f;
							if (frame_start_ns < m->predicted_next_frame_ns - avg_interval)
								m->frames_early++;
							else
								m->frames_late++;
						} else {
							/* Poor prediction */
							m->prediction_accuracy = m->prediction_accuracy * 0.9f;
							m->frames_late++;
						}
						if (m->prediction_accuracy > 100.0f) m->prediction_accuracy = 100.0f;
					}

					/*
					 * Update dynamic game VRR.
					 * This adjusts the display to match the game's framerate,
					 * eliminating judder when games run below native refresh.
					 */
					if (m->game_vrr_active) {
						update_game_vrr(m, m->estimated_game_fps);
					}
				}
			}
		}
		m->game_frame_submit_ns = frame_start_ns;
		m->pending_game_frame = 1;

		/*
		 * Frame hold/release decision:
		 *
		 * If the game is running slower than display refresh, present immediately.
		 * If the game is running faster, we may want to hold frames to prevent
		 * judder from uneven frame presentation.
		 *
		 * Key insight: With direct scanout, the display directly shows the
		 * game's buffer. We control WHEN it flips via the commit timing.
		 */
		if (m->present_interval_ns > 0 && m->target_present_ns > 0) {
			uint64_t time_to_target = 0;
			if (frame_start_ns < m->target_present_ns)
				time_to_target = m->target_present_ns - frame_start_ns;

			/*
			 * If we're more than 2ms early, the frame arrived too soon.
			 * In a perfect world we'd hold it, but that requires async
			 * frame scheduling which is complex. For now, just track it.
			 */
			if (time_to_target > 2000000) {
				m->frames_held++;
				/*
				 * Note: True frame holding would require:
				 * 1. Storing the buffer
				 * 2. Setting up a timer for optimal commit time
				 * 3. Committing at the right moment
				 *
				 * wlroots doesn't easily support this, so we commit
				 * immediately but track that we could have held it.
				 */
			}
		}
	}

	/*
	 * ============ FRAME DOUBLING/TRIPLING FOR SMOOTH LOW-FPS PLAYBACK ============
	 *
	 * When a game runs at a framerate that doesn't divide evenly into the
	 * display refresh rate, you get "judder" - uneven frame presentation times.
	 *
	 * Example: 30 FPS game on 60 Hz display
	 *   Without frame doubling: frame times are 16.6ms, 16.6ms (uneven feel)
	 *   With frame doubling: each game frame shown 2x = perfect 33.3ms cadence
	 *
	 * Example: 24 FPS video on 120 Hz display
	 *   Without: judder from 5:5:5:5:5 pattern not matching 24fps
	 *   With frame 5x repeat: each frame shown 5 times = perfect 41.6ms cadence
	 *
	 * This technique is used by:
	 * - Gamescope (Steam Deck compositor)
	 * - High-end TVs (motion interpolation alternative)
	 * - Professional video players
	 *
	 * We ONLY use this when VRR is NOT available or disabled, as VRR solves
	 * this problem by adjusting the display refresh to match content.
	 */
	if (is_game && m->frame_pacing_active && !m->game_vrr_active && !allow_tearing) {
		float display_hz = 0.0f;
		float game_fps = m->estimated_game_fps;
		int optimal_repeat = 1;

		/* Get current display refresh rate */
		if (m->present_interval_ns > 0) {
			display_hz = 1000000000.0f / (float)m->present_interval_ns;
		} else if (m->wlr_output->current_mode) {
			display_hz = (float)m->wlr_output->current_mode->refresh / 1000.0f;
		}

		/*
		 * Calculate optimal frame repeat count.
		 * We want to find integer N where: display_hz / N ≈ game_fps
		 *
		 * This gives perfectly even frame timing without interpolation.
		 */
		if (game_fps > 5.0f && display_hz > 30.0f) {
			float ratio = display_hz / game_fps;

			/*
			 * Find the best integer repeat count (1-6x).
			 * We check which integer gives the smallest error.
			 */
			float min_error = 9999.0f;
			for (int n = 1; n <= 6; n++) {
				float effective_fps = display_hz / (float)n;
				float error = fabsf(effective_fps - game_fps);
				/* Also check if game fps is close to a fraction */
				float frac_error = fabsf(ratio - (float)n);

				if (frac_error < 0.15f && error < min_error) {
					min_error = error;
					optimal_repeat = n;
				}
			}

			/*
			 * Common patterns we optimize for:
			 *
			 * 60Hz display:
			 *   30 FPS → 2x repeat (60/2=30) ✓
			 *   20 FPS → 3x repeat (60/3=20) ✓
			 *   15 FPS → 4x repeat (60/4=15) ✓
			 *
			 * 120Hz display:
			 *   60 FPS → 2x repeat (120/2=60) ✓
			 *   40 FPS → 3x repeat (120/3=40) ✓
			 *   30 FPS → 4x repeat (120/4=30) ✓
			 *   24 FPS → 5x repeat (120/5=24) ✓ (perfect for film content)
			 *
			 * 144Hz display:
			 *   72 FPS → 2x repeat (144/2=72) ✓
			 *   48 FPS → 3x repeat (144/3=48) ✓
			 *   36 FPS → 4x repeat (144/4=36) ✓
			 */

			/* Only enable repeat if we have a good match (within 10%) */
			if (optimal_repeat > 1) {
				float target_fps = display_hz / (float)optimal_repeat;
				float match_quality = fabsf(game_fps - target_fps) / target_fps;

				if (match_quality > 0.10f) {
					/* Not a good match, disable repeat */
					optimal_repeat = 1;
				}
			}
		}

		/* Update frame repeat state */
		if (optimal_repeat != m->frame_repeat_count) {
			if (optimal_repeat > 1 && !m->frame_repeat_enabled) {
				m->frame_repeat_enabled = 1;
				m->frame_repeat_interval_ns = m->present_interval_ns;
				wlr_log(WLR_INFO, "Frame repeat enabled: %dx (%.1f FPS → %.1f Hz display)",
					optimal_repeat, game_fps, display_hz);
			} else if (optimal_repeat == 1 && m->frame_repeat_enabled) {
				m->frame_repeat_enabled = 0;
				wlr_log(WLR_INFO, "Frame repeat disabled - game FPS matches display");
			}
			m->frame_repeat_count = optimal_repeat;
			m->frame_repeat_current = 0;
		}

		/*
		 * Calculate judder score (0-100, lower is better).
		 * This measures how well game fps matches display refresh.
		 */
		if (game_fps > 0.0f && display_hz > 0.0f) {
			float ideal_interval_ms = 1000.0f / game_fps;
			float actual_interval_ms = 1000.0f / display_hz * (float)optimal_repeat;
			float deviation_pct = fabsf(ideal_interval_ms - actual_interval_ms) / ideal_interval_ms * 100.0f;
			m->judder_score = (int)(deviation_pct * 10.0f); /* Scale to 0-100ish */
			if (m->judder_score > 100) m->judder_score = 100;
		}

		/* Enable adaptive pacing for smoother non-VRR playback */
		m->adaptive_pacing_enabled = 1;
		m->target_frame_time_ms = 1000.0f / game_fps;

	} else {
		/* Reset frame repeat state when not applicable */
		if (m->frame_repeat_enabled) {
			m->frame_repeat_enabled = 0;
			m->frame_repeat_count = 1;
			m->frame_repeat_current = 0;
			m->adaptive_pacing_enabled = 0;
			wlr_log(WLR_INFO, "Frame repeat disabled - VRR active or tearing enabled");
		}
	}

	if (needs_frame) {
		m->frames_since_content_change = 0;

		/*
		 * Apply tearing for games that want it - async page flips bypass vsync.
		 * This gives the lowest possible input latency at the cost of
		 * potential screen tearing (which some gamers prefer).
		 *
		 * Note: When frame pacing is active and tearing is NOT requested,
		 * we rely on vsync'd commits for smooth presentation.
		 */
		if (allow_tearing && wlr_output_is_drm(m->wlr_output)) {
			state.tearing_page_flip = true;
		}

		/* Commit the frame */
		if (!wlr_output_commit_state(m->wlr_output, &state)) {
			/* Fallback: retry without tearing if it failed */
			if (allow_tearing) {
				state.tearing_page_flip = false;
				wlr_output_commit_state(m->wlr_output, &state);
			}
		}

		/* Track commit timing for frame pacing analysis */
		commit_end_ns = get_time_ns();
		m->last_commit_duration_ns = commit_end_ns - frame_start_ns;

		/*
		 * INPUT LATENCY TRACKING
		 *
		 * Measure time from last input event to frame commit.
		 * This is a key component of "input-to-photon" latency:
		 *   Total latency = input processing + game logic + render + commit + scanout
		 *
		 * We measure: input event → frame commit (what compositor controls)
		 * The remaining latency (scanout) depends on display.
		 */
		if (game_mode_ultra && m->last_input_ns > 0) {
			uint64_t input_latency = commit_end_ns - m->last_input_ns;

			/* Only track reasonable latencies (< 500ms) */
			if (input_latency < 500000000ULL) {
				m->input_to_frame_ns = input_latency;

				/* Track min/max for statistics */
				if (m->min_input_latency_ns == 0 || input_latency < m->min_input_latency_ns)
					m->min_input_latency_ns = input_latency;
				if (input_latency > m->max_input_latency_ns)
					m->max_input_latency_ns = input_latency;
			}
		}

		/*
		 * Update rolling average of commit time (Gamescope-style).
		 * Use 98% decay rate - this means spikes are remembered but
		 * gradually fade, while sustained high times are tracked accurately.
		 */
		if (m->rolling_commit_time_ns == 0) {
			m->rolling_commit_time_ns = m->last_commit_duration_ns;
		} else {
			/* Rolling average: 98% old + 2% new */
			m->rolling_commit_time_ns =
				(m->rolling_commit_time_ns * 98 + m->last_commit_duration_ns * 2) / 100;

			/* But always track upward spikes immediately */
			if (m->last_commit_duration_ns > m->rolling_commit_time_ns) {
				m->rolling_commit_time_ns = m->last_commit_duration_ns;
			}
		}
	} else {
		m->frames_since_content_change++;

		/*
		 * If frame pacing is active but no new frame, track potential drop.
		 * This happens when the game misses a vblank deadline.
		 */
		if (use_frame_pacing && m->pending_game_frame) {
			/* Game didn't submit a new frame - previous one will be shown again */
			m->frames_dropped++;
			m->pending_game_frame = 0;
		}
	}

	wlr_output_state_finish(&state);
	m->last_frame_ns = frame_start_ns;

	/*
	 * FPS Limiter - controls when we send frame_done to clients.
	 *
	 * By delaying frame_done, we effectively limit how fast games can
	 * render. The game waits for frame_done before starting the next
	 * frame, so controlling this signal controls the framerate.
	 */
	if (fps_limit_enabled && fps_limit_value > 0 && is_game) {
		uint64_t target_interval_ns = 1000000000ULL / (uint64_t)fps_limit_value;
		uint64_t now_ns = get_time_ns();
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
			/* Don't send frame_done yet - limiter is active */
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
			 * The game will wait, and we'll show the same frame again
			 * on the next vblank. This is frame "doubling" or "tripling".
			 */
			m->frames_repeated++;

			/*
			 * We still need to acknowledge this vblank for timing,
			 * but we DON'T tell the game to render a new frame.
			 * The display will simply show the previous frame again.
			 *
			 * This creates perfect frame cadence:
			 * - 2x repeat: frame shown at vblank 0 and 1, new frame at 2
			 * - 3x repeat: frame shown at vblank 0, 1, 2, new frame at 3
			 * - etc.
			 */
			return;
		}

		/* Time for new frame - reset counter and continue to frame_done */
		m->frame_repeat_current = 0;
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
	clock_gettime(CLOCK_MONOTONIC, &now);
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

	/* Calculate interval between presents (vblank interval) */
	if (m->last_present_ns > 0 && present_ns > m->last_present_ns) {
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
	 * We want to present at the next vblank, minus a small margin
	 * for compositor overhead (1ms).
	 */
	if (m->present_interval_ns > 0) {
		m->target_present_ns = present_ns + m->present_interval_ns - 1000000;
	}
}

void
requestmonstate(struct wl_listener *listener, void *data)
{
	struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(event->output, event->state);
	updatemons(NULL, NULL);
}

void
set_adaptive_sync(Monitor *m, int enable)
{
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;

	if (!m || !m->wlr_output || !m->wlr_output->enabled
			|| !fullscreen_adaptive_sync_enabled)
		return;

	config = wlr_output_configuration_v1_create();
	config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

	/* Set and commit the adaptive sync state change */
	wlr_output_state_init(&state);
	wlr_output_state_set_adaptive_sync_enabled(&state, enable);
	wlr_output_commit_state(m->wlr_output, &state);
	wlr_output_state_finish(&state);

	/* Broadcast the adaptive sync state change to output_mgr */
	config_head->state.adaptive_sync_enabled = enable;
	wlr_output_manager_v1_set_configuration(output_mgr, config);
}

void
enable_game_vrr(Monitor *m)
{
	struct wlr_output_state state;
	char osd_msg[64];

	if (!m || !m->vrr_capable || m->game_vrr_active)
		return;

	if (!fullscreen_adaptive_sync_enabled) {
		wlr_log(WLR_INFO, "Game VRR: disabled by user setting");
		return;
	}

	wlr_output_state_init(&state);
	wlr_output_state_set_adaptive_sync_enabled(&state, 1);

	if (wlr_output_commit_state(m->wlr_output, &state)) {
		m->game_vrr_active = 1;
		m->game_vrr_target_fps = 0.0f;
		m->game_vrr_last_fps = 0.0f;
		m->game_vrr_last_change_ns = get_time_ns();
		m->game_vrr_stable_frames = 0;

		snprintf(osd_msg, sizeof(osd_msg), "Game VRR Enabled");
		show_hz_osd(m, osd_msg);
		wlr_log(WLR_INFO, "Game VRR enabled on %s", m->wlr_output->name);
	}

	wlr_output_state_finish(&state);
}

void
disable_game_vrr(Monitor *m)
{
	struct wlr_output_state state;

	if (!m || !m->game_vrr_active)
		return;

	wlr_output_state_init(&state);
	wlr_output_state_set_adaptive_sync_enabled(&state, 0);

	if (wlr_output_commit_state(m->wlr_output, &state)) {
		wlr_log(WLR_INFO, "Game VRR disabled on %s (was targeting %.1f FPS)",
			m->wlr_output->name, m->game_vrr_target_fps);
	}

	wlr_output_state_finish(&state);

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

	/* Sanity check FPS range */
	if (current_fps < GAME_VRR_MIN_FPS || current_fps > GAME_VRR_MAX_FPS)
		return;

	now_ns = get_time_ns();

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
			wlr_log(WLR_INFO, "Game VRR: back to full refresh %.1f Hz", display_max_hz);
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
	wlr_log(WLR_INFO, "Game VRR: adjusted to %.1f FPS on %s",
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

	/* Don't switch if already in video mode */
	if (m->video_mode_active)
		return;

	/* Check if content type is video */
	if (!is_video_content(c))
		return;

	/* Reset frame tracking to detect actual video Hz */
	c->frame_time_idx = 0;
	c->frame_time_count = 0;
	c->detected_video_hz = 0.0f;

	wlr_log(WLR_INFO, "Video content detected on %s, starting frame rate detection",
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

	wlr_log(WLR_INFO, "Restoring %s to max mode: %dx%d@%dmHz",
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

			/* Also set max bpc to 10 via DRM */
			set_drm_color_properties(m, 10);

			/* Show notification */
			toast_show(m, "10-bit color", 1500);
		} else {
			wlr_log(WLR_ERROR, "Failed to commit 10-bit state on %s", m->wlr_output->name);
		}
	} else {
		wlr_log(WLR_DEBUG, "10-bit render format not supported by backend on %s",
			m->wlr_output->name);
	}

	wlr_output_state_finish(&state);
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
	 */
	if (m->supports_10bit) {
		/* Try to enable 10-bit - if it fails, we fall back to 8-bit automatically */
		enable_10bit_rendering(m);
	}

	/*
	 * Always try to set max bpc even if 10-bit rendering isn't available.
	 * This ensures we're using the best color depth the display link supports.
	 */
	if (wlr_output_is_drm(m->wlr_output)) {
		int target_bpc = m->supports_10bit ? 10 : 8;
		set_drm_color_properties(m, target_bpc);
	}
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
		wlr_log(WLR_INFO, "Non-standard framerate detected: %.3f Hz", exact_hz);
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
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;
	char osd_msg[64];

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

	/* Save original mode before first switch */
	if (!m->video_mode_active && !m->vrr_active)
		m->original_mode = m->wlr_output->current_mode;

	wlr_log(WLR_INFO, "Enabling VRR for %.3f Hz video on %s",
			video_hz, m->wlr_output->name);

	/* Enable adaptive sync */
	wlr_output_state_init(&state);
	wlr_output_state_set_adaptive_sync_enabled(&state, 1);

	if (wlr_output_test_state(m->wlr_output, &state)) {
		if (wlr_output_commit_state(m->wlr_output, &state)) {
			m->vrr_active = 1;
			m->vrr_target_hz = video_hz;

			/* Broadcast state change */
			config = wlr_output_configuration_v1_create();
			config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
			config_head->state.adaptive_sync_enabled = 1;
			wlr_output_manager_v1_set_configuration(output_mgr, config);

			wlr_log(WLR_INFO, "VRR enabled for %.3f Hz video - judder-free playback",
					video_hz);

			/* Show OSD */
			snprintf(osd_msg, sizeof(osd_msg), "VRR %.3f Hz", video_hz);
			show_hz_osd(m, osd_msg);

			wlr_output_state_finish(&state);
			return 1;
		}
	}

	wlr_output_state_finish(&state);
	wlr_log(WLR_DEBUG, "Failed to enable VRR on %s", m->wlr_output->name);
	return 0;
}

void
disable_vrr_video_mode(Monitor *m)
{
	struct wlr_output_state state;
	struct wlr_output_configuration_v1 *config;
	struct wlr_output_configuration_head_v1 *config_head;

	if (!m || !m->wlr_output || !m->wlr_output->enabled || !m->vrr_active)
		return;

	wlr_log(WLR_INFO, "Disabling VRR on %s", m->wlr_output->name);

	wlr_output_state_init(&state);
	wlr_output_state_set_adaptive_sync_enabled(&state, 0);

	if (wlr_output_commit_state(m->wlr_output, &state)) {
		m->vrr_active = 0;
		m->vrr_target_hz = 0.0f;

		config = wlr_output_configuration_v1_create();
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
		config_head->state.adaptive_sync_enabled = 0;
		wlr_output_manager_v1_set_configuration(output_mgr, config);
	}

	wlr_output_state_finish(&state);
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

			wlr_log(WLR_INFO, "Applied existing mode %d.%03d Hz for %.3f Hz video",
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

			wlr_log(WLR_INFO, "Applied CVT mode %.3f Hz for %.3f Hz video",
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
		wlr_log(WLR_INFO, "Using VRR for %.3f Hz video - optimal solution", exact_hz);
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

				wlr_log(WLR_INFO, "Using existing mode %d.%03d Hz for %.3f Hz video (%dx)",
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

	wlr_log(WLR_INFO, "Video mode: no VRR, no friendly mode, trying CVT at %dx multiplier",
			multiplier);

	for (; multiplier <= 8 && !success; multiplier++) {
		actual_hz = exact_hz * multiplier;

		/* Skip if above typical maximum */
		if (actual_hz > 300.0f)
			break;

		wlr_log(WLR_INFO, "Video mode: trying %.3f Hz -> %.3f Hz (%dx multiplier)",
				exact_hz, actual_hz, multiplier);

		/* Generate CVT mode with exact timing parameters */
		generate_cvt_mode(&drm_mode, width, height, actual_hz);

		/* Add custom mode to DRM connector */
		new_mode = wlr_drm_connector_add_mode(m->wlr_output, &drm_mode);
		if (!new_mode) {
			wlr_log(WLR_DEBUG, "wlr_drm_connector_add_mode failed for %.3f Hz", actual_hz);
			continue;
		}

		wlr_log(WLR_INFO, "Added custom mode: %dx%d@%d mHz",
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

				wlr_log(WLR_INFO, "Custom mode %.3f Hz applied for %.3f Hz video (%dx)",
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

	/* Create tree if needed */
	if (!playback_osd_tree) {
		playback_osd_tree = wlr_scene_tree_create(layers[LyrOverlay]);
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
	int right_x = bar_w - padding - btn_w * 2 - 10;  /* 10px gap between buttons */

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

	/* Subtitles button */
	{
		int sub_x = right_x + btn_w + 10;
		float *bg = (osd_menu_open == OSD_MENU_SUBTITLES) ? btn_selected : btn_bg;
		drawrect(playback_osd_tree, sub_x, (bar_h - btn_h) / 2, btn_w, btn_h, bg);

		text_tree = wlr_scene_tree_create(playback_osd_tree);
		if (text_tree) {
			wlr_scene_node_set_position(&text_tree->node, sub_x + 15, (bar_h - 16) / 2);
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

	/* Position and show */
	wlr_scene_node_set_position(&playback_osd_tree->node, m->m.x, m->m.y + bar_y);
	wlr_scene_node_set_enabled(&playback_osd_tree->node, 1);
	osd_visible = 1;

	/* Schedule auto-hide (1 second) when playing, not paused */
	int is_playing = active_videoplayer && active_videoplayer->state == VP_STATE_PLAYING;
	if (is_playing && osd_menu_open == OSD_MENU_NONE) {
		if (!playback_osd_timer)
			playback_osd_timer = wl_event_loop_add_timer(event_loop, playback_osd_timeout, NULL);
		if (playback_osd_timer)
			wl_event_source_timer_update(playback_osd_timer, 1000);
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

	wlr_log(WLR_INFO, "CVT-RB mode: %s clock=%d htotal=%d vtotal=%d",
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
		wlr_log(WLR_INFO, "Output is not DRM, cannot add custom mode");
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
		wlr_log(WLR_INFO, "Generated CVT base mode: %dx%d htotal=%d vtotal=%d",
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

		wlr_log(WLR_INFO, "Fixed mode: trying %.3f Hz -> %d Hz (%dx multiplier)",
				target_hz, target_vrefresh, multiplier);

		/* Generate fixed mode - only changes clock, preserves all timings */
		generate_fixed_mode(&drm_mode, &base_drm_mode, target_vrefresh);

		wlr_log(WLR_INFO, "Fixed mode params: clock=%d htotal=%d vtotal=%d vrefresh=%d",
				drm_mode.clock, drm_mode.htotal, drm_mode.vtotal, drm_mode.vrefresh);

		/* Add custom mode to DRM connector */
		new_mode = wlr_drm_connector_add_mode(m->wlr_output, &drm_mode);
		if (!new_mode) {
			wlr_log(WLR_DEBUG, "wlr_drm_connector_add_mode failed for fixed %d Hz", target_vrefresh);
			continue;
		}

		wlr_log(WLR_INFO, "Added fixed mode: %dx%d@%d mHz",
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

				wlr_log(WLR_INFO, "Fixed mode %d Hz applied (%dx %.3f fps)",
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

			wlr_log(WLR_INFO, "CVT fallback: trying %.3f Hz -> %.3f Hz (%dx multiplier)",
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

					wlr_log(WLR_INFO, "CVT mode %.3f Hz applied (%dx %.3f fps)",
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

				wlr_log(WLR_INFO, "Fallback mode %d.%03d Hz applied (%dx %.3f Hz)",
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

		/* Check if this is a game - video detection runs silently for games */
		int is_game = is_game_content(c) || client_wants_tearing(c);

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

		/* Already successfully detected and mode set */
		if (c->video_detect_phase == 2 && (m->video_mode_active || m->vrr_active)) {
			/* Check if Hz changed significantly - video may have switched */
			if (hz > 0.0f && fabsf(hz - c->detected_video_hz) > 0.5f) {
				wlr_log(WLR_INFO, "Video Hz changed from %.3f to %.3f, re-evaluating",
						c->detected_video_hz, hz);
				c->detected_video_hz = hz;
				apply_best_video_mode(m, hz);
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
			if (hz > 0.0f) {
				/* Successfully detected! Apply best mode */
				c->detected_video_hz = hz;
				c->video_detect_phase = 2;
				c->video_detect_retries = 0;

				wlr_log(WLR_INFO, "Video detected at %.3f Hz on %s, applying best mode",
						hz, m->wlr_output->name);

				if (apply_best_video_mode(m, hz)) {
					wlr_log(WLR_INFO, "Successfully applied optimal video mode");
				} else {
					wlr_log(WLR_ERROR, "Failed to apply video mode");
				}
				continue;
			}

			/* No stable framerate detected yet */
			if (is_video) {
				/* Content-type says video but can't detect fps.
				 * Use default 59.94 Hz as fallback. */
				wlr_log(WLR_INFO, "Video content-type detected but fps unclear, using 59.94 Hz");
				c->detected_video_hz = 59.94f;
				c->video_detect_phase = 2;
				apply_best_video_mode(m, 59.94f);
				continue;
			}

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
					float use_hz;
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

					c->detected_video_hz = use_hz;
					c->video_detect_phase = 2;

					wlr_log(WLR_INFO, "Estimated framerate %.3f Hz (raw ~%.1f Hz) on %s",
							use_hz, raw_hz, m->wlr_output->name);

					apply_best_video_mode(m, use_hz);
					continue;
				}

				/* Hz outside video range - likely not video content */
				wlr_log(WLR_DEBUG, "Measured Hz (~%.1f) outside video range", raw_hz);
			}

			/* No video detected - silent retry (1 retry only for speed) */
			c->video_detect_retries++;

			if (c->video_detect_retries < 1) {
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

	/* Continue checking while fullscreen clients exist (fast interval) */
	if (any_fullscreen_active)
		schedule_video_check(200);
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
		if (!m->wlr_output->enabled)
			continue;
		wl_list_for_each(c, &clients, link) {
			if (c->output && c->mon != m
					&& strcmp(m->wlr_output->name, c->output) == 0) {
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
		/* make sure fullscreen clients have the right size */
		if ((c = focustop(m)) && c->isfullscreen)
			resize(c, m->m, 0);

		/* Try to re-set the gamma LUT when updating monitors,
		 * it's only really needed when enabling a disabled output, but meh. */
		m->gamma_lut_changed = 1;

		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;

		if (!selmon) {
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

