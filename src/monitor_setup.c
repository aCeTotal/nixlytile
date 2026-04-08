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

/* ── forward declarations ──────────────────────────────────────────── */
static void render_box_content(MonitorSetup *ms, int idx);
static int  grid_label_number(MonitorSetup *ms, int entry_idx);

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
		int size_changed = 0;
		int rot_changed = 0;

		/* Skip dragged entry — it follows cursor directly */
		if (ms->dragging == i)
			continue;

		float dx = e->target_x - e->anim_x;
		float dy = e->target_y - e->anim_y;

		if (fabsf(dx) > 0.5f) { e->anim_x += dx * ease; still_moving = 1; }
		else e->anim_x = e->target_x;

		if (fabsf(dy) > 0.5f) { e->anim_y += dy * ease; still_moving = 1; }
		else e->anim_y = e->target_y;

		/* Animate size smoothly */
		float dw = e->target_w - e->anim_w;
		float dh = e->target_h - e->anim_h;
		float dr = e->target_rot - e->anim_rot;

		if (fabsf(dw) > 0.5f) {
			e->anim_w += dw * ease; still_moving = 1; size_changed = 1;
		} else if (e->anim_w != e->target_w) {
			e->anim_w = e->target_w; size_changed = 1;
		}

		if (fabsf(dh) > 0.5f) {
			e->anim_h += dh * ease; still_moving = 1; size_changed = 1;
		} else if (e->anim_h != e->target_h) {
			e->anim_h = e->target_h; size_changed = 1;
		}

		if (fabsf(dr) > 0.5f) {
			e->anim_rot += dr * ease; still_moving = 1;
			rot_changed = 1;
		} else if (e->anim_rot != e->target_rot) {
			e->anim_rot = e->target_rot;
			rot_changed = 1;
		}

		/* Re-render content if size or rotation changed */
		if (size_changed || rot_changed)
			render_box_content(ms, i);

		/* Just reposition — no full scene rebuild */
		if (e->box_tree)
			wlr_scene_node_set_position(&e->box_tree->node,
				(int)e->anim_x, (int)e->anim_y);
	}

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

/* ── per-box rendering ─────────────────────────────────────────────── */

static void
render_box_content(MonitorSetup *ms, int idx)
{
	SetupMonitorEntry *e = &ms->entries[idx];
	int bw, bh, tw1, tw2, rw, num;
	char line1[128], line2[64], res[64];
	float box_bg[4] = {0.18f, 0.20f, 0.22f, 1.0f};
	float box_border[4] = {0.4f, 0.5f, 0.7f, 1.0f};
	float box_drag_border[4] = {0.5f, 0.7f, 1.0f, 1.0f};
	float text_col[4] = {0.9f, 0.9f, 0.9f, 1.0f};
	float sub_col[4] = {0.6f, 0.6f, 0.65f, 1.0f};
	float dim_col[4] = {0.5f, 0.5f, 0.55f, 1.0f};
	float *bord;

	if (!e->box_tree)
		return;

	clear_tree_children(e->box_tree, NULL);

	bw = (int)e->anim_w;
	bh = (int)e->anim_h;
	if (bw <= 0 || bh <= 0)
		return;

	/* Rotation flip effect: squeeze width at midpoint of rotation */
	float rot_rad = e->anim_rot * (float)M_PI / 90.0f;
	float rot_scale = fabsf(cosf(rot_rad));
	if (rot_scale < 0.05f)
		rot_scale = 0.05f;
	int vw = (int)(bw * rot_scale);
	if (vw < 4) vw = 4;
	int xoff = (bw - vw) / 2;

	bord = (ms->dragging == idx) ? box_drag_border : box_border;

	/* Box background + border */
	drawroundedrect(e->box_tree, xoff, 0, vw, bh, box_bg);
	draw_border(e->box_tree, xoff, 0, vw, bh, 2, bord);

	/* Screen N (XXHz) */
	num = grid_label_number(ms, idx);
	snprintf(line1, sizeof(line1), "Screen %d (%.0fHz)", num, e->refresh);
	tw1 = text_width(line1);
	if (tw1 < vw - 10) {
		render_text_at(e->box_tree, line1,
			xoff + (vw - tw1) / 2,
			bh / 2 - statusfont.height / 2 + statusfont.ascent - statusfont.height / 2,
			text_col);
	}

	/* Connector name */
	snprintf(line2, sizeof(line2), "%s", e->name);
	tw2 = text_width(line2);
	if (tw2 < vw - 10) {
		render_text_at(e->box_tree, line2,
			xoff + (vw - tw2) / 2,
			bh / 2 + statusfont.height / 2 + statusfont.ascent - statusfont.height / 2 + 4,
			sub_col);
	}

	/* Resolution/transform indicator */
	if (e->transform == WL_OUTPUT_TRANSFORM_90 ||
	    e->transform == WL_OUTPUT_TRANSFORM_270)
		snprintf(res, sizeof(res), "%dx%d (R)", e->height, e->width);
	else
		snprintf(res, sizeof(res), "%dx%d", e->width, e->height);
	rw = text_width(res);
	if (rw < vw - 10) {
		render_text_at(e->box_tree, res,
			xoff + (vw - rw) / 2,
			bh - 8 - statusfont.descent,
			dim_col);
	}
}

static void
update_onscreen_labels(MonitorSetup *ms)
{
	for (int i = 0; i < ms->entry_count; i++) {
		Monitor *om;
		wl_list_for_each(om, &mons, link) {
			if (strcmp(om->wlr_output->name, ms->entries[i].name) == 0 &&
			    ms->label_trees[i]) {
				clear_tree_children(ms->label_trees[i], NULL);
				char label[128];
				int num = grid_label_number(ms, i);
				snprintf(label, sizeof(label), "Screen %d (%.0fHz)",
					num, ms->entries[i].refresh);
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
}

/* ── grid compaction ───────────────────────────────────────────────── */

static void
compact_grid(MonitorSetup *ms)
{
	int n = ms->entry_count;
	int max_col = 0, max_row = 0;

	if (n <= 0)
		return;

	/* Normalize: ensure minimum col and row are 0 */
	{
		int min_col = ms->entries[0].grid_col;
		int min_row = ms->entries[0].grid_row;
		for (int i = 1; i < n; i++) {
			if (ms->entries[i].grid_col < min_col)
				min_col = ms->entries[i].grid_col;
			if (ms->entries[i].grid_row < min_row)
				min_row = ms->entries[i].grid_row;
		}
		if (min_col != 0 || min_row != 0) {
			for (int i = 0; i < n; i++) {
				ms->entries[i].grid_col -= min_col;
				ms->entries[i].grid_row -= min_row;
			}
		}
	}

	/* Find grid extents */
	for (int i = 0; i < n; i++) {
		if (ms->entries[i].grid_col > max_col)
			max_col = ms->entries[i].grid_col;
		if (ms->entries[i].grid_row > max_row)
			max_row = ms->entries[i].grid_row;
	}

	/* Remove empty columns */
	for (int c = max_col; c >= 0; c--) {
		int used = 0;
		for (int i = 0; i < n; i++) {
			if (ms->entries[i].grid_col == c) {
				used = 1;
				break;
			}
		}
		if (!used) {
			for (int i = 0; i < n; i++) {
				if (ms->entries[i].grid_col > c)
					ms->entries[i].grid_col--;
			}
		}
	}

	/* Remove empty rows */
	for (int r = max_row; r >= 0; r--) {
		int used = 0;
		for (int i = 0; i < n; i++) {
			if (ms->entries[i].grid_row == r) {
				used = 1;
				break;
			}
		}
		if (!used) {
			for (int i = 0; i < n; i++) {
				if (ms->entries[i].grid_row > r)
					ms->entries[i].grid_row--;
			}
		}
	}
}

/* ── box geometry computation (2D grid) ───────────────────────────── */

static void
compute_box_layout(MonitorSetup *ms, int popup_w, int popup_h)
{
	int n = ms->entry_count;
	int padding = 40;
	int spacing = 20;
	int title_h = statusfont.height + 20;
	int button_area_h = statusfont.height + 30;
	int avail_w = popup_w - 2 * padding;
	int avail_h = popup_h - title_h - button_area_h - 2 * padding;
	int max_col = 0, max_row = 0;

	if (n <= 0)
		return;

	/* Find grid dimensions */
	for (int i = 0; i < n; i++) {
		if (ms->entries[i].grid_col > max_col)
			max_col = ms->entries[i].grid_col;
		if (ms->entries[i].grid_row > max_row)
			max_row = ms->entries[i].grid_row;
	}

	ms->grid_cols = max_col + 1;
	ms->grid_rows = max_row + 1;

	/* Compute uniform cell size */
	int cell_w = (avail_w - (ms->grid_cols - 1) * spacing) / ms->grid_cols;
	int cell_h = (avail_h - (ms->grid_rows - 1) * spacing) / ms->grid_rows;
	if (cell_w > 300) cell_w = 300;
	if (cell_h > 200) cell_h = 200;
	if (cell_w < 80) cell_w = 80;
	if (cell_h < 60) cell_h = 60;

	ms->cell_w = cell_w;
	ms->cell_h = cell_h;

	/* Uniform box size (same for all entries, slightly smaller than cell) */
	int box_w = cell_w - 10;
	int box_h = cell_h - 10;

	/* Total grid pixel size */
	int grid_w = ms->grid_cols * cell_w + (ms->grid_cols - 1) * spacing;
	int grid_h = ms->grid_rows * cell_h + (ms->grid_rows - 1) * spacing;

	/* Center grid in available area */
	int origin_x = padding + (avail_w - grid_w) / 2;
	int origin_y = title_h + padding + (avail_h - grid_h) / 2;

	ms->grid_origin_x = origin_x;
	ms->grid_origin_y = origin_y;

	for (int i = 0; i < n; i++) {
		SetupMonitorEntry *e = &ms->entries[i];
		int cx = origin_x + e->grid_col * (cell_w + spacing);
		int cy = origin_y + e->grid_row * (cell_h + spacing);

		e->target_w = (float)box_w;
		e->target_h = (float)box_h;
		/* Center box within cell */
		e->target_x = (float)(cx + (cell_w - box_w) / 2);
		e->target_y = (float)(cy + (cell_h - box_h) / 2);
		e->box_x = (int)e->target_x;
		e->box_y = (int)e->target_y;
		e->box_w = box_w;
		e->box_h = box_h;
	}
}

/* ── show / hide ───────────────────────────────────────────────────── */

/* ── helper: row-major label number ────────────────────────────────── */

static int
grid_label_number(MonitorSetup *ms, int entry_idx)
{
	/* Assign sequential numbers in row-major order */
	int num = 1;
	SetupMonitorEntry *target = &ms->entries[entry_idx];
	for (int i = 0; i < ms->entry_count; i++) {
		SetupMonitorEntry *e = &ms->entries[i];
		if (e->grid_row < target->grid_row ||
		    (e->grid_row == target->grid_row && e->grid_col < target->grid_col))
			num++;
	}
	return num;
}

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
	ms->ctx_submenu = 0;

	/* If monitors.conf exists, load grid/transform from it */
	int has_conf = monitors_conf_exists();

	wl_list_for_each(other, &mons, link) {
		struct wlr_output_mode *mode;

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

		/* Enumerate available modes */
		e->mode_count = 0;
		e->selected_mode = -1;
		wl_list_for_each(mode, &other->wlr_output->modes, link) {
			if (e->mode_count >= MAX_SETUP_MODES)
				break;
			/* Check for duplicates */
			int dup = 0;
			for (int j = 0; j < e->mode_count; j++) {
				if (e->modes[j].width == mode->width &&
				    e->modes[j].height == mode->height &&
				    e->modes[j].refresh_mhz == mode->refresh) {
					dup = 1;
					break;
				}
			}
			if (!dup) {
				e->modes[e->mode_count].width = mode->width;
				e->modes[e->mode_count].height = mode->height;
				e->modes[e->mode_count].refresh_mhz = mode->refresh;
				/* Mark current mode as selected */
				if (mode->width == e->width && mode->height == e->height &&
				    abs(mode->refresh - (int)(e->refresh * 1000.0f)) < 500)
					e->selected_mode = e->mode_count;
				e->mode_count++;
			}
		}

		/* If config exists, load grid position and settings */
		if (has_conf) {
			RuntimeMonitorConfig *cfg = find_monitor_config(other->wlr_output->name);
			if (cfg) {
				e->transform = cfg->transform;
				if (cfg->grid_col >= 0 && cfg->grid_row >= 0) {
					e->grid_col = cfg->grid_col;
					e->grid_row = cfg->grid_row;
				} else {
					/* Convert old position format to grid */
					if (cfg->position == MON_POS_MASTER)
						e->grid_col = 0;
					else if (cfg->position == MON_POS_LEFT)
						e->grid_col = 0; /* will shift master right */
					else
						e->grid_col = idx;
					e->grid_row = 0;
				}
				/* Load resolution/refresh if specified */
				if (cfg->width > 0 && cfg->height > 0) {
					e->width = cfg->width;
					e->height = cfg->height;
					if (cfg->refresh > 0)
						e->refresh = cfg->refresh;
				}
			} else {
				e->grid_col = idx;
				e->grid_row = 0;
			}
		} else {
			/* Assign grid positions based on physical x,y layout */
			e->grid_col = idx;
			e->grid_row = 0;
		}

		e->target_rot = (e->transform == WL_OUTPUT_TRANSFORM_90 ||
				 e->transform == WL_OUTPUT_TRANSFORM_270) ? 90.0f : 0.0f;
		e->anim_rot = e->target_rot;

		idx++;
	}
	ms->entry_count = idx;

	if (ms->entry_count == 0)
		return;

	/* If no config, sort by physical x-position and assign grid cols */
	if (!has_conf) {
		/* Sort by physical x position */
		for (int i = 0; i < ms->entry_count; i++) {
			Monitor *om;
			wl_list_for_each(om, &mons, link) {
				if (strcmp(om->wlr_output->name, ms->entries[i].name) == 0) {
					ms->entries[i].grid_col = om->m.x;
					break;
				}
			}
		}
		/* Normalize to 0-based consecutive cols */
		for (int i = 0; i < ms->entry_count - 1; i++) {
			for (int j = 0; j < ms->entry_count - 1 - i; j++) {
				if (ms->entries[j].grid_col > ms->entries[j + 1].grid_col) {
					SetupMonitorEntry tmp = ms->entries[j];
					ms->entries[j] = ms->entries[j + 1];
					ms->entries[j + 1] = tmp;
				}
			}
		}
		for (int i = 0; i < ms->entry_count; i++) {
			ms->entries[i].grid_col = i;
			ms->entries[i].grid_row = 0;
		}
	}

	/* Ensure no two entries share the same grid cell */
	for (int i = 0; i < ms->entry_count; i++) {
		for (int j = i + 1; j < ms->entry_count; j++) {
			if (ms->entries[i].grid_col == ms->entries[j].grid_col &&
			    ms->entries[i].grid_row == ms->entries[j].grid_row) {
				/* Duplicate — move j to next available column */
				int mc = 0;
				for (int k = 0; k < ms->entry_count; k++) {
					if (ms->entries[k].grid_col > mc)
						mc = ms->entries[k].grid_col;
				}
				ms->entries[j].grid_col = mc + 1;
			}
		}
	}

	compact_grid(ms);

	/* Popup size: ~50% width, 70% height */
	ms->width = (int)(m->m.width * 0.50f);
	ms->height = (int)(m->m.height * 0.70f);
	if (ms->width < 500) ms->width = 500;
	if (ms->height < 400) ms->height = 400;
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
					int num = grid_label_number(ms, i);
					snprintf(label, sizeof(label), "Screen %d (%.0fHz)",
						num, ms->entries[i].refresh);

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

	/* Null out box_tree pointers (they're children of ms->tree) */
	for (int i = 0; i < MAX_SETUP_MONITORS; i++)
		ms->entries[i].box_tree = NULL;

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
	float bg_col[4] = {0.12f, 0.12f, 0.14f, 0.95f};
	float border_col[4] = {0.3f, 0.3f, 0.35f, 1.0f};
	float title_col[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float text_col[4] = {0.9f, 0.9f, 0.9f, 1.0f};
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

	/* Null out pointers — clear_tree_children will destroy them */
	for (int i = 0; i < ms->entry_count; i++)
		ms->entries[i].box_tree = NULL;
	ms->drop_indicator = NULL;

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

	/* Monitor boxes — use persistent per-box subtrees */
	for (int i = 0; i < ms->entry_count; i++) {
		SetupMonitorEntry *e = &ms->entries[i];
		e->box_tree = wlr_scene_tree_create(ms->tree);
		if (e->box_tree) {
			render_box_content(ms, i);
			wlr_scene_node_set_position(&e->box_tree->node,
				(int)e->anim_x, (int)e->anim_y);
		}
	}

	/* Raise dragged box to top */
	if (ms->dragging >= 0 && ms->dragging < ms->entry_count &&
	    ms->entries[ms->dragging].box_tree)
		wlr_scene_node_raise_to_top(&ms->entries[ms->dragging].box_tree->node);

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

	/* Context menu (right-click) */
	if (ms->ctx_visible) {
		float ctx_hover_bg[4] = {0.3f, 0.3f, 0.35f, 1.0f};
		float ctx_check_col[4] = {0.4f, 0.8f, 0.4f, 1.0f};

		if (ms->ctx_submenu == 0) {
			/* Main context menu: Rotate, Resolution >, Refresh Rate > */
			const char *items[] = { "Rotate", "Resolution >", "Refresh Rate >" };
			int item_count = 3;
			int item_h = statusfont.height + 12;
			int max_tw = 0;
			for (int i = 0; i < item_count; i++) {
				int tw = text_width(items[i]);
				if (tw > max_tw) max_tw = tw;
			}
			ms->ctx_w = max_tw + 30;
			ms->ctx_h = item_count * item_h + 4;

			/* Clamp to popup bounds */
			if (ms->ctx_x + ms->ctx_w > ms->width - 4)
				ms->ctx_x = ms->width - 4 - ms->ctx_w;
			if (ms->ctx_y + ms->ctx_h > ms->height - 4)
				ms->ctx_y = ms->height - 4 - ms->ctx_h;

			drawrect(ms->tree, ms->ctx_x, ms->ctx_y, ms->ctx_w, ms->ctx_h, ctx_bg);
			draw_border(ms->tree, ms->ctx_x, ms->ctx_y, ms->ctx_w, ms->ctx_h, 1, ctx_border);

			for (int i = 0; i < item_count; i++) {
				int iy = ms->ctx_y + 2 + i * item_h;
				if (ms->ctx_hover_item == i)
					drawrect(ms->tree, ms->ctx_x + 1, iy, ms->ctx_w - 2, item_h, ctx_hover_bg);
				render_text_at(ms->tree, items[i],
					ms->ctx_x + 10,
					iy + (item_h - statusfont.height) / 2 + statusfont.ascent,
					text_col);
			}
		} else if (ms->ctx_submenu == 1 && ms->ctx_entry_idx >= 0 &&
			   ms->ctx_entry_idx < ms->entry_count) {
			/* Resolution submenu */
			SetupMonitorEntry *ce = &ms->entries[ms->ctx_entry_idx];
			/* Collect unique resolutions */
			struct { int w, h; } resolutions[MAX_SETUP_MODES];
			int res_count = 0;
			int max_res_refresh[MAX_SETUP_MODES]; /* max refresh for each resolution */

			for (int j = 0; j < ce->mode_count; j++) {
				int found = -1;
				for (int k = 0; k < res_count; k++) {
					if (resolutions[k].w == ce->modes[j].width &&
					    resolutions[k].h == ce->modes[j].height) {
						found = k;
						break;
					}
				}
				if (found < 0 && res_count < MAX_SETUP_MODES) {
					resolutions[res_count].w = ce->modes[j].width;
					resolutions[res_count].h = ce->modes[j].height;
					max_res_refresh[res_count] = ce->modes[j].refresh_mhz;
					res_count++;
				} else if (found >= 0 && ce->modes[j].refresh_mhz > max_res_refresh[found]) {
					max_res_refresh[found] = ce->modes[j].refresh_mhz;
				}
			}

			/* Sort resolutions by total pixels descending */
			for (int a = 0; a < res_count - 1; a++) {
				for (int b = 0; b < res_count - 1 - a; b++) {
					long pa = (long)resolutions[b].w * resolutions[b].h;
					long pb = (long)resolutions[b + 1].w * resolutions[b + 1].h;
					if (pa < pb) {
						int tw = resolutions[b].w, th = resolutions[b].h;
						int tr = max_res_refresh[b];
						resolutions[b] = resolutions[b + 1];
						max_res_refresh[b] = max_res_refresh[b + 1];
						resolutions[b + 1].w = tw;
						resolutions[b + 1].h = th;
						max_res_refresh[b + 1] = tr;
					}
				}
			}

			int item_h = statusfont.height + 10;
			int max_visible = 10;
			int visible = res_count < max_visible ? res_count : max_visible;
			int max_tw = 0;
			for (int j = 0; j < res_count; j++) {
				char buf[64];
				snprintf(buf, sizeof(buf), "%dx%d", resolutions[j].w, resolutions[j].h);
				int tw = text_width(buf);
				if (tw > max_tw) max_tw = tw;
			}
			ms->ctx_w = max_tw + 60;
			ms->ctx_h = visible * item_h + 4;

			if (ms->ctx_x + ms->ctx_w > ms->width - 4)
				ms->ctx_x = ms->width - 4 - ms->ctx_w;
			if (ms->ctx_y + ms->ctx_h > ms->height - 4)
				ms->ctx_y = ms->height - 4 - ms->ctx_h;

			drawrect(ms->tree, ms->ctx_x, ms->ctx_y, ms->ctx_w, ms->ctx_h, ctx_bg);
			draw_border(ms->tree, ms->ctx_x, ms->ctx_y, ms->ctx_w, ms->ctx_h, 1, ctx_border);

			for (int j = 0; j < visible; j++) {
				int ri = j + ms->ctx_scroll_offset;
				if (ri >= res_count) break;
				int iy = ms->ctx_y + 2 + j * item_h;
				int is_current = (resolutions[ri].w == ce->width &&
						  resolutions[ri].h == ce->height);
				int is_max = (ri == 0); /* first after sort = highest */

				if (ms->ctx_hover_item == j)
					drawrect(ms->tree, ms->ctx_x + 1, iy, ms->ctx_w - 2, item_h, ctx_hover_bg);

				char buf[80];
				if (is_max)
					snprintf(buf, sizeof(buf), "%s%dx%d (max)",
						is_current ? "> " : "  ",
						resolutions[ri].w, resolutions[ri].h);
				else
					snprintf(buf, sizeof(buf), "%s%dx%d",
						is_current ? "> " : "  ",
						resolutions[ri].w, resolutions[ri].h);

				render_text_at(ms->tree, buf,
					ms->ctx_x + 6,
					iy + (item_h - statusfont.height) / 2 + statusfont.ascent,
					is_current ? ctx_check_col : text_col);
			}
		} else if (ms->ctx_submenu == 2 && ms->ctx_entry_idx >= 0 &&
			   ms->ctx_entry_idx < ms->entry_count) {
			/* Refresh rate submenu — show rates for current resolution */
			SetupMonitorEntry *ce = &ms->entries[ms->ctx_entry_idx];
			int rates[MAX_SETUP_MODES];
			int rate_count = 0;

			for (int j = 0; j < ce->mode_count; j++) {
				if (ce->modes[j].width == ce->width &&
				    ce->modes[j].height == ce->height) {
					/* Check duplicate */
					int dup = 0;
					for (int k = 0; k < rate_count; k++) {
						if (abs(rates[k] - ce->modes[j].refresh_mhz) < 500) {
							dup = 1;
							break;
						}
					}
					if (!dup && rate_count < MAX_SETUP_MODES)
						rates[rate_count++] = ce->modes[j].refresh_mhz;
				}
			}

			/* Sort descending */
			for (int a = 0; a < rate_count - 1; a++) {
				for (int b = 0; b < rate_count - 1 - a; b++) {
					if (rates[b] < rates[b + 1]) {
						int t = rates[b]; rates[b] = rates[b + 1]; rates[b + 1] = t;
					}
				}
			}

			int item_h = statusfont.height + 10;
			int max_visible = 10;
			int visible = rate_count < max_visible ? rate_count : max_visible;
			ms->ctx_w = text_width("> 999.99 Hz (max)") + 30;
			ms->ctx_h = visible * item_h + 4;

			if (ms->ctx_x + ms->ctx_w > ms->width - 4)
				ms->ctx_x = ms->width - 4 - ms->ctx_w;
			if (ms->ctx_y + ms->ctx_h > ms->height - 4)
				ms->ctx_y = ms->height - 4 - ms->ctx_h;

			drawrect(ms->tree, ms->ctx_x, ms->ctx_y, ms->ctx_w, ms->ctx_h, ctx_bg);
			draw_border(ms->tree, ms->ctx_x, ms->ctx_y, ms->ctx_w, ms->ctx_h, 1, ctx_border);

			for (int j = 0; j < visible; j++) {
				int ri = j + ms->ctx_scroll_offset;
				if (ri >= rate_count) break;
				int iy = ms->ctx_y + 2 + j * item_h;
				float hz = (float)rates[ri] / 1000.0f;
				int is_current = (fabsf(hz - ce->refresh) < 1.0f);
				int is_max = (ri == 0);

				if (ms->ctx_hover_item == j)
					drawrect(ms->tree, ms->ctx_x + 1, iy, ms->ctx_w - 2, item_h, ctx_hover_bg);

				char buf[64];
				if (is_max)
					snprintf(buf, sizeof(buf), "%s%.2f Hz (max)",
						is_current ? "> " : "  ", hz);
				else
					snprintf(buf, sizeof(buf), "%s%.2f Hz",
						is_current ? "> " : "  ", hz);

				render_text_at(ms->tree, buf,
					ms->ctx_x + 6,
					iy + (item_h - statusfont.height) / 2 + statusfont.ascent,
					is_current ? ctx_check_col : text_col);
			}
		}
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

/* ── helper: find entry at grid position ───────────────────────────── */

static int
entry_at_grid(MonitorSetup *ms, int col, int row)
{
	for (int i = 0; i < ms->entry_count; i++) {
		if (ms->entries[i].grid_col == col && ms->entries[i].grid_row == row)
			return i;
	}
	return -1;
}

/* ── helper: check if grid cell is adjacent to any occupied cell ──── */

static int
cell_adjacent_to_occupied(MonitorSetup *ms, int col, int row)
{
	static const int dx[] = {-1, 1, 0, 0};
	static const int dy[] = {0, 0, -1, 1};

	for (int d = 0; d < 4; d++) {
		if (entry_at_grid(ms, col + dx[d], row + dy[d]) >= 0)
			return 1;
	}
	return 0;
}

/* ── helpers excluding dragged entry ──────────────────────────────── */

static int
entry_at_grid_excl(MonitorSetup *ms, int col, int row, int excl_idx)
{
	for (int i = 0; i < ms->entry_count; i++) {
		if (i == excl_idx)
			continue;
		if (ms->entries[i].grid_col == col && ms->entries[i].grid_row == row)
			return i;
	}
	return -1;
}

static int
cell_adjacent_to_occupied_excl(MonitorSetup *ms, int col, int row, int excl_idx)
{
	static const int dx[] = {-1, 1, 0, 0};
	static const int dy[] = {0, 0, -1, 1};

	for (int d = 0; d < 4; d++) {
		if (entry_at_grid_excl(ms, col + dx[d], row + dy[d], excl_idx) >= 0)
			return 1;
	}
	return 0;
}

/* ── drop indicator ───────────────────────────────────────────────── */

static void
update_drop_indicator(MonitorSetup *ms)
{
	int spacing = 20;
	int cell_step_x = ms->cell_w + spacing;
	int cell_step_y = ms->cell_h + spacing;
	float ind_color[4] = {0.3f, 0.5f, 0.8f, 0.25f};
	float ind_between[4] = {0.4f, 0.7f, 1.0f, 0.5f};

	if (!ms->drop_indicator || ms->dragging < 0)
		return;

	clear_tree_children(ms->drop_indicator, NULL);

	if (ms->drag_insert_dir >= 0) {
		/* Between insertion: thin bar in the gap */
		if (ms->drag_insert_dir == 1) {
			/* Vertical gap between rows */
			wlr_scene_node_set_position(&ms->drop_indicator->node,
				ms->grid_origin_x + ms->drag_target_col * cell_step_x,
				ms->grid_origin_y + ms->drag_insert_after * cell_step_y + ms->cell_h);
			drawroundedrect(ms->drop_indicator, 0, 0,
				ms->cell_w, spacing, ind_between);
		} else {
			/* Horizontal gap between columns */
			wlr_scene_node_set_position(&ms->drop_indicator->node,
				ms->grid_origin_x + ms->drag_insert_after * cell_step_x + ms->cell_w,
				ms->grid_origin_y + ms->drag_target_row * cell_step_y);
			drawroundedrect(ms->drop_indicator, 0, 0,
				spacing, ms->cell_h, ind_between);
		}
	} else {
		/* Regular cell indicator */
		wlr_scene_node_set_position(&ms->drop_indicator->node,
			ms->grid_origin_x + ms->drag_target_col * cell_step_x,
			ms->grid_origin_y + ms->drag_target_row * cell_step_y);
		drawroundedrect(ms->drop_indicator, 0, 0,
			ms->cell_w, ms->cell_h, ind_color);
	}

	wlr_scene_node_set_enabled(&ms->drop_indicator->node, 1);
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

	/* Update context menu hover (only when not dragging) */
	if (ms->ctx_visible && ms->dragging < 0) {
		int item_h = statusfont.height + (ms->ctx_submenu == 0 ? 12 : 10);
		int old_hover = ms->ctx_hover_item;
		if (lx >= ms->ctx_x && lx < ms->ctx_x + ms->ctx_w &&
		    ly >= ms->ctx_y && ly < ms->ctx_y + ms->ctx_h) {
			ms->ctx_hover_item = (ly - ms->ctx_y - 2) / item_h;
		} else {
			ms->ctx_hover_item = -1;
		}
		if (ms->ctx_hover_item != old_hover)
			monitor_setup_render(m);
	}

	if (ms->dragging < 0 || ms->dragging >= ms->entry_count)
		return;

	{
		SetupMonitorEntry *e = &ms->entries[ms->dragging];
		int di = ms->dragging;
		int spacing = 20;
		int cell_step_x = ms->cell_w + spacing;
		int cell_step_y = ms->cell_h + spacing;
		float drag_cx, drag_cy, rel_x, rel_y, in_cell_x, in_cell_y;
		int slot_col, slot_row;
		int new_target_col, new_target_row, new_insert_dir, new_insert_after;
		int found;

		/* Follow cursor — instant, no scene rebuild */
		e->anim_x = (float)(lx - ms->drag_offset_x);
		e->anim_y = (float)(ly - ms->drag_offset_y);
		e->target_x = e->anim_x;
		e->target_y = e->anim_y;
		if (e->box_tree)
			wlr_scene_node_set_position(&e->box_tree->node,
				(int)e->anim_x, (int)e->anim_y);

		/* Cursor center in popup-local coords */
		drag_cx = e->anim_x + e->anim_w / 2.0f;
		drag_cy = e->anim_y + e->anim_h / 2.0f;

		/* Grid-relative position */
		rel_x = drag_cx - (float)ms->grid_origin_x;
		rel_y = drag_cy - (float)ms->grid_origin_y;

		/* Which cell step are we in? */
		slot_col = (int)floorf(rel_x / (float)cell_step_x);
		slot_row = (int)floorf(rel_y / (float)cell_step_y);
		in_cell_x = rel_x - slot_col * (float)cell_step_x;
		in_cell_y = rel_y - slot_row * (float)cell_step_y;

		new_target_col = ms->drag_target_col;
		new_target_row = ms->drag_target_row;
		new_insert_dir = -1;
		new_insert_after = -1;
		found = 0;

		/* --- 1. Between insertion (expanded detection zone) --- */
		{
			/* Extend gap hit zone into adjacent cells by 18% of cell size */
			float extend_y = ms->cell_h * 0.18f;
			float extend_x = ms->cell_w * 0.18f;
			float v_thresh = (float)spacing * 0.5f + extend_y;
			float h_thresh = (float)spacing * 0.5f + extend_x;

			/* Distance from cursor to each adjacent gap midline */
			float dv_below = fabsf(in_cell_y -
				((float)ms->cell_h + (float)spacing * 0.5f));
			float dv_above = in_cell_y + (float)spacing * 0.5f;
			float dh_right = fabsf(in_cell_x -
				((float)ms->cell_w + (float)spacing * 0.5f));
			float dh_left  = in_cell_x + (float)spacing * 0.5f;

			int near_any_v = (dv_below < v_thresh) ||
					 (dv_above < v_thresh);
			int near_any_h = (dh_right < h_thresh) ||
					 (dh_left < h_thresh);

			/* Vertical gap below: between slot_row and slot_row+1 */
			if (dv_below < v_thresh && !near_any_h) {
				int top = entry_at_grid_excl(ms, slot_col,
						slot_row, di);
				int bot = entry_at_grid_excl(ms, slot_col,
						slot_row + 1, di);
				if (top >= 0 && bot >= 0) {
					new_target_col = slot_col;
					new_target_row = slot_row + 1;
					new_insert_dir = 1;
					new_insert_after = slot_row;
					found = 1;
				}
			}

			/* Vertical gap above: between slot_row-1 and slot_row */
			if (!found && dv_above < v_thresh && !near_any_h) {
				int top = entry_at_grid_excl(ms, slot_col,
						slot_row - 1, di);
				int bot = entry_at_grid_excl(ms, slot_col,
						slot_row, di);
				if (top >= 0 && bot >= 0) {
					new_target_col = slot_col;
					new_target_row = slot_row;
					new_insert_dir = 1;
					new_insert_after = slot_row - 1;
					found = 1;
				}
			}

			/* Horizontal gap right: between slot_col and slot_col+1 */
			if (!found && dh_right < h_thresh && !near_any_v) {
				int left = entry_at_grid_excl(ms, slot_col,
						slot_row, di);
				int right = entry_at_grid_excl(ms, slot_col + 1,
						slot_row, di);
				if (left >= 0 && right >= 0) {
					new_target_col = slot_col + 1;
					new_target_row = slot_row;
					new_insert_dir = 0;
					new_insert_after = slot_col;
					found = 1;
				}
			}

			/* Horizontal gap left: between slot_col-1 and slot_col */
			if (!found && dh_left < h_thresh && !near_any_v) {
				int left = entry_at_grid_excl(ms, slot_col - 1,
						slot_row, di);
				int right = entry_at_grid_excl(ms, slot_col,
						slot_row, di);
				if (left >= 0 && right >= 0) {
					new_target_col = slot_col;
					new_target_row = slot_row;
					new_insert_dir = 0;
					new_insert_after = slot_col - 1;
					found = 1;
				}
			}
		}

		/* --- 2. Snap to nearest valid empty adjacent cell --- */
		if (!found) {
			int hover_col = (int)roundf(rel_x / (float)cell_step_x);
			int hover_row = (int)roundf(rel_y / (float)cell_step_y);
			int occ = entry_at_grid_excl(ms, hover_col, hover_row, di);

			if (occ >= 0) {
				/* Over an occupied cell: snap to adjacent empty cell
				 * Priority based on approach direction */
				float cell_cx = (float)ms->grid_origin_x +
					hover_col * cell_step_x + ms->cell_w / 2.0f;
				float cell_cy = (float)ms->grid_origin_y +
					hover_row * cell_step_y + ms->cell_h / 2.0f;
				float ddx = drag_cx - cell_cx;
				float ddy = drag_cy - cell_cy;
				int dc[4], dr[4];

				if (fabsf(ddy) >= fabsf(ddx)) {
					if (ddy <= 0) {
						/* Approach from above */
						dc[0]=0;  dr[0]=-1;
						dc[1]=-1; dr[1]=0;
						dc[2]=1;  dr[2]=0;
						dc[3]=0;  dr[3]=1;
					} else {
						/* Approach from below */
						dc[0]=0;  dr[0]=1;
						dc[1]=-1; dr[1]=0;
						dc[2]=1;  dr[2]=0;
						dc[3]=0;  dr[3]=-1;
					}
				} else {
					if (ddx <= 0) {
						/* Approach from left */
						dc[0]=-1; dr[0]=0;
						dc[1]=0;  dr[1]=-1;
						dc[2]=0;  dr[2]=1;
						dc[3]=1;  dr[3]=0;
					} else {
						/* Approach from right */
						dc[0]=1;  dr[0]=0;
						dc[1]=0;  dr[1]=-1;
						dc[2]=0;  dr[2]=1;
						dc[3]=-1; dr[3]=0;
					}
				}

				for (int d = 0; d < 4; d++) {
					int tc = hover_col + dc[d];
					int tr = hover_row + dr[d];
					if (entry_at_grid_excl(ms, tc, tr, di) < 0) {
						new_target_col = tc;
						new_target_row = tr;
						found = 1;
						break;
					}
				}
			} else if (cell_adjacent_to_occupied_excl(ms, hover_col,
					hover_row, di)) {
				/* Empty cell adjacent to occupied: valid target */
				new_target_col = hover_col;
				new_target_row = hover_row;
				found = 1;
			}
		}

		/* --- 3. Fallback: nearest valid cell by distance --- */
		if (!found) {
			float best_dist = 1e18f;
			for (int r = -1; r <= ms->grid_rows; r++) {
				for (int c = -1; c <= ms->grid_cols; c++) {
					float cx, cy, dx, dy, dist;
					if (entry_at_grid_excl(ms, c, r, di) >= 0)
						continue;
					if (!cell_adjacent_to_occupied_excl(ms, c, r, di))
						continue;
					cx = (float)ms->grid_origin_x +
						c * cell_step_x + ms->cell_w / 2.0f;
					cy = (float)ms->grid_origin_y +
						r * cell_step_y + ms->cell_h / 2.0f;
					dx = drag_cx - cx;
					dy = drag_cy - cy;
					dist = dx * dx + dy * dy;
					if (dist < best_dist) {
						best_dist = dist;
						new_target_col = c;
						new_target_row = r;
						found = 1;
					}
				}
			}
		}

		/* Update state if target changed */
		if (found && (new_target_col != ms->drag_target_col ||
			      new_target_row != ms->drag_target_row ||
			      new_insert_dir != ms->drag_insert_dir)) {
			ms->drag_target_col = new_target_col;
			ms->drag_target_row = new_target_row;
			ms->drag_insert_dir = new_insert_dir;
			ms->drag_insert_after = new_insert_after;
			update_drop_indicator(ms);
		}
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
			int di = ms->dragging;
			SetupMonitorEntry *de = &ms->entries[di];

			/* Restore all non-dragged entries to saved positions */
			for (int i = 0; i < ms->entry_count; i++) {
				if (i == di)
					continue;
				ms->entries[i].grid_col = ms->drag_saved_col[i];
				ms->entries[i].grid_row = ms->drag_saved_row[i];
			}

			/* Handle between insertion (shift boxes to make room) */
			if (ms->drag_insert_dir >= 0) {
				if (ms->drag_insert_dir == 1) {
					/* Vertical: shift rows > insert_after */
					for (int i = 0; i < ms->entry_count; i++) {
						if (i == di)
							continue;
						if (ms->entries[i].grid_row > ms->drag_insert_after)
							ms->entries[i].grid_row++;
					}
				} else {
					/* Horizontal: shift cols > insert_after */
					for (int i = 0; i < ms->entry_count; i++) {
						if (i == di)
							continue;
						if (ms->entries[i].grid_col > ms->drag_insert_after)
							ms->entries[i].grid_col++;
					}
				}
			}

			/* Set dragged entry to target position */
			de->grid_col = ms->drag_target_col;
			de->grid_row = ms->drag_target_row;

			/* Clean up drag state */
			ms->dragging = -1;
			ms->drag_insert_dir = -1;

			/* Hide drop indicator */
			if (ms->drop_indicator) {
				wlr_scene_node_set_enabled(&ms->drop_indicator->node, 0);
			}

			/* Compact grid and recompute layout */
			compact_grid(ms);
			compute_box_layout(ms, ms->width, ms->height);

			/* Re-render all box content and animate */
			for (int i = 0; i < ms->entry_count; i++) {
				render_box_content(ms, i);
				if (ms->entries[i].box_tree)
					wlr_scene_node_set_position(&ms->entries[i].box_tree->node,
						(int)ms->entries[i].anim_x, (int)ms->entries[i].anim_y);
			}
			update_onscreen_labels(ms);
			start_animation(m);
		}
		return 1;
	}

	/* Mouse down */
	if (button == BTN_LEFT) {
		/* Handle context menu clicks */
		if (ms->ctx_visible) {
			if (lx >= ms->ctx_x && lx < ms->ctx_x + ms->ctx_w &&
			    ly >= ms->ctx_y && ly < ms->ctx_y + ms->ctx_h) {
				int ci = ms->ctx_entry_idx;
				if (ci >= 0 && ci < ms->entry_count) {
					SetupMonitorEntry *ce = &ms->entries[ci];

					if (ms->ctx_submenu == 0) {
						/* Main menu */
						int item_h = statusfont.height + 12;
						int clicked = (ly - ms->ctx_y - 2) / item_h;

						if (clicked == 0) {
							/* Rotate */
							if (ce->transform == WL_OUTPUT_TRANSFORM_90 ||
							    ce->transform == WL_OUTPUT_TRANSFORM_270) {
								ce->transform = WL_OUTPUT_TRANSFORM_NORMAL;
								ce->target_rot = 0.0f;
							} else {
								ce->transform = WL_OUTPUT_TRANSFORM_90;
								ce->target_rot = 90.0f;
							}
							compute_box_layout(ms, ms->width, ms->height);
							start_animation(m);
							ms->ctx_visible = 0;
						} else if (clicked == 1) {
							/* Resolution submenu */
							ms->ctx_submenu = 1;
							ms->ctx_scroll_offset = 0;
							ms->ctx_hover_item = -1;
						} else if (clicked == 2) {
							/* Refresh rate submenu */
							ms->ctx_submenu = 2;
							ms->ctx_scroll_offset = 0;
							ms->ctx_hover_item = -1;
						}
					} else if (ms->ctx_submenu == 1) {
						/* Resolution submenu click */
						int item_h = statusfont.height + 10;
						int clicked = (ly - ms->ctx_y - 2) / item_h + ms->ctx_scroll_offset;

						/* Collect unique resolutions (same as render) */
						struct { int w, h; } resolutions[MAX_SETUP_MODES];
						int res_count = 0;
						for (int j = 0; j < ce->mode_count; j++) {
							int found = 0;
							for (int k = 0; k < res_count; k++) {
								if (resolutions[k].w == ce->modes[j].width &&
								    resolutions[k].h == ce->modes[j].height) {
									found = 1;
									break;
								}
							}
							if (!found && res_count < MAX_SETUP_MODES) {
								resolutions[res_count].w = ce->modes[j].width;
								resolutions[res_count].h = ce->modes[j].height;
								res_count++;
							}
						}
						/* Sort descending */
						for (int a = 0; a < res_count - 1; a++) {
							for (int b = 0; b < res_count - 1 - a; b++) {
								long pa = (long)resolutions[b].w * resolutions[b].h;
								long pb = (long)resolutions[b + 1].w * resolutions[b + 1].h;
								if (pa < pb) {
									int tw = resolutions[b].w, th = resolutions[b].h;
									resolutions[b] = resolutions[b + 1];
									resolutions[b + 1].w = tw;
									resolutions[b + 1].h = th;
								}
							}
						}

						if (clicked >= 0 && clicked < res_count) {
							ce->width = resolutions[clicked].w;
							ce->height = resolutions[clicked].h;
							/* Reset refresh to highest for this resolution */
							int best_refresh = 0;
							for (int j = 0; j < ce->mode_count; j++) {
								if (ce->modes[j].width == ce->width &&
								    ce->modes[j].height == ce->height &&
								    ce->modes[j].refresh_mhz > best_refresh)
									best_refresh = ce->modes[j].refresh_mhz;
							}
							if (best_refresh > 0)
								ce->refresh = (float)best_refresh / 1000.0f;
						}
						ms->ctx_visible = 0;
					} else if (ms->ctx_submenu == 2) {
						/* Refresh rate submenu click */
						int item_h = statusfont.height + 10;
						int clicked = (ly - ms->ctx_y - 2) / item_h + ms->ctx_scroll_offset;

						/* Collect rates for current resolution */
						int rates[MAX_SETUP_MODES];
						int rate_count = 0;
						for (int j = 0; j < ce->mode_count; j++) {
							if (ce->modes[j].width == ce->width &&
							    ce->modes[j].height == ce->height) {
								int dup = 0;
								for (int k = 0; k < rate_count; k++) {
									if (abs(rates[k] - ce->modes[j].refresh_mhz) < 500) {
										dup = 1;
										break;
									}
								}
								if (!dup && rate_count < MAX_SETUP_MODES)
									rates[rate_count++] = ce->modes[j].refresh_mhz;
							}
						}
						/* Sort descending */
						for (int a = 0; a < rate_count - 1; a++) {
							for (int b = 0; b < rate_count - 1 - a; b++) {
								if (rates[b] < rates[b + 1]) {
									int t = rates[b]; rates[b] = rates[b + 1]; rates[b + 1] = t;
								}
							}
						}

						if (clicked >= 0 && clicked < rate_count)
							ce->refresh = (float)rates[clicked] / 1000.0f;
						ms->ctx_visible = 0;
					}
				}
			} else {
				/* Click outside context menu — close it */
				ms->ctx_visible = 0;
			}
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
			ms->drag_orig_col = ms->entries[idx].grid_col;
			ms->drag_orig_row = ms->entries[idx].grid_row;
			ms->drag_target_col = ms->entries[idx].grid_col;
			ms->drag_target_row = ms->entries[idx].grid_row;
			ms->drag_insert_dir = -1;
			ms->drag_insert_after = -1;
			/* Save all grid positions at drag start */
			for (int k = 0; k < ms->entry_count; k++) {
				ms->drag_saved_col[k] = ms->entries[k].grid_col;
				ms->drag_saved_row[k] = ms->entries[k].grid_row;
			}
			/* Create drop indicator */
			if (!ms->drop_indicator)
				ms->drop_indicator = wlr_scene_tree_create(ms->tree);
			if (ms->drop_indicator) {
				wlr_scene_node_set_enabled(&ms->drop_indicator->node, 1);
				update_drop_indicator(ms);
			}
			/* Close context menu if open */
			ms->ctx_visible = 0;
			/* Re-render with drag border color and raise to top */
			render_box_content(ms, idx);
			if (ms->entries[idx].box_tree)
				wlr_scene_node_raise_to_top(&ms->entries[idx].box_tree->node);
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
			ms->ctx_submenu = 0;
			ms->ctx_scroll_offset = 0;
			ms->ctx_hover_item = -1;
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
			if (m->monitor_setup.ctx_submenu > 0) {
				/* Go back to main context menu */
				m->monitor_setup.ctx_submenu = 0;
				m->monitor_setup.ctx_hover_item = -1;
			} else {
				m->monitor_setup.ctx_visible = 0;
			}
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

	for (int i = 0; i < count; i++) {
		SetupMonitorEntry *e = &entries[i];
		fprintf(f, "monitor = %s grid=%d,%d %dx%d@%.0f",
			e->name, e->grid_col, e->grid_row,
			e->width, e->height, e->refresh);

		if (e->transform == WL_OUTPUT_TRANSFORM_90)
			fprintf(f, " transform=rotate-90");
		else if (e->transform == WL_OUTPUT_TRANSFORM_270)
			fprintf(f, " transform=rotate-270");

		fprintf(f, "\n");
	}

	fclose(f);
	wlr_log(WLR_INFO, "Wrote monitors.conf with %d monitors", count);
}

/* ── apply ─────────────────────────────────────────────────────────── */

void
monitor_setup_apply(Monitor *m)
{
	MonitorSetup *ms;

	if (!m)
		return;

	ms = &m->monitor_setup;

	/* Compact grid before saving/applying */
	compact_grid(ms);

	/* Write config file */
	write_monitors_conf(ms->entries, ms->entry_count);

	/* Apply immediately: reload config into runtime_monitors */
	runtime_monitor_count = 0;
	monitor_master_set = 0;
	memset(runtime_monitors, 0, sizeof(runtime_monitors));
	load_monitors_conf();

	/* Convert 2D grid to pixel coordinates */
	int max_col = 0, max_row = 0;
	for (int i = 0; i < ms->entry_count; i++) {
		if (ms->entries[i].grid_col > max_col)
			max_col = ms->entries[i].grid_col;
		if (ms->entries[i].grid_row > max_row)
			max_row = ms->entries[i].grid_row;
	}
	int grid_cols = max_col + 1;
	int grid_rows = max_row + 1;

	/* Compute column widths and row heights based on actual monitor sizes */
	int col_width[MAX_SETUP_MONITORS] = {0};
	int row_height[MAX_SETUP_MONITORS] = {0};

	for (int i = 0; i < ms->entry_count; i++) {
		SetupMonitorEntry *e = &ms->entries[i];
		int ew, eh;
		if (e->transform == WL_OUTPUT_TRANSFORM_90 ||
		    e->transform == WL_OUTPUT_TRANSFORM_270) {
			ew = e->height;
			eh = e->width;
		} else {
			ew = e->width;
			eh = e->height;
		}
		if (ew > col_width[e->grid_col])
			col_width[e->grid_col] = ew;
		if (eh > row_height[e->grid_row])
			row_height[e->grid_row] = eh;
	}

	/* Compute pixel offsets */
	int pixel_x[MAX_SETUP_MONITORS] = {0};
	int pixel_y[MAX_SETUP_MONITORS] = {0};
	for (int c = 1; c < grid_cols; c++)
		pixel_x[c] = pixel_x[c - 1] + col_width[c - 1];
	for (int r = 1; r < grid_rows; r++)
		pixel_y[r] = pixel_y[r - 1] + row_height[r - 1];

	/* Apply mode, transform, and position to each monitor */
	for (int i = 0; i < ms->entry_count; i++) {
		SetupMonitorEntry *e = &ms->entries[i];
		Monitor *om;

		wl_list_for_each(om, &mons, link) {
			if (strcmp(om->wlr_output->name, e->name) != 0)
				continue;

			/* Apply resolution/refresh + transform in a single commit */
			{
				struct wlr_output_state ostate;
				wlr_output_state_init(&ostate);

				/* Set mode (resolution + refresh) */
				struct wlr_output_mode *mode = find_mode(
					om->wlr_output, e->width, e->height, e->refresh);
				if (mode)
					wlr_output_state_set_mode(&ostate, mode);

				/* Set transform */
				wlr_output_state_set_transform(&ostate, e->transform);

				if (!wlr_output_commit_state(om->wlr_output, &ostate))
					wlr_log(WLR_ERROR, "Failed to apply state for %s",
						om->wlr_output->name);
				wlr_output_state_finish(&ostate);
			}

			/* Position in layout */
			wlr_output_layout_add(output_layout, om->wlr_output,
				pixel_x[e->grid_col], pixel_y[e->grid_row]);
			break;
		}
	}

	updatemons(NULL, NULL);
	monitor_setup_hide(m);
}
