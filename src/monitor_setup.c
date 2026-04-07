#include "nixlytile.h"
#include "client.h"

/*
 * Monitor Setup Popup — graphical UI for arranging multi-monitor layout.
 *
 * Appears automatically when multiple monitors are connected and
 * ~/.local/nixlyos/monitors.conf doesn't exist yet. Also accessible
 * via the modkey+p launcher by searching "monitors".
 *
 * Users drag monitor boxes to reorder (left-to-right), right-click
 * to rotate (landscape/portrait), and click Apply to write config
 * and apply instantly.
 */

/* ── helpers ───────────────────────────────────────────────────────── */

static void
render_text_at(struct wlr_scene_tree *tree, const char *text,
		int base_x, int base_y, const float color[static 4])
{
	int pen_x = 0;
	uint32_t prev_cp = 0;
	const float *prev_fg;

	if (!tree || !text || !statusfont.font)
		return;

	prev_fg = statusbar_fg_override;
	statusbar_fg_override = color;

	for (size_t i = 0; text[i]; i++) {
		long kern_x = 0, kern_y = 0;
		uint32_t cp = (unsigned char)text[i];
		const struct fcft_glyph *glyph;
		struct wlr_buffer *buffer;
		struct wlr_scene_buffer *scene_buf;

		if (prev_cp)
			fcft_kerning(statusfont.font, prev_cp, cp, &kern_x, &kern_y);
		pen_x += (int)kern_x;

		glyph = fcft_rasterize_char_utf32(statusfont.font, cp, statusbar_font_subpixel);
		if (!glyph || !glyph->pix) {
			prev_cp = cp;
			continue;
		}

		buffer = statusbar_buffer_from_glyph(glyph);
		if (buffer) {
			scene_buf = wlr_scene_buffer_create(tree, NULL);
			if (scene_buf) {
				wlr_scene_buffer_set_buffer(scene_buf, buffer);
				wlr_scene_node_set_position(&scene_buf->node,
					base_x + pen_x + glyph->x,
					base_y - glyph->y);
			}
			wlr_buffer_drop(buffer);
		}
		pen_x += glyph->advance.x;
		if (text[i + 1])
			pen_x += statusbar_font_spacing;
		prev_cp = cp;
	}

	statusbar_fg_override = prev_fg;
}

static int
text_width(const char *text)
{
	return status_text_width(text);
}

static void
clear_tree_children(struct wlr_scene_tree *tree, struct wlr_scene_tree *keep)
{
	struct wlr_scene_node *node, *tmp;

	if (!tree)
		return;
	wl_list_for_each_safe(node, tmp, &tree->children, link) {
		if (keep && node == &keep->node)
			continue;
		wlr_scene_node_destroy(node);
	}
}

/* ── animation ─────────────────────────────────────────────────────── */

static int
monitor_setup_animate_cb(void *data)
{
	Monitor *m = data;
	MonitorSetup *ms;
	int still_moving = 0;
	float ease = 0.25f;

	if (!m)
		return 0;

	ms = &m->monitor_setup;
	if (!ms->visible) {
		ms->animating = 0;
		return 0;
	}

	for (int i = 0; i < ms->entry_count; i++) {
		SetupMonitorEntry *e = &ms->entries[i];

		/* Skip dragged entry — it follows cursor directly */
		if (ms->dragging == i)
			continue;

		float dx = e->target_x - e->anim_x;
		float dy = e->target_y - e->anim_y;
		float dw = e->target_w - e->anim_w;
		float dh = e->target_h - e->anim_h;
		float dr = e->target_rot - e->anim_rot;

		if (fabsf(dx) > 0.5f) { e->anim_x += dx * ease; still_moving = 1; }
		else e->anim_x = e->target_x;

		if (fabsf(dy) > 0.5f) { e->anim_y += dy * ease; still_moving = 1; }
		else e->anim_y = e->target_y;

		if (fabsf(dw) > 0.5f) { e->anim_w += dw * ease; still_moving = 1; }
		else e->anim_w = e->target_w;

		if (fabsf(dh) > 0.5f) { e->anim_h += dh * ease; still_moving = 1; }
		else e->anim_h = e->target_h;

		if (fabsf(dr) > 0.5f) { e->anim_rot += dr * ease; still_moving = 1; }
		else e->anim_rot = e->target_rot;
	}

	monitor_setup_render(m);

	if (still_moving) {
		wl_event_source_timer_update(ms->anim_timer, 16);
	} else {
		ms->animating = 0;
	}

	return 0;
}

static void
start_animation(Monitor *m)
{
	MonitorSetup *ms = &m->monitor_setup;

	if (ms->animating)
		return;

	ms->animating = 1;
	if (!ms->anim_timer)
		ms->anim_timer = wl_event_loop_add_timer(event_loop,
			monitor_setup_animate_cb, m);
	if (ms->anim_timer)
		wl_event_source_timer_update(ms->anim_timer, 16);
}

/* ── box geometry computation ──────────────────────────────────────── */

static void
compute_box_layout(MonitorSetup *ms, int popup_w, int popup_h)
{
	int n = ms->entry_count;
	int padding = 40;
	int spacing = 30;
	int title_h = statusfont.height + 20;
	int button_area_h = statusfont.height + 30;
	int avail_w = popup_w - 2 * padding;
	int avail_h = popup_h - title_h - button_area_h - 2 * padding;
	int max_box_w, max_box_h;
	int total_spacing;

	if (n <= 0)
		return;

	total_spacing = (n - 1) * spacing;
	max_box_w = (avail_w - total_spacing) / n;
	max_box_h = avail_h;

	/* Limit box size */
	if (max_box_w > 300) max_box_w = 300;
	if (max_box_h > 200) max_box_h = 200;

	/* Sort entries by order for positioning */
	/* Simple bubble sort (max 8 entries) */
	for (int i = 0; i < n - 1; i++) {
		for (int j = 0; j < n - 1 - i; j++) {
			if (ms->entries[j].order > ms->entries[j + 1].order) {
				SetupMonitorEntry tmp = ms->entries[j];
				ms->entries[j] = ms->entries[j + 1];
				ms->entries[j + 1] = tmp;
			}
		}
	}

	/* Compute total width needed */
	int total_w = 0;
	for (int i = 0; i < n; i++) {
		SetupMonitorEntry *e = &ms->entries[i];
		float aspect;
		int bw, bh;

		/* Use aspect ratio based on transform */
		if (e->transform == WL_OUTPUT_TRANSFORM_90 ||
		    e->transform == WL_OUTPUT_TRANSFORM_270) {
			aspect = (float)e->height / (float)e->width;
		} else {
			aspect = (float)e->width / (float)e->height;
		}

		/* Fit within max box, preserving aspect */
		bh = max_box_h;
		bw = (int)(bh * aspect);
		if (bw > max_box_w) {
			bw = max_box_w;
			bh = (int)(bw / aspect);
		}
		if (bw < 60) bw = 60;
		if (bh < 40) bh = 40;

		e->target_w = (float)bw;
		e->target_h = (float)bh;
		total_w += bw;
	}
	total_w += total_spacing;

	/* Center the row horizontally */
	int start_x = (popup_w - total_w) / 2;
	int center_y = title_h + padding + avail_h / 2;
	int cx = start_x;

	for (int i = 0; i < n; i++) {
		SetupMonitorEntry *e = &ms->entries[i];
		int bw = (int)e->target_w;
		int bh = (int)e->target_h;

		e->target_x = (float)cx;
		e->target_y = (float)(center_y - bh / 2);
		e->box_x = cx;
		e->box_y = center_y - bh / 2;
		e->box_w = bw;
		e->box_h = bh;

		cx += bw + spacing;
	}
}

/* ── show / hide ───────────────────────────────────────────────────── */

void
monitor_setup_show(Monitor *m)
{
	MonitorSetup *ms;
	Monitor *other;
	int idx = 0;

	if (!m)
		return;

	ms = &m->monitor_setup;

	/* Populate entries from all monitors */
	ms->entry_count = 0;
	ms->dragging = -1;
	ms->ctx_visible = 0;

	/* If monitors.conf exists, load order/transform from it */
	int has_conf = monitors_conf_exists();

	wl_list_for_each(other, &mons, link) {
		if (!other->wlr_output->enabled || other->is_mirror)
			continue;
		if (idx >= MAX_SETUP_MONITORS)
			break;

		SetupMonitorEntry *e = &ms->entries[idx];
		memset(e, 0, sizeof(*e));
		snprintf(e->name, sizeof(e->name), "%s", other->wlr_output->name);

		if (other->wlr_output->current_mode) {
			e->width = other->wlr_output->current_mode->width;
			e->height = other->wlr_output->current_mode->height;
			e->refresh = (float)other->wlr_output->current_mode->refresh / 1000.0f;
		} else {
			e->width = other->m.width > 0 ? other->m.width : 1920;
			e->height = other->m.height > 0 ? other->m.height : 1080;
			e->refresh = 60.0f;
		}

		e->transform = other->wlr_output->transform;

		/* If config exists, find this monitor's configured order */
		if (has_conf) {
			RuntimeMonitorConfig *cfg = find_monitor_config(other->wlr_output->name);
			if (cfg) {
				e->transform = cfg->transform;
				/* Position determines order */
				if (cfg->position == MON_POS_MASTER)
					e->order = 0;
				else if (cfg->position == MON_POS_LEFT)
					e->order = -1; /* will be fixed below */
				else
					e->order = idx + 1;
			} else {
				e->order = idx;
			}
		} else {
			/* Use current physical position (left-to-right by x) */
			e->order = idx;
		}

		e->target_rot = (e->transform == WL_OUTPUT_TRANSFORM_90 ||
				 e->transform == WL_OUTPUT_TRANSFORM_270) ? 90.0f : 0.0f;
		e->anim_rot = e->target_rot;

		idx++;
	}
	ms->entry_count = idx;

	if (ms->entry_count == 0)
		return;

	/* Sort by physical x-position if no config exists */
	if (!has_conf) {
		/* Assign order based on x position of monitors */
		for (int i = 0; i < ms->entry_count; i++) {
			Monitor *om;
			wl_list_for_each(om, &mons, link) {
				if (strcmp(om->wlr_output->name, ms->entries[i].name) == 0) {
					ms->entries[i].order = om->m.x;
					break;
				}
			}
		}
		/* Now normalize orders to 0..N-1 */
		for (int i = 0; i < ms->entry_count - 1; i++) {
			for (int j = 0; j < ms->entry_count - 1 - i; j++) {
				if (ms->entries[j].order > ms->entries[j + 1].order) {
					SetupMonitorEntry tmp = ms->entries[j];
					ms->entries[j] = ms->entries[j + 1];
					ms->entries[j + 1] = tmp;
				}
			}
		}
		for (int i = 0; i < ms->entry_count; i++)
			ms->entries[i].order = i;
	} else {
		/* Fix left=-1 and normalize */
		for (int i = 0; i < ms->entry_count - 1; i++) {
			for (int j = 0; j < ms->entry_count - 1 - i; j++) {
				if (ms->entries[j].order > ms->entries[j + 1].order) {
					SetupMonitorEntry tmp = ms->entries[j];
					ms->entries[j] = ms->entries[j + 1];
					ms->entries[j + 1] = tmp;
				}
			}
		}
		for (int i = 0; i < ms->entry_count; i++)
			ms->entries[i].order = i;
	}

	/* Popup size: ~80% width, 60% height */
	ms->width = (int)(m->m.width * 0.8f);
	ms->height = (int)(m->m.height * 0.6f);
	ms->x = m->m.x + (m->m.width - ms->width) / 2;
	ms->y = m->m.y + (m->m.height - ms->height) / 2;

	/* Compute box layout */
	compute_box_layout(ms, ms->width, ms->height);

	/* Initialize animation state to target (no initial animation) */
	for (int i = 0; i < ms->entry_count; i++) {
		SetupMonitorEntry *e = &ms->entries[i];
		e->anim_x = e->target_x;
		e->anim_y = e->target_y;
		e->anim_w = e->target_w;
		e->anim_h = e->target_h;
	}

	ms->visible = 1;

	/* Create scene tree if needed */
	if (!ms->tree) {
		ms->tree = wlr_scene_tree_create(layers[LyrBlock]);
		if (ms->tree)
			ms->bg = wlr_scene_tree_create(ms->tree);
	}

	/* Create on-screen labels on each physical monitor */
	for (int i = 0; i < ms->entry_count; i++) {
		Monitor *om;
		wl_list_for_each(om, &mons, link) {
			if (strcmp(om->wlr_output->name, ms->entries[i].name) == 0) {
				if (!ms->label_trees[i])
					ms->label_trees[i] = wlr_scene_tree_create(layers[LyrBlock]);
				if (ms->label_trees[i]) {
					clear_tree_children(ms->label_trees[i], NULL);
					char label[128];
					snprintf(label, sizeof(label), "Screen %d (%.0fHz)",
						ms->entries[i].order + 1, ms->entries[i].refresh);

					float label_bg[4] = {1.0f, 1.0f, 1.0f, 0.95f};
					float label_fg[4] = {0.0f, 0.0f, 0.0f, 1.0f};

					int tw = text_width(label);
					int tw2 = text_width(ms->entries[i].name);
					int lw = (tw > tw2 ? tw : tw2) + 24;
					int lh = statusfont.height * 2 + 20;

					wlr_scene_node_set_position(&ms->label_trees[i]->node,
						om->m.x + 20, om->m.y + 20);
					drawroundedrect(ms->label_trees[i], 0, 0, lw, lh, label_bg);
					render_text_at(ms->label_trees[i], label,
						12, 8 + statusfont.ascent, label_fg);
					render_text_at(ms->label_trees[i], ms->entries[i].name,
						12, 8 + statusfont.height + 4 + statusfont.ascent, label_fg);
					wlr_scene_node_set_enabled(&ms->label_trees[i]->node, 1);
				}
				break;
			}
		}
	}

	monitor_setup_render(m);

	wlr_log(WLR_INFO, "Monitor setup popup shown with %d monitors", ms->entry_count);
}

void
monitor_setup_hide(Monitor *m)
{
	MonitorSetup *ms;

	if (!m)
		return;

	ms = &m->monitor_setup;
	ms->visible = 0;
	ms->dragging = -1;
	ms->ctx_visible = 0;

	if (ms->anim_timer) {
		wl_event_source_timer_update(ms->anim_timer, 0);
		ms->animating = 0;
	}

	if (ms->tree)
		wlr_scene_node_set_enabled(&ms->tree->node, 0);

	/* Hide on-screen labels */
	for (int i = 0; i < MAX_SETUP_MONITORS; i++) {
		if (ms->label_trees[i])
			wlr_scene_node_set_enabled(&ms->label_trees[i]->node, 0);
	}
}

Monitor *
monitor_setup_visible_monitor(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (m->monitor_setup.visible)
			return m;
	}
	return NULL;
}

/* ── rendering ─────────────────────────────────────────────────────── */

void
monitor_setup_render(Monitor *m)
{
	MonitorSetup *ms;
	int padding = 20;
	int title_h = statusfont.height + 20;
	float bg_col[4] = {0.12f, 0.12f, 0.14f, 0.95f};
	float border_col[4] = {0.3f, 0.3f, 0.35f, 1.0f};
	float box_bg[4] = {0.18f, 0.20f, 0.22f, 1.0f};
	float box_border[4] = {0.4f, 0.5f, 0.7f, 1.0f};
	float box_drag_border[4] = {0.5f, 0.7f, 1.0f, 1.0f};
	float title_col[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float text_col[4] = {0.9f, 0.9f, 0.9f, 1.0f};
	float sub_col[4] = {0.6f, 0.6f, 0.65f, 1.0f};
	float apply_bg[4] = {0.2f, 0.5f, 0.3f, 1.0f};
	float cancel_bg[4] = {0.4f, 0.2f, 0.2f, 1.0f};
	float btn_text[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float ctx_bg[4] = {0.2f, 0.2f, 0.24f, 0.98f};
	float ctx_border[4] = {0.4f, 0.4f, 0.45f, 1.0f};

	if (!m)
		return;

	ms = &m->monitor_setup;
	if (!ms->tree || !ms->visible)
		return;

	if (!statusfont.font)
		loadstatusfont();
	if (!statusfont.font)
		return;

	/* Clear previous */
	clear_tree_children(ms->tree, ms->bg);
	if (ms->bg)
		clear_tree_children(ms->bg, NULL);

	/* Position popup */
	wlr_scene_node_set_position(&ms->tree->node, ms->x, ms->y);

	/* Background + border */
	if (ms->bg) {
		wlr_scene_node_set_position(&ms->bg->node, 0, 0);
		drawrect(ms->bg, 0, 0, ms->width, ms->height, bg_col);
		draw_border(ms->bg, 0, 0, ms->width, ms->height, 2, border_col);
	}

	/* Title */
	{
		const char *title = "Monitor Setup";
		int tw = text_width(title);
		render_text_at(ms->tree, title,
			(ms->width - tw) / 2, padding + statusfont.ascent, title_col);
	}

	/* Monitor boxes */
	for (int i = 0; i < ms->entry_count; i++) {
		SetupMonitorEntry *e = &ms->entries[i];
		int bx = (int)e->anim_x;
		int by = (int)e->anim_y;
		int bw = (int)e->anim_w;
		int bh = (int)e->anim_h;
		float *bord = (ms->dragging == i) ? box_drag_border : box_border;
		char line1[128], line2[64];
		int tw1, tw2;

		if (bw <= 0 || bh <= 0)
			continue;

		/* Box background */
		drawroundedrect(ms->tree, bx, by, bw, bh, box_bg);
		draw_border(ms->tree, bx, by, bw, bh, 2, bord);

		/* Screen N (XXHz) */
		snprintf(line1, sizeof(line1), "Screen %d (%.0fHz)",
			e->order + 1, e->refresh);
		tw1 = text_width(line1);
		if (tw1 < bw - 10) {
			render_text_at(ms->tree, line1,
				bx + (bw - tw1) / 2,
				by + bh / 2 - statusfont.height / 2 + statusfont.ascent - statusfont.height / 2,
				text_col);
		}

		/* Connector name */
		snprintf(line2, sizeof(line2), "%s", e->name);
		tw2 = text_width(line2);
		if (tw2 < bw - 10) {
			render_text_at(ms->tree, line2,
				bx + (bw - tw2) / 2,
				by + bh / 2 + statusfont.height / 2 + statusfont.ascent - statusfont.height / 2 + 4,
				sub_col);
		}

		/* Resolution/transform indicator */
		{
			char res[64];
			if (e->transform == WL_OUTPUT_TRANSFORM_90 ||
			    e->transform == WL_OUTPUT_TRANSFORM_270)
				snprintf(res, sizeof(res), "%dx%d (R)", e->height, e->width);
			else
				snprintf(res, sizeof(res), "%dx%d", e->width, e->height);
			int rw = text_width(res);
			if (rw < bw - 10) {
				float dim_col[4] = {0.5f, 0.5f, 0.55f, 1.0f};
				render_text_at(ms->tree, res,
					bx + (bw - rw) / 2,
					by + bh - 8 - statusfont.descent,
					dim_col);
			}
		}
	}

	/* Apply button */
	{
		const char *apply_text = "Apply";
		int atw = text_width(apply_text);
		ms->apply_w = atw + 40;
		ms->apply_h = statusfont.height + 16;
		ms->apply_x = ms->width - padding - ms->apply_w;
		ms->apply_y = ms->height - padding - ms->apply_h;

		drawroundedrect(ms->tree, ms->apply_x, ms->apply_y,
			ms->apply_w, ms->apply_h, apply_bg);
		render_text_at(ms->tree, apply_text,
			ms->apply_x + (ms->apply_w - atw) / 2,
			ms->apply_y + (ms->apply_h - statusfont.height) / 2 + statusfont.ascent,
			btn_text);
	}

	/* Cancel button */
	{
		const char *cancel_text = "Cancel";
		int ctw = text_width(cancel_text);
		ms->cancel_w = ctw + 40;
		ms->cancel_h = statusfont.height + 16;
		ms->cancel_x = padding;
		ms->cancel_y = ms->height - padding - ms->cancel_h;

		drawroundedrect(ms->tree, ms->cancel_x, ms->cancel_y,
			ms->cancel_w, ms->cancel_h, cancel_bg);
		render_text_at(ms->tree, cancel_text,
			ms->cancel_x + (ms->cancel_w - ctw) / 2,
			ms->cancel_y + (ms->cancel_h - statusfont.height) / 2 + statusfont.ascent,
			btn_text);
	}

	/* Context menu (right-click rotate) */
	if (ms->ctx_visible) {
		const char *rotate_text = "Rotate";
		int rtw = text_width(rotate_text);
		ms->ctx_w = rtw + 30;
		ms->ctx_h = statusfont.height + 16;

		drawrect(ms->tree, ms->ctx_x, ms->ctx_y, ms->ctx_w, ms->ctx_h, ctx_bg);
		draw_border(ms->tree, ms->ctx_x, ms->ctx_y, ms->ctx_w, ms->ctx_h, 1, ctx_border);
		render_text_at(ms->tree, rotate_text,
			ms->ctx_x + (ms->ctx_w - rtw) / 2,
			ms->ctx_y + (ms->ctx_h - statusfont.height) / 2 + statusfont.ascent,
			text_col);
	}

	wlr_scene_node_set_enabled(&ms->tree->node, 1);
}

/* ── drag-to-reorder ───────────────────────────────────────────────── */

static int
entry_at(MonitorSetup *ms, int lx, int ly)
{
	for (int i = 0; i < ms->entry_count; i++) {
		SetupMonitorEntry *e = &ms->entries[i];
		int bx = (int)e->anim_x;
		int by = (int)e->anim_y;
		int bw = (int)e->anim_w;
		int bh = (int)e->anim_h;

		if (lx >= bx && lx < bx + bw && ly >= by && ly < by + bh)
			return i;
	}
	return -1;
}

void
monitor_setup_handle_motion(Monitor *m, int gx, int gy)
{
	MonitorSetup *ms;
	int lx, ly;

	if (!m)
		return;

	ms = &m->monitor_setup;
	if (!ms->visible)
		return;

	/* Convert global to popup-local */
	lx = gx - ms->x;
	ly = gy - ms->y;

	if (ms->dragging >= 0 && ms->dragging < ms->entry_count) {
		SetupMonitorEntry *e = &ms->entries[ms->dragging];

		/* Follow cursor */
		e->anim_x = (float)(lx - ms->drag_offset_x);
		e->anim_y = (float)(ly - ms->drag_offset_y);
		e->target_x = e->anim_x;
		e->target_y = e->anim_y;

		/* Check if dragged past midpoint of neighbor → swap order */
		float drag_cx = e->anim_x + e->anim_w / 2.0f;

		for (int i = 0; i < ms->entry_count; i++) {
			if (i == ms->dragging)
				continue;
			SetupMonitorEntry *other = &ms->entries[i];
			float other_cx = other->target_x + other->target_w / 2.0f;

			if (e->order < other->order && drag_cx > other_cx) {
				/* Dragged right past neighbor — swap orders */
				int tmp = e->order;
				e->order = other->order;
				other->order = tmp;
				compute_box_layout(ms, ms->width, ms->height);
				/* Restore dragged entry position (compute_box_layout resets it) */
				e->target_x = e->anim_x;
				e->target_y = e->anim_y;
				start_animation(m);
				break;
			} else if (e->order > other->order && drag_cx < other_cx) {
				/* Dragged left past neighbor — swap orders */
				int tmp = e->order;
				e->order = other->order;
				other->order = tmp;
				compute_box_layout(ms, ms->width, ms->height);
				e->target_x = e->anim_x;
				e->target_y = e->anim_y;
				start_animation(m);
				break;
			}
		}

		/* Update on-screen labels with new order */
		for (int i = 0; i < ms->entry_count; i++) {
			Monitor *om;
			wl_list_for_each(om, &mons, link) {
				if (strcmp(om->wlr_output->name, ms->entries[i].name) == 0 &&
				    ms->label_trees[i]) {
					clear_tree_children(ms->label_trees[i], NULL);
					char label[128];
					snprintf(label, sizeof(label), "Screen %d (%.0fHz)",
						ms->entries[i].order + 1, ms->entries[i].refresh);
					float label_bg[4] = {1.0f, 1.0f, 1.0f, 0.95f};
					float label_fg[4] = {0.0f, 0.0f, 0.0f, 1.0f};
					int tw = text_width(label);
					int tw2 = text_width(ms->entries[i].name);
					int lw = (tw > tw2 ? tw : tw2) + 24;
					int lh = statusfont.height * 2 + 20;
					drawroundedrect(ms->label_trees[i], 0, 0, lw, lh, label_bg);
					render_text_at(ms->label_trees[i], label,
						12, 8 + statusfont.ascent, label_fg);
					render_text_at(ms->label_trees[i], ms->entries[i].name,
						12, 8 + statusfont.height + 4 + statusfont.ascent, label_fg);
					break;
				}
			}
		}

		monitor_setup_render(m);
	}
}

int
monitor_setup_handle_button(Monitor *m, int gx, int gy, uint32_t button, uint32_t state)
{
	MonitorSetup *ms;
	int lx, ly;
	int idx;

	if (!m)
		return 0;

	ms = &m->monitor_setup;
	if (!ms->visible)
		return 0;

	lx = gx - ms->x;
	ly = gy - ms->y;

	/* Check if click is outside popup */
	if (lx < 0 || ly < 0 || lx >= ms->width || ly >= ms->height) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			/* Close context menu if visible, otherwise close popup */
			if (ms->ctx_visible) {
				ms->ctx_visible = 0;
				monitor_setup_render(m);
			} else {
				monitor_setup_hide(m);
			}
		}
		return 1;
	}

	if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
		/* Mouse up — drop dragged entry */
		if (ms->dragging >= 0) {
			SetupMonitorEntry *e = &ms->entries[ms->dragging];
			ms->dragging = -1;
			/* Snap to computed position */
			compute_box_layout(ms, ms->width, ms->height);
			start_animation(m);
		}
		return 1;
	}

	/* Mouse down */
	if (button == BTN_LEFT) {
		/* Close context menu first */
		if (ms->ctx_visible) {
			/* Check if clicking the rotate option */
			if (lx >= ms->ctx_x && lx < ms->ctx_x + ms->ctx_w &&
			    ly >= ms->ctx_y && ly < ms->ctx_y + ms->ctx_h) {
				/* Toggle transform */
				int ci = ms->ctx_entry_idx;
				if (ci >= 0 && ci < ms->entry_count) {
					SetupMonitorEntry *e = &ms->entries[ci];
					if (e->transform == WL_OUTPUT_TRANSFORM_90 ||
					    e->transform == WL_OUTPUT_TRANSFORM_270) {
						e->transform = WL_OUTPUT_TRANSFORM_NORMAL;
						e->target_rot = 0.0f;
					} else {
						e->transform = WL_OUTPUT_TRANSFORM_90;
						e->target_rot = 90.0f;
					}
					/* Recompute layout (aspect ratio changed) */
					compute_box_layout(ms, ms->width, ms->height);
					start_animation(m);
				}
			}
			ms->ctx_visible = 0;
			monitor_setup_render(m);
			return 1;
		}

		/* Apply button */
		if (lx >= ms->apply_x && lx < ms->apply_x + ms->apply_w &&
		    ly >= ms->apply_y && ly < ms->apply_y + ms->apply_h) {
			monitor_setup_apply(m);
			return 1;
		}

		/* Cancel button */
		if (lx >= ms->cancel_x && lx < ms->cancel_x + ms->cancel_w &&
		    ly >= ms->cancel_y && ly < ms->cancel_y + ms->cancel_h) {
			monitor_setup_hide(m);
			return 1;
		}

		/* Start drag on a monitor box */
		idx = entry_at(ms, lx, ly);
		if (idx >= 0) {
			ms->dragging = idx;
			ms->drag_offset_x = lx - (int)ms->entries[idx].anim_x;
			ms->drag_offset_y = ly - (int)ms->entries[idx].anim_y;
			return 1;
		}
	} else if (button == BTN_RIGHT) {
		/* Right-click on a monitor box → show context menu */
		ms->ctx_visible = 0;
		idx = entry_at(ms, lx, ly);
		if (idx >= 0) {
			ms->ctx_entry_idx = idx;
			ms->ctx_x = lx;
			ms->ctx_y = ly;
			ms->ctx_visible = 1;
			monitor_setup_render(m);
		}
		return 1;
	}

	return 1; /* consume all clicks inside popup */
}

int
monitor_setup_handle_key(Monitor *m, xkb_keysym_t sym)
{
	if (!m || !m->monitor_setup.visible)
		return 0;

	if (sym == XKB_KEY_Escape) {
		if (m->monitor_setup.ctx_visible) {
			m->monitor_setup.ctx_visible = 0;
			monitor_setup_render(m);
		} else {
			monitor_setup_hide(m);
		}
		return 1;
	}

	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		monitor_setup_apply(m);
		return 1;
	}

	return 1; /* consume all keys while popup visible */
}

/* ── config write ──────────────────────────────────────────────────── */

void
write_monitors_conf(SetupMonitorEntry *entries, int count)
{
	char path[PATH_MAX], dir[PATH_MAX];
	const char *home;
	FILE *f;

	home = getenv("HOME");
	if (!home)
		return;

	snprintf(dir, sizeof(dir), "%s/.local/nixlyos", home);
	mkdir(dir, 0755);

	snprintf(path, sizeof(path), "%s/.local/nixlyos/monitors.conf", home);

	f = fopen(path, "w");
	if (!f) {
		wlr_log(WLR_ERROR, "Failed to write monitors.conf: %s", strerror(errno));
		return;
	}

	fprintf(f, "# Auto-generated by nixlytile monitor setup\n");

	/* Sort by order */
	SetupMonitorEntry sorted[MAX_SETUP_MONITORS];
	memcpy(sorted, entries, (size_t)count * sizeof(SetupMonitorEntry));
	for (int i = 0; i < count - 1; i++) {
		for (int j = 0; j < count - 1 - i; j++) {
			if (sorted[j].order > sorted[j + 1].order) {
				SetupMonitorEntry tmp = sorted[j];
				sorted[j] = sorted[j + 1];
				sorted[j + 1] = tmp;
			}
		}
	}

	for (int i = 0; i < count; i++) {
		const char *pos;
		if (i == 0)
			pos = "master";
		else
			pos = "right";

		if (sorted[i].transform == WL_OUTPUT_TRANSFORM_90)
			fprintf(f, "monitor = %s %s transform=rotate-90\n", sorted[i].name, pos);
		else if (sorted[i].transform == WL_OUTPUT_TRANSFORM_270)
			fprintf(f, "monitor = %s %s transform=rotate-270\n", sorted[i].name, pos);
		else
			fprintf(f, "monitor = %s %s\n", sorted[i].name, pos);
	}

	fclose(f);
	wlr_log(WLR_INFO, "Wrote monitors.conf with %d monitors", count);
}

/* ── apply ─────────────────────────────────────────────────────────── */

void
monitor_setup_apply(Monitor *m)
{
	MonitorSetup *ms;
	SetupMonitorEntry sorted[MAX_SETUP_MONITORS];

	if (!m)
		return;

	ms = &m->monitor_setup;

	/* Write config file */
	write_monitors_conf(ms->entries, ms->entry_count);

	/* Apply immediately: reload config into runtime_monitors */
	/* Clear existing monitor configs */
	runtime_monitor_count = 0;
	monitor_master_set = 0;
	memset(runtime_monitors, 0, sizeof(runtime_monitors));

	/* Load the file we just wrote */
	load_monitors_conf();

	/* Also reload main config (which may have additional settings) */
	/* Don't call full reload_config() as that resets monitor configs.
	 * Instead, directly apply positions. */

	/* Sort entries by order */
	memcpy(sorted, ms->entries, (size_t)ms->entry_count * sizeof(SetupMonitorEntry));
	for (int i = 0; i < ms->entry_count - 1; i++) {
		for (int j = 0; j < ms->entry_count - 1 - i; j++) {
			if (sorted[j].order > sorted[j + 1].order) {
				SetupMonitorEntry tmp = sorted[j];
				sorted[j] = sorted[j + 1];
				sorted[j + 1] = tmp;
			}
		}
	}

	/* Apply transforms and positions to each monitor */
	int pos_x = 0;
	for (int i = 0; i < ms->entry_count; i++) {
		Monitor *om;
		wl_list_for_each(om, &mons, link) {
			if (strcmp(om->wlr_output->name, sorted[i].name) != 0)
				continue;

			/* Apply transform if changed */
			if ((int)om->wlr_output->transform != sorted[i].transform) {
				struct wlr_output_state ostate;
				wlr_output_state_init(&ostate);
				wlr_output_state_set_transform(&ostate, sorted[i].transform);
				if (!wlr_output_commit_state(om->wlr_output, &ostate))
					wlr_log(WLR_ERROR, "Failed to set transform for %s",
						om->wlr_output->name);
				wlr_output_state_finish(&ostate);
			}

			/* Calculate effective dimensions */
			int ew, eh;
			if (sorted[i].transform == WL_OUTPUT_TRANSFORM_90 ||
			    sorted[i].transform == WL_OUTPUT_TRANSFORM_270) {
				ew = sorted[i].height;
				eh = sorted[i].width;
			} else {
				ew = sorted[i].width;
				eh = sorted[i].height;
			}

			/* Position in layout */
			wlr_output_layout_add(output_layout, om->wlr_output, pos_x, 0);
			pos_x += ew;
			break;
		}
	}

	/* Update all monitors */
	updatemons(NULL, NULL);

	/* Close popup */
	monitor_setup_hide(m);
}
