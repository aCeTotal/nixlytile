#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "nixlytile.h"
#include "client.h"

/* Apps that default to occupying 2 default-tile slots when first
 * attached to a workspace.  Matched case-insensitively as substrings
 * against client appid+title.  Override with explicit Mod+R preset. */
static const char *const wide_app_tokens[] = {
	"blender",
	"firefox",
	"librewolf",
	"chromium",
	"chrome",
	"google-chrome",
	"brave",
	"vivaldi",
	"opera",
	"libreoffice",
	"soffice",
	"onlyoffice",
	"wpsoffice",
	"wps-office",
	"freeoffice",
	"writer",
	"calc",
	"impress",
	"draw",
};

static int
ci_substr(const char *hay, const char *needle)
{
	size_t nlen, i;
	if (!hay || !needle)
		return 0;
	nlen = strlen(needle);
	if (nlen == 0)
		return 1;
	for (i = 0; hay[i]; i++) {
		size_t j;
		for (j = 0; j < nlen; j++) {
			if (!hay[i + j])
				return 0;
			if (tolower((unsigned char)hay[i + j])
					!= tolower((unsigned char)needle[j]))
				break;
		}
		if (j == nlen)
			return 1;
	}
	return 0;
}

static int
client_wide_tile_count(Client *c)
{
	const char *appid, *title;
	size_t i;

	if (!c)
		return 1;
	appid = client_get_appid(c);
	title = client_get_title(c);
	for (i = 0; i < sizeof(wide_app_tokens) / sizeof(wide_app_tokens[0]); i++) {
		const char *tok = wide_app_tokens[i];
		if (ci_substr(appid, tok) || ci_substr(title, tok))
			return 2;
	}
	return 1;
}

/*
 * Niri-style workspace + column primitives.
 *
 * Lifecycle (phase 2):
 *   - Each Monitor has a wl_list of Workspaces (vertical stack).
 *   - Each Workspace has a wl_list of Columns (horizontal row).
 *   - Each Column has a wl_list of Clients (vertical stack within column).
 *   - One scene_tree per workspace; columns + clients render under it.
 *
 * NOTE: layout, scrolling, and animations are not wired in yet — those
 * arrive in phase 3 + 4.  This file only owns the data structures and
 * lifecycle hooks.  Legacy dwl tag/seltags state still drives visibility.
 */

Workspace *
workspace_create(Monitor *m)
{
	Workspace *ws;

	if (!m)
		return NULL;

	ws = ecalloc(1, sizeof(*ws));
	if (!ws)
		return NULL;

	{
		static uint64_t next_window_id = 1;
		ws->window_id = next_window_id++;
	}
	ws->mon = m;
	ws->idx = m->next_ws_id++;
	wl_list_init(&ws->columns);
	ws->n_columns = 0;
	ws->scroll_x = ws->target_scroll_x = 0;
	ws->focused_col = NULL;

	/* Scene tree parented under the tile layer.  In phase 3 this is
	 * where we attach Y-offsets for vertical switch animation. */
	if (layers[LyrTile])
		ws->scene = wlr_scene_tree_create(layers[LyrTile]);

	wl_list_insert(m->workspaces.prev, &ws->link);
	m->n_workspaces++;
	return ws;
}

void
workspace_destroy(Workspace *ws)
{
	Column *col, *coltmp;
	Client *c;

	if (!ws)
		return;

	/* A fullscreen client detached from its column still references this
	 * workspace via fs_ws — clear it so the dangling pointer can't be
	 * compared after free (it reverts to always-visible/unbound). */
	wl_list_for_each(c, &clients, link)
		if (c->fs_ws == ws)
			c->fs_ws = NULL;

	wl_list_for_each_safe(col, coltmp, &ws->columns, link)
		column_destroy(col);

	if (ws->scene)
		wlr_scene_node_destroy(&ws->scene->node);

	wl_list_remove(&ws->link);
	if (ws->mon) {
		ws->mon->n_workspaces--;
		if (ws->mon->active_ws == ws)
			ws->mon->active_ws = NULL;
		if (ws->mon->prev_ws == ws)
			ws->mon->prev_ws = NULL;
	}
	free(ws);
}

Column *
column_create(Workspace *ws)
{
	Column *col;

	if (!ws)
		return NULL;

	col = ecalloc(1, sizeof(*col));
	if (!col)
		return NULL;

	col->ws = ws;
	wl_list_init(&col->clients);
	col->n_clients = 0;
	col->x = col->y = 0;
	col->width = col->height = 0;
	col->target_x = col->target_y = 0;
	col->target_width = col->target_height = 0;
	col->width_idx = -1;
	col->wide_tiles = 1;
	col->width_px_override = 0;
	col->fullscreen = 0;
	col->x_f = 0.0;
	col->x_vel = 0.0;
	col->width_f = 0.0;
	col->width_vel = 0.0;
	col->just_created = 1;

	wl_list_insert(ws->columns.prev, &col->link);
	ws->n_columns++;
	return col;
}

void
column_destroy(Column *col)
{
	Client *c, *ctmp;

	if (!col)
		return;

	/* Detach any remaining clients (shouldn't normally happen — caller
	 * should remove clients before destroying the column). */
	wl_list_for_each_safe(c, ctmp, &col->clients, column_link) {
		wl_list_remove(&c->column_link);
		c->column = NULL;
	}

	/* When a focused column dies, move focus to its LEFT neighbor
	 * (Niri-style: closing a tile pulls the spotlight one column
	 * left).  If there's no left neighbor (focused was the leftmost),
	 * fall back to the right neighbor; if neither, NULL. */
	if (col->ws && col->ws->focused_col == col) {
		Column *neighbor = NULL;
		if (col->link.prev != &col->ws->columns)
			neighbor = wl_container_of(col->link.prev, neighbor, link);
		else if (col->link.next != &col->ws->columns)
			neighbor = wl_container_of(col->link.next, neighbor, link);
		col->ws->focused_col = neighbor;
	}

	wl_list_remove(&col->link);
	if (col->ws) {
		col->ws->n_columns--;
	}
	free(col);
}

void
column_add_client(Column *col, Client *c)
{
	if (!col || !c)
		return;

	if (c->column)
		column_remove_client(c);

	c->column = col;
	wl_list_insert(col->clients.prev, &c->column_link);
	col->n_clients++;
}

void
column_remove_client(Client *c)
{
	Column *col;

	if (!c || !c->column)
		return;

	col = c->column;
	wl_list_remove(&c->column_link);
	c->column = NULL;
	col->n_clients--;

	/* Empty columns auto-destruct (Niri behavior). */
	if (col->n_clients == 0)
		column_destroy(col);
}

void
monitor_init_workspaces(Monitor *m)
{
	if (!m)
		return;

	wl_list_init(&m->workspaces);
	m->active_ws = NULL;
	m->next_ws_id = 0;
	m->n_workspaces = 0;

	/* Bootstrap with one empty workspace so the monitor always has
	 * a current workspace.  Niri lazily creates the next one when the
	 * user navigates downward. */
	m->active_ws = workspace_create(m);
}

void
monitor_cleanup_workspaces(Monitor *m)
{
	Workspace *ws, *wstmp;

	if (!m)
		return;

	wl_list_for_each_safe(ws, wstmp, &m->workspaces, link)
		workspace_destroy(ws);

	m->active_ws = NULL;
	m->n_workspaces = 0;
}

/* ── attach/detach (phase 3) ─────────────────────────────────────────
 * Each newly-tiled client gets its own column appended to the right
 * of the active workspace.  Niri's "always one client per column"
 * default — the user can stack later via explicit move-into-column.
 */
void
workspace_attach_client(Workspace *ws, Client *c)
{
	Column *col;
	Column *focused;

	if (!ws || !c)
		return;
	if (c->column && c->column->ws == ws)
		return;
	if (c->column)
		column_remove_client(c);

	col = ecalloc(1, sizeof(*col));
	if (!col)
		return;
	col->ws = ws;
	wl_list_init(&col->clients);
	col->n_clients = 0;
	col->width_idx = -1;
	col->wide_tiles = client_wide_tile_count(c);
	col->width_px_override = 0;
	col->fullscreen = 0;
	col->x_f = 0.0;
	col->x_vel = 0.0;
	col->width_f = 0.0;
	col->width_vel = 0.0;
	col->just_created = 1;

	/* Insert immediately to the right of the currently-focused
	 * column.  Niri-style: spawning while looking at column N puts
	 * the new column at N+1 (between N and what was N+1).  When
	 * there's no focused column (empty ws or first spawn), append
	 * at the end. */
	focused = ws->focused_col;
	if (focused)
		wl_list_insert(&focused->link, &col->link);
	else
		wl_list_insert(ws->columns.prev, &col->link);
	ws->n_columns++;

	column_add_client(col, c);
	ws->focused_col = col;
}

void
workspace_detach_client(Client *c)
{
	column_remove_client(c);
}

/* Re-insert a (previously detached) client into the workspace as a new
 * column placed at the drop position.  Used by mouse drag-to-tile: the
 * client floated during drag; on release we slot it into the column row
 * at the cursor's horizontal position. */
void
workspace_drop_tile(Workspace *ws, Client *c, double screen_x)
{
	Monitor *m;
	Column *iter, *target = NULL, *col;
	double local_x;
	int insert_after = 1; /* default: append at end */

	if (!ws || !c || !ws->mon)
		return;
	m = ws->mon;

	local_x = screen_x - m->w.x + ws->target_scroll_x;

	wl_list_for_each(iter, &ws->columns, link) {
		if (iter == c->column)
			continue;
		if (local_x < iter->target_x) {
			target = iter;
			insert_after = 0;
			break;
		}
		if (local_x < iter->target_x + iter->target_width) {
			int mid = iter->target_x + iter->target_width / 2;
			target = iter;
			insert_after = (local_x >= mid) ? 1 : 0;
			break;
		}
	}

	if (c->column)
		column_remove_client(c);

	col = ecalloc(1, sizeof(*col));
	if (!col)
		return;
	col->ws = ws;
	wl_list_init(&col->clients);
	col->n_clients = 0;
	col->width_idx = -1;
	col->wide_tiles = client_wide_tile_count(c);
	col->width_px_override = 0;
	col->fullscreen = 0;
	col->x_f = 0.0;
	col->x_vel = 0.0;
	col->width_f = 0.0;
	col->width_vel = 0.0;
	col->just_created = 1;

	if (target && !insert_after)
		wl_list_insert(target->link.prev, &col->link);
	else if (target && insert_after)
		wl_list_insert(&target->link, &col->link);
	else
		wl_list_insert(ws->columns.prev, &col->link);
	ws->n_columns++;

	column_add_client(col, c);
	ws->focused_col = col;
}

/* When a client gains focus, point ws.focused_col to its owning column
 * so the camera (next workspace_layout call) can follow it. */
void
workspace_focus_client(Client *c)
{
	if (!c || !c->column)
		return;
	if (c->column->ws)
		c->column->ws->focused_col = c->column;
}

/* ── layout (phase 3, columns + camera; phase 4 added animation) ─────
 *
 * Algorithm:
 *   - Single column: fills the entire usable monitor width.
 *   - Multiple columns: each gets a default of half the monitor width.
 *     Total row width can exceed monitor width → camera scrolls.
 *   - Camera centers the focused column when possible, clamped to
 *     [0, total_width − mon.w.width].
 *   - Clients within a column stack vertically with equal heights.
 *
 * workspace_layout(): writes target_x/target_width on each column and
 *   target_scroll_x on the workspace.  Does NOT touch the scene graph.
 *
 * workspace_apply_positions(): reads the current (animated) values
 *   and writes them to client geometry / scene positions.  Called
 *   every frame from the rendermon path.
 */
/* Highest workspace index that currently holds at least one tile.
 * Used to cap forward navigation: the user can move into AT MOST one
 * trailing empty workspace, never two consecutive empties. */
static int
max_nonempty_ws_idx(Monitor *m)
{
	Workspace *ws;
	int max_idx = -1;
	if (!m)
		return -1;
	wl_list_for_each(ws, &m->workspaces, link) {
		if (ws->n_columns > 0 && ws->idx > max_idx)
			max_idx = ws->idx;
	}
	return max_idx;
}

/* Close index gaps left by emptied workspaces.  When a workspace
 * becomes empty but there's still a populated workspace below it,
 * destroy the empty one and shift everything below it up by one slot.
 * Always leaves at most one trailing empty workspace (Niri convention).
 *
 * If the empty workspace was the active one, the next workspace takes
 * its place — the user is never stranded on a workspace they can't
 * leave to reach tiles. */
void
monitor_compact_workspaces(Monitor *m)
{
	int changed = 1;

	if (!m)
		return;

	while (changed) {
		Workspace *ws, *other, *target;
		int gap_idx, was_active;

		changed = 0;
		wl_list_for_each(ws, &m->workspaces, link) {
			int has_filled_after = 0;

			if (ws->n_columns > 0)
				continue;
			wl_list_for_each(other, &m->workspaces, link) {
				if (other != ws && other->idx > ws->idx
						&& other->n_columns > 0) {
					has_filled_after = 1;
					break;
				}
			}
			if (!has_filled_after)
				continue;

			gap_idx = ws->idx;
			was_active = (m->active_ws == ws);
			target = NULL;
			if (was_active) {
				wl_list_for_each(other, &m->workspaces, link) {
					if (other != ws && other->idx > gap_idx) {
						if (!target || other->idx < target->idx)
							target = other;
					}
				}
			}
			if (m->prev_ws == ws)
				m->prev_ws = NULL;
			if (was_active)
				m->active_ws = target;

			workspace_destroy(ws);

			wl_list_for_each(other, &m->workspaces, link) {
				if (other->idx > gap_idx)
					other->idx--;
			}
			if (m->next_ws_id > 0)
				m->next_ws_id--;

			changed = 1;
			break;
		}
	}

	/* Trim extra trailing empties: Niri keeps at most ONE empty
	 * workspace after the last populated one.  When the last tile is
	 * removed from ws N, both ws N and the previously-synthesized
	 * trailing empty ws N+1 are now empty — collapse to a single
	 * trailing slot so waybar's dwl/tags doesn't keep showing the
	 * stale extra. */
	for (;;) {
		Workspace *ws, *last = NULL, *prev = NULL;
		wl_list_for_each(ws, &m->workspaces, link) {
			if (!last || ws->idx > last->idx) {
				prev = last;
				last = ws;
			} else if (!prev || ws->idx > prev->idx) {
				prev = ws;
			}
		}
		if (!last || !prev)
			break;
		if (last->n_columns != 0 || prev->n_columns != 0)
			break;
		if (m->active_ws == last)
			m->active_ws = prev;
		if (m->prev_ws == last)
			m->prev_ws = NULL;
		workspace_destroy(last);
		if (m->next_ws_id > 0)
			m->next_ws_id--;
	}
}

/* Niri preset_column_widths (proportional).  Matches user's Niri config:
 *   0.25, 0.333, 0.5, 0.667, 0.75, 1.0
 * Column.width_idx indexes this array.  -1 = aspect-based default (see
 * default_tiles_per_row() — 2/3/4 tiles fit exactly per row). */
const double preset_column_widths[] = {
	0.25, 0.333, 0.5, 0.667, 0.75, 1.0,
};
const int n_preset_column_widths =
	(int)(sizeof(preset_column_widths) / sizeof(preset_column_widths[0]));
const int default_column_width_idx = 2; /* 0.5 — used by Mod+R cycle */

/* Default tiles-per-row based on aspect ratio of the physical output:
 *   widescreen (≤2.0)        → 2 tiles per row (each ≈ 50%)
 *   ultrawide  (2.0 ≤ a <3.0)→ 3 tiles per row (each ≈ 33.3%)
 *   super     (≥3.0)         → 4 tiles per row (each ≈ 25%)
 * The exact width subtracts (N-1)*gap so N default tiles fit EXACTLY
 * inside m->w.width with the correct inter-tile gaps showing. */
static int
default_tiles_per_row(Monitor *m)
{
	double aspect;
	if (!m || m->m.height <= 0)
		return 2;
	aspect = (double)m->m.width / (double)m->m.height;
	if (aspect >= 3.0) return 4;
	if (aspect >= 2.0) return 3;
	return 2;
}

/* Target width in PIXELS for a column.  Three cases:
 *   - fullscreen toggle: full m->w.width (covers tile area).
 *   - user-chosen preset (Mod+R): literal proportion of m->w.width.
 *   - default (width_idx == -1): aspect-based fit width — N tiles
 *     filling the row exactly with (N-1) inter-tile gaps visible. */
static int
column_target_width_px(Column *col, int mon_w, int gap, int n_default)
{
	int tile_w, tiles;

	if (!col)
		return mon_w / 2;
	if (col->fullscreen)
		return mon_w;
	if (col->width_px_override > 0) {
		int w = col->width_px_override;
		if (w > mon_w) w = mon_w;
		if (w < 50) w = 50;
		return w;
	}
	if (col->width_idx >= 0 && col->width_idx < n_preset_column_widths)
		return (int)((double)mon_w *
				preset_column_widths[col->width_idx]);
	if (n_default < 1) n_default = 1;
	tile_w = (mon_w - (n_default - 1) * gap) / n_default;
	tiles = col->wide_tiles >= 1 ? col->wide_tiles : 1;
	if (tiles > n_default) tiles = n_default;
	return tiles * tile_w + (tiles - 1) * gap;
}

void
workspace_layout(Workspace *ws)
{
	Monitor *m;
	Column *col;
	int n, mon_w, mon_h, gap, total_w;
	int x_cursor;

	if (!ws || !ws->mon)
		return;

	m = ws->mon;
	if (!m->wlr_output->enabled)
		return;

	gap = m->gaps ? (int)gappx : 0;
	/* Use TARGET tile area for size calculations.  m->w may be
	 * mid-spring (waybar toggle) — using the lerped value here
	 * would snap col->target_width/height to an intermediate state,
	 * preventing the size spring from converging on the final
	 * value as m->w settles. */
	mon_w = m->w_initialized ? m->w_target.width : m->w.width;
	mon_h = m->w_initialized ? m->w_target.height : m->w.height;
	if (mon_w < 0) mon_w = 0;
	if (mon_h < 0) mon_h = 0;

	n = ws->n_columns;
	if (n == 0)
		return;

	/* Aspect-based default tile width: N default tiles fit EXACTLY
	 * in m->w.width with (N-1)*gap inter-tile gaps visible.  Per-col
	 * width: fullscreen → full, preset (Mod+R) → literal proportion,
	 * default → aspect fit. */
	{
		int n_default = default_tiles_per_row(m);
		x_cursor = 0;
		wl_list_for_each(col, &ws->columns, link) {
			int w = column_target_width_px(col, mon_w, gap,
					n_default);
			if (w < 1) w = 1;
			col->target_width = w;
			col->target_height = mon_h;
			col->target_x = x_cursor;
			col->target_y = 0;
			col->height = col->target_height;
			if (col->just_created) {
				col->x_f = (double)col->target_x;
				col->x_vel = 0.0;
				col->x = col->target_x;
				col->width_f = (double)col->target_width;
				col->width_vel = 0.0;
				col->width = col->target_width;
				col->just_created = 0;
			}
			x_cursor += col->target_width + gap;
		}
	}
	total_w = x_cursor - (n > 0 ? gap : 0);

	/* Niri-style "center-focused-column never": scroll-minimum.
	 * Only shift the camera if the focused column edge is outside
	 * the viewport — keep it pinned otherwise.  No centering. */
	if (total_w <= mon_w) {
		ws->target_scroll_x = 0;
	} else if (ws->focused_col) {
		Column *fc = ws->focused_col;
		int cur = ws->target_scroll_x;
		int left = fc->target_x;
		int right = fc->target_x + fc->target_width;
		if (left < cur)
			cur = left;
		else if (right > cur + mon_w)
			cur = right - mon_w;
		if (cur < 0) cur = 0;
		if (cur > total_w - mon_w) cur = total_w - mon_w;
		ws->target_scroll_x = cur;
	} else {
		if (ws->target_scroll_x < 0) ws->target_scroll_x = 0;
		if (ws->target_scroll_x > total_w - mon_w)
			ws->target_scroll_x = total_w - mon_w;
	}
}

/*
 * monitor_apply_positions: write live (animated) positions of all
 * clients on this monitor's workspaces.  Called every frame from the
 * rendermon path.  Cheap when nothing is animating — it's just
 * arithmetic + scene_node_set_position calls.
 *
 * Vertical layout: each workspace is placed at (ws.idx − active.idx)
 * * mon.h, plus the live ws_y_offset that decays toward 0 after a
 * switch.  Active workspace at y=0 once settled; others off-screen
 * above/below.
 *
 * Horizontal: only the active workspace uses scroll_x — non-active
 * workspaces show columns in their resting positions so they're ready
 * to render the moment they slide into view.
 */
/* dwl-style status output on stdout.  waybar's dwl/tags module reads
 * lines from its stdin and renders the workspace selector accordingly.
 *
 * Format (one event per line):
 *   <output> tags <occ> <selected> <focused> <urgent>
 *   <output> layout <symbol>
 *   <output> title <title>
 *   <output> appid <app_id>
 *   <output> selmon 0|1
 *   <output> fullscreen 0|1
 *   <output> floating 0|1
 *
 * For Niri-style workspaces we map workspace.idx → tag bit (1 << idx),
 * capped at 31 since tags is uint32.  The dwl module shows tags 1..9
 * by default — adjust waybar config to show more.
 */
void
printstatus(void)
{
	Monitor *m;
	Client *c;
	Workspace *ws;
	uint32_t occ, sel, urg;

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output)
			continue;
		occ = sel = urg = 0;

		wl_list_for_each(ws, &m->workspaces, link) {
			uint32_t bit;
			if (ws->idx >= 31)
				continue;
			bit = 1u << ws->idx;
			if (ws->n_columns > 0)
				occ |= bit;
		}
		if (m->active_ws && m->active_ws->idx < 31)
			sel = 1u << m->active_ws->idx;

		wl_list_for_each(c, &clients, link) {
			if (c->mon == m && c->isurgent && c->column &&
					c->column->ws && c->column->ws->idx < 31)
				urg |= 1u << c->column->ws->idx;
		}

		c = focustop(m);
		if (c) {
			printf("%s title %s\n", m->wlr_output->name,
				client_get_title(c) ? client_get_title(c) : "");
			printf("%s appid %s\n", m->wlr_output->name,
				client_get_appid(c) ? client_get_appid(c) : "");
			printf("%s fullscreen %d\n", m->wlr_output->name, c->isfullscreen);
			printf("%s floating %d\n", m->wlr_output->name, c->isfloating);
		} else {
			printf("%s title \n", m->wlr_output->name);
			printf("%s appid \n", m->wlr_output->name);
			printf("%s fullscreen 0\n", m->wlr_output->name);
			printf("%s floating 0\n", m->wlr_output->name);
		}
		printf("%s selmon %u\n", m->wlr_output->name,
			m == selmon ? 1 : 0);
		printf("%s tags %"PRIu32" %"PRIu32" %"PRIu32" %"PRIu32"\n",
			m->wlr_output->name, occ, sel, sel, urg);
		printf("%s layout %s\n", m->wlr_output->name,
			(m->active_ws && m->active_ws->focused_col &&
				m->active_ws->focused_col->fullscreen)
				? "[F]" : "[]=");
	}
	fflush(stdout);

	/* Niri waybar parity: emit zdwl_ipc events to bound clients. */
	dwl_ipc_publish();

	/* Niri-IPC subscribers (waybar niri/workspaces). */
	window_ipc_publish_workspaces();
	window_ipc_publish_workspace_activated();
}

/* Niri-style column-expand fullscreen: focused column takes the full
 * monitor width.  The client surface is reconfigured to that width
 * (so its content actually scales).  Other columns continue to exist
 * unchanged — Mod+H/L still cycles through them and Mod+J/K still
 * switches workspaces.  Camera animates to the new layout.
 *
 * No black artifacts because we never reparent or hide the client
 * during the transition — the existing surface buffer keeps rendering
 * at its old size until the client commits a new one at the target
 * size, while the camera scroll and column-width changes animate
 * through the normal monitor_apply_positions path.
 */
void
toggle_column_fullscreen(const Arg *arg)
{
	Client *c;
	Column *col;

	(void)arg;
	if (!selmon || !selmon->active_ws)
		return;

	c = focustop(selmon);
	if (!c || !c->column)
		return;

	col = c->column;
	col->fullscreen = !col->fullscreen;
	arrange(selmon);
}

/* Toggle waybar by process kill/spawn.
 *
 * Scan /proc for any waybar process.  If found → SIGKILL all
 * (compositor-side toggle off).  If none → fork+execlp waybar
 * (toggle on).  We match by substring on /proc/<pid>/comm so the
 * NixOS launcher name ".waybar-wrapped" is caught too.
 *
 * Layer-shell teardown on waybar exit reclaims its exclusive zone;
 * arrangelayers() updates m->w_target on the next layer event, and
 * the m->w_x_f / w_y_f / w_w_f / w_h_f springs animate tiles into
 * the freed area at SPRING_WINDOW stiffness (smooth reflow).
 *
 * Compositor inherits a full user PATH from SDDM/login, so execlp
 * resolves "waybar" via /etc/profiles/per-user/<u>/bin.
 */
void
togglewaybar(const Arg *arg)
{
	DIR *d;
	struct dirent *de;
	char path[64];
	char comm[64];
	FILE *f;
	size_t len;
	int killed = 0;

	(void)arg;
	d = opendir("/proc");
	if (!d)
		return;
	while ((de = readdir(d))) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		if (fgets(comm, sizeof(comm), f)) {
			len = strlen(comm);
			if (len && comm[len - 1] == '\n')
				comm[len - 1] = '\0';
			if (strstr(comm, "waybar")) {
				pid_t pid = (pid_t)atoi(de->d_name);
				if (pid > 0 && kill(pid, SIGKILL) == 0)
					killed = 1;
			}
		}
		fclose(f);
	}
	closedir(d);

	if (killed)
		return;

	/* No running waybar — spawn a new one detached. */
	if (fork() == 0) {
		setsid();
		execlp("waybar", "waybar", (char *)NULL);
		_exit(127);
	}
}

/* Niri: switch-preset-column-width — cycle the focused column through
 * the preset_column_widths array.  Wraps at the end. */
void
switch_preset_column_width(const Arg *arg)
{
	Column *col;
	(void)arg;
	if (!selmon || !selmon->active_ws)
		return;
	col = selmon->active_ws->focused_col;
	if (!col)
		return;
	col->fullscreen = 0;
	if (col->width_idx < 0)
		col->width_idx = default_column_width_idx;
	col->width_idx = (col->width_idx + 1) % n_preset_column_widths;
	arrange(selmon);
}

/* Resize focused column.  arg->i > 0 = grow, arg->i < 0 = shrink.
 *
 * Edge rules:
 *   • Edges that border a neighbour column move outward (grow) or
 *     inward (shrink) by `step`.
 *   • Edges at the screen border are LOCKED — they never move.
 *
 * Side-effects on neighbours:
 *   • Each neighbour adjacent to a moving edge gains or loses `step`
 *     so the row's total width is preserved.
 *
 * Examples:
 *   • Tile alone or fully maximised (no neighbours)      → no-op.
 *   • Tile bordering one neighbour and one screen edge  → inner edge
 *     moves; that single neighbour shrinks (grow) or grows (shrink)
 *     by `step`.  Focused width changes by ±step.
 *   • Tile with neighbours on both sides (middle column) → BOTH edges
 *     move outward (grow) or inward (shrink); each neighbour changes
 *     by ∓step.  Focused width changes by ±2·step.
 *
 * Minimum column width: 100 px.  If any participant would drop below
 * that, the whole resize is skipped (atomic). */
void
resize_column_dir(const Arg *arg)
{
	Column *col, *left_nbr = NULL, *right_nbr = NULL;
	int dir;
	int mon_w, gap, n_default, step;
	const int min_w = 100;
	int cur_w, new_w;
	int left_cur = 0, left_new = 0;
	int right_cur = 0, right_new = 0;
	int delta_total;

	if (!arg || !selmon || !selmon->active_ws)
		return;
	col = selmon->active_ws->focused_col;
	if (!col || !selmon->wlr_output->enabled)
		return;

	dir = arg->i >= 0 ? 1 : -1;

	mon_w = selmon->w_initialized ? selmon->w_target.width : selmon->w.width;
	if (mon_w < 1)
		mon_w = selmon->w.width;
	gap = selmon->gaps ? (int)gappx : 0;
	n_default = 2;
	if (selmon->m.height > 0) {
		double a = (double)selmon->m.width / (double)selmon->m.height;
		if (a >= 3.0) n_default = 4;
		else if (a >= 2.0) n_default = 3;
	}
	step = mon_w / 20;
	if (step < 40) step = 40;

	if (col->link.prev != &col->ws->columns)
		left_nbr = wl_container_of(col->link.prev, left_nbr, link);
	if (col->link.next != &col->ws->columns)
		right_nbr = wl_container_of(col->link.next, right_nbr, link);

	cur_w = column_target_width_px(col, mon_w, gap, n_default);

	/* Alone on the workspace: no neighbours to share with — just
	 * resize own width.  Right edge moves (column is left-anchored at
	 * x=0); grow adds `step`, shrink subtracts. */
	if (!left_nbr && !right_nbr) {
		int new_w_alone = cur_w + dir * step;
		if (new_w_alone < min_w)
			return;
		if (new_w_alone > mon_w)
			new_w_alone = mon_w;
		if (new_w_alone == cur_w)
			return;
		col->fullscreen = 0;
		col->width_px_override = new_w_alone;
		col->just_created = 1;
		arrange(selmon);
		if (!wl_list_empty(&col->clients)) {
			Client *fc = wl_container_of(col->clients.next, fc, column_link);
			warpcursor(fc);
		}
		return;
	}

	delta_total = 0;
	if (left_nbr) {
		left_cur = column_target_width_px(left_nbr, mon_w, gap, n_default);
		left_new = left_cur - dir * step;
		if (left_new < min_w)
			return;
		delta_total += step;
	}
	if (right_nbr) {
		right_cur = column_target_width_px(right_nbr, mon_w, gap, n_default);
		right_new = right_cur - dir * step;
		if (right_new < min_w)
			return;
		delta_total += step;
	}

	new_w = cur_w + dir * delta_total;
	if (new_w < min_w)
		return;
	if (new_w > mon_w)
		new_w = mon_w;

	col->fullscreen = 0;
	col->width_px_override = new_w;
	col->just_created = 1;
	if (left_nbr) {
		left_nbr->fullscreen = 0;
		left_nbr->width_px_override = left_new;
		left_nbr->just_created = 1;
	}
	if (right_nbr) {
		right_nbr->fullscreen = 0;
		right_nbr->width_px_override = right_new;
		right_nbr->just_created = 1;
	}

	arrange(selmon);

	/* Keep the cursor centred on the focused tile.  Without this, a
	 * sustained resize (held Mod+Shift+arrow) slides the tile boundary
	 * past the cursor, sloppy-focus on the next motion event flips
	 * focus to the neighbour, and the following key repeat resizes
	 * the WRONG column — the boundary bounces back.  Warping with the
	 * focused tile pins the pointer inside it for the whole repeat. */
	if (!wl_list_empty(&col->clients)) {
		Client *fc = wl_container_of(col->clients.next, fc, column_link);
		warpcursor(fc);
	}
}

/* Niri: maximize-column — focused column expands to full monitor width.
 * Re-press toggles back to its prior preset width. */
void
maximize_column(const Arg *arg)
{
	Column *col;
	(void)arg;
	if (!selmon || !selmon->active_ws)
		return;
	col = selmon->active_ws->focused_col;
	if (!col)
		return;
	col->fullscreen = !col->fullscreen;
	arrange(selmon);
}

/* Niri: center-column — set target_scroll_x so the focused column is
 * centered in the viewport. */
void
center_column(const Arg *arg)
{
	Workspace *ws;
	Column *col;
	int mon_w, total_w, x;
	Column *iter;
	(void)arg;
	if (!selmon || !selmon->active_ws)
		return;
	ws = selmon->active_ws;
	col = ws->focused_col;
	if (!col)
		return;
	mon_w = selmon->w.width;
	total_w = 0;
	wl_list_for_each(iter, &ws->columns, link)
		total_w += iter->target_width;
	if (total_w > 0)
		total_w += (ws->n_columns - 1) * (selmon->gaps ? (int)gappx : 0);
	x = col->target_x + col->target_width / 2 - mon_w / 2;
	if (x < 0) x = 0;
	if (total_w > mon_w && x > total_w - mon_w) x = total_w - mon_w;
	ws->target_scroll_x = x;
	arrange(selmon);
}

/* Niri: swap-window-left/right — swap focused column with its left/right
 * neighbor.  Identical to move_column_dir for single-window columns;
 * here we just delegate. */
void
swap_window_dir(const Arg *arg)
{
	move_column_dir(arg);
}

/* Niri: expel-window-from-column — pop the focused window out of its
 * column into a new column to its right. */
void
expel_window_from_column(const Arg *arg)
{
	Workspace *ws;
	Column *src, *dst;
	Client *c;
	(void)arg;
	if (!selmon || !selmon->active_ws)
		return;
	ws = selmon->active_ws;
	src = ws->focused_col;
	if (!src || src->n_clients < 2)
		return;
	c = focustop(selmon);
	if (!c || c->column != src)
		return;

	dst = ecalloc(1, sizeof(*dst));
	if (!dst)
		return;
	dst->ws = ws;
	wl_list_init(&dst->clients);
	dst->n_clients = 0;
	dst->width_idx = -1;
	wl_list_insert(&src->link, &dst->link);
	ws->n_columns++;

	column_remove_client(c);
	column_add_client(dst, c);
	ws->focused_col = dst;
	arrange(selmon);
}

/* Move focused window up/down within its column (or across columns). */
void
move_window_in_column_dir(const Arg *arg)
{
	Column *col;
	Client *c;
	struct wl_list *target_link;
	(void)arg;
	if (!arg || !selmon || !selmon->active_ws)
		return;
	c = focustop(selmon);
	if (!c || !c->column)
		return;
	col = c->column;
	if (col->n_clients < 2)
		return;
	if (arg->i > 0) {
		if (c->column_link.next == &col->clients)
			return;
		target_link = c->column_link.next->next;
		wl_list_remove(&c->column_link);
		wl_list_insert(target_link->prev, &c->column_link);
	} else {
		if (c->column_link.prev == &col->clients)
			return;
		target_link = c->column_link.prev;
		wl_list_remove(&c->column_link);
		wl_list_insert(target_link->prev, &c->column_link);
	}
	arrange(selmon);
}

void
focus_window_in_column_dir(const Arg *arg)
{
	Column *col;
	Client *c, *target = NULL;
	struct wl_list *link;
	if (!arg || !selmon || !selmon->active_ws)
		return;
	c = focustop(selmon);
	if (!c || !c->column)
		return;
	col = c->column;
	if (col->n_clients < 2)
		return;
	if (arg->i > 0) {
		link = c->column_link.next;
		if (link == &col->clients)
			return;
	} else {
		link = c->column_link.prev;
		if (link == &col->clients)
			return;
	}
	target = wl_container_of(link, target, column_link);
	if (target)
		focusclient(target, 1);
}

/* ── keybind wrappers (phase 5) ──────────────────────────────────────
 * These match the (const Arg *) signature used by the keybind table.
 * Direction sign: +1 = down/right, −1 = up/left.
 */
/* After landing on a workspace, restore keyboard focus to its
 * remembered column (workspace.focused_col).  lift=0: do NOT warp
 * cursor — the workspace slide animation is still in flight, warping
 * to the new ws's tile mid-anim makes the cursor jump ahead of the
 * visible workspace, creating a visible glitch.  Cursor stays put;
 * once the slide settles, sloppy-focus picks up whatever tile is
 * under the cursor at its current screen position. */
static void
focus_first_in_workspace(Workspace *ws)
{
	Column *col;
	Client *c;

	if (!ws)
		return;

	/* A fullscreen client bound to this workspace owns focus
	 * exclusively — don't hand it to a tile behind it. */
	if (ws->mon) {
		Client *fsc = fullscreen_visible_on(ws->mon);
		if (fsc && fsc->fs_ws == ws) {
			focusclient(fsc, 0);
			return;
		}
	}

	col = ws->focused_col;
	if (!col || wl_list_empty(&col->clients)) {
		if (wl_list_empty(&ws->columns))
			return;
		col = wl_container_of(ws->columns.next, col, link);
		ws->focused_col = col;
	}
	if (wl_list_empty(&col->clients))
		return;

	c = wl_container_of(col->clients.next, c, column_link);
	focusclient(c, 0);
}

void
focus_workspace_dir(const Arg *arg)
{
	if (!arg || !selmon)
		return;
	workspace_focus_dir(selmon, arg->i);
	/* Recompute target_scroll_x / target_x for the new active workspace
	 * BEFORE warp-cursor — warpcursor reads target_* and would otherwise
	 * land at the previous workspace's layout. */
	arrange(selmon);
	focus_first_in_workspace(selmon->active_ws);
	printstatus();
}

void
focus_column_dir(const Arg *arg)
{
	Column *col;
	Monitor *m_next;
	struct wlr_output *next_out;
	enum wlr_direction wdir;

	if (!arg || !selmon || !selmon->active_ws)
		return;

	col = workspace_focus_col_dir(selmon->active_ws, arg->i);
	if (col && !wl_list_empty(&col->clients)) {
		/* Arrange FIRST so target_scroll_x / target_x reflect the new
		 * focus.  focusclient → warpcursor reads target_* — stale
		 * values would land the cursor on the OLD focused tile's
		 * projected position. */
		arrange(selmon);
		{
			Client *c = wl_container_of(col->clients.next, c, column_link);
			focusclient(c, 1);
		}
		return;
	}

	/* At edge of current workspace — try crossing to adjacent monitor. */
	wdir = (arg->i > 0) ? WLR_DIRECTION_RIGHT : WLR_DIRECTION_LEFT;
	next_out = wlr_output_layout_adjacent_output(output_layout, wdir,
			selmon->wlr_output,
			selmon->m.x + selmon->m.width / 2.0,
			selmon->m.y + selmon->m.height / 2.0);
	if (!next_out)
		return;
	m_next = next_out->data;
	if (!m_next || !m_next->wlr_output->enabled)
		return;

	selmon = m_next;

	/* Pick a tile on the new monitor — entering from the LEFT (we moved
	 * right) lands on the leftmost tile; entering from the right lands
	 * on the rightmost.  If empty, warp the cursor to the monitor centre
	 * so the user has a clear visual landing point. */
	if (m_next->active_ws && !wl_list_empty(&m_next->active_ws->columns)) {
		Column *target;
		struct wl_list *node = (arg->i > 0)
				? m_next->active_ws->columns.next
				: m_next->active_ws->columns.prev;
		target = wl_container_of(node, target, link);
		if (target && !wl_list_empty(&target->clients)) {
			m_next->active_ws->focused_col = target;
			arrange(m_next);
			{
				Client *c = wl_container_of(target->clients.next,
						c, column_link);
				focusclient(c, 1);
			}
			printstatus();
			return;
		}
	}

	/* No tiles on the destination monitor — warp cursor to its centre
	 * and drop keyboard focus so the user sees they've landed on an
	 * empty screen. */
	wlr_cursor_warp(cursor, NULL,
			m_next->m.x + m_next->m.width / 2.0,
			m_next->m.y + m_next->m.height / 2.0);
	focusclient(NULL, 0);
	printstatus();
}

/* Move focused column left/right by swapping list order.  At the workspace
 * edge, cross to the adjacent monitor (treats the whole multi-monitor row
 * as one continuous tile strip).  Multi-client columns are preserved
 * across the hop. */
void
move_column_dir(const Arg *arg)
{
	Workspace *ws;
	Column *cur, *neighbor;
	Monitor *src_mon, *m_next;
	Workspace *dst_ws;
	struct wlr_output *next_out;
	enum wlr_direction wdir;
	Client *clients[64];
	Client *focus_target = NULL;
	int n = 0, i;
	Column *new_col = NULL;

	if (!arg || !selmon || !selmon->active_ws)
		return;
	ws = selmon->active_ws;
	cur = ws->focused_col;
	if (!cur)
		return;

	if (arg->i > 0 && cur->link.next != &ws->columns) {
		neighbor = wl_container_of(cur->link.next, neighbor, link);
		wl_list_remove(&cur->link);
		wl_list_insert(&neighbor->link, &cur->link);
		arrange(selmon);
		return;
	}
	if (arg->i < 0 && cur->link.prev != &ws->columns) {
		neighbor = wl_container_of(cur->link.prev, neighbor, link);
		wl_list_remove(&cur->link);
		wl_list_insert(neighbor->link.prev, &cur->link);
		arrange(selmon);
		return;
	}
	if (arg->i == 0)
		return;

	/* At edge — try crossing to adjacent monitor. */
	wdir = (arg->i > 0) ? WLR_DIRECTION_RIGHT : WLR_DIRECTION_LEFT;
	next_out = wlr_output_layout_adjacent_output(output_layout, wdir,
			selmon->wlr_output,
			selmon->m.x + selmon->m.width / 2.0,
			selmon->m.y + selmon->m.height / 2.0);
	if (!next_out)
		return;
	m_next = next_out->data;
	if (!m_next || !m_next->wlr_output->enabled || !m_next->active_ws)
		return;
	src_mon = selmon;
	dst_ws = m_next->active_ws;

	{
		Client *c;
		wl_list_for_each(c, &cur->clients, column_link) {
			if (n >= (int)(sizeof(clients) / sizeof(clients[0])))
				break;
			clients[n++] = c;
		}
	}
	if (n == 0)
		return;
	focus_target = focustop(src_mon);
	if (!focus_target || focus_target->column != cur)
		focus_target = clients[0];

	/* setmon detaches from old column (auto-destroying it when empty)
	 * and attaches to dst_ws as a fresh column.  We migrate every
	 * client, then merge them back into a single column so multi-client
	 * groupings survive the hop. */
	for (i = 0; i < n; i++) {
		Client *cc = clients[i];
		setmon(cc, m_next, 0);
		if (!new_col) {
			new_col = cc->column;
		} else if (cc->column && cc->column != new_col) {
			Column *stray = cc->column;
			column_remove_client(cc);
			column_add_client(new_col, cc);
			(void)stray; /* column_remove_client auto-destroys when empty */
		}
	}

	if (new_col) {
		wl_list_remove(&new_col->link);
		if (arg->i > 0)
			wl_list_insert(&dst_ws->columns, &new_col->link);
		else
			wl_list_insert(dst_ws->columns.prev, &new_col->link);
		dst_ws->focused_col = new_col;
	}

	selmon = m_next;
	arrange(src_mon);
	arrange(m_next);
	if (focus_target)
		focusclient(focus_target, 1);
	printstatus();
}

/* Mod+Tab style: toggle between the two most recently used workspaces.
 * If there's no previous workspace (first switch ever), no-op. */
void
focus_last_workspace(const Arg *arg)
{
	(void)arg;
	if (!selmon || !selmon->prev_ws)
		return;
	/* Validate prev_ws is still in the list (could've been destroyed). */
	Workspace *ws;
	int found = 0;
	wl_list_for_each(ws, &selmon->workspaces, link) {
		if (ws == selmon->prev_ws) { found = 1; break; }
	}
	if (!found) {
		selmon->prev_ws = NULL;
		return;
	}
	workspace_switch(selmon, selmon->prev_ws);
	arrange(selmon);
	focus_first_in_workspace(selmon->active_ws);
	printstatus();
}

/* Numbered workspace jump (Mod+1..9).  Creates intermediate empty
 * workspaces if needed so idx N exists. */
void
focus_workspace_n(const Arg *arg)
{
	Workspace *ws, *target = NULL;
	int n;
	int max_filled;

	if (!arg || !selmon)
		return;
	n = arg->i;
	if (n < 0)
		return;

	/* Cap N at (highest-non-empty-ws + 1) so the user can land on
	 * at most one trailing empty workspace.  If they want ws 5 but
	 * only ws 0 has tiles, they get redirected to ws 1 (the single
	 * allowed empty trailing slot). */
	max_filled = max_nonempty_ws_idx(selmon);
	if (n > max_filled + 1)
		n = max_filled + 1;
	if (n < 0)
		n = 0;

	wl_list_for_each(ws, &selmon->workspaces, link) {
		if (ws->idx == n) {
			target = ws;
			break;
		}
	}

	while (!target && selmon->n_workspaces <= n)
		target = workspace_create(selmon);

	if (target) {
		workspace_switch(selmon, target);
		arrange(selmon);
		focus_first_in_workspace(target);
		printstatus();
	}
}

void
move_client_to_ws_n(const Arg *arg)
{
	Workspace *ws, *target = NULL;
	Client *c;
	int n;

	if (!arg || !selmon || !selmon->active_ws)
		return;
	n = arg->i;
	if (n < 0)
		return;

	c = focustop(selmon);
	if (!c || c->isfloating || c->isfullscreen)
		return;

	wl_list_for_each(ws, &selmon->workspaces, link) {
		if (ws->idx == n) {
			target = ws;
			break;
		}
	}
	while (!target && selmon->n_workspaces <= n)
		target = workspace_create(selmon);
	if (!target || target == selmon->active_ws)
		return;

	workspace_detach_client(c);
	workspace_attach_client(target, c);
	workspace_switch(selmon, target);
	monitor_compact_workspaces(selmon);
	arrange(selmon);
	focusclient(c, 1);
}

/* Move focused client to the workspace above/below.  Creates a new
 * workspace if moving past the last one. */
void
move_client_to_ws_dir(const Arg *arg)
{
	Workspace *cur, *target = NULL;
	Client *c;

	if (!arg || !selmon || !selmon->active_ws)
		return;

	c = focustop(selmon);
	if (!c || c->isfloating || c->isfullscreen)
		return;

	cur = selmon->active_ws;
	if (arg->i > 0) {
		if (cur->link.next != &selmon->workspaces)
			target = wl_container_of(cur->link.next, target, link);
		else
			target = workspace_create(selmon);
	} else {
		if (cur->link.prev != &selmon->workspaces)
			target = wl_container_of(cur->link.prev, target, link);
	}
	if (!target)
		return;

	workspace_detach_client(c);
	workspace_attach_client(target, c);
	workspace_switch(selmon, target);
	monitor_compact_workspaces(selmon);
	arrange(selmon);
	focusclient(c, 1);
}

/* ── workspace switching (phase 4 hook for vertical anim trigger) ────
 *
 * workspace_switch(): change m->active_ws to `target` and prime the
 * vertical animation by setting ws_y_offset to the inverse of the
 * positional shift, so visually nothing jumps — the offset then
 * decays back to 0, sliding the new workspace into view.
 *
 * If target is NULL, a fresh empty workspace is appended at the end
 * (Niri-style auto-add when scrolling past the last one).
 */
void
workspace_switch(Monitor *m, Workspace *target)
{
	int old_idx, new_idx, mon_h;

	if (!m || !target || target == m->active_ws)
		return;

	old_idx = m->active_ws ? m->active_ws->idx : 0;
	new_idx = target->idx;
	mon_h = m->m.height;

	/* Snap outgoing ws's camera fully to its target so its slide-out
	 * renders at the same position the user just left.  Sync the float
	 * spring state too — monitor_apply_positions now reads ws->scroll_x
	 * for every workspace (not just active), and we don't want a
	 * leftover scroll_x_f from a previous incomplete spring carrying
	 * the outgoing ws to a wrong position during slide. */
	if (m->active_ws) {
		m->active_ws->scroll_x = m->active_ws->target_scroll_x;
		m->active_ws->scroll_x_f = (double)m->active_ws->target_scroll_x;
		m->active_ws->scroll_x_vel = 0.0;
	}
	target->scroll_x = target->target_scroll_x;
	target->scroll_x_f = (double)target->target_scroll_x;
	target->scroll_x_vel = 0.0;

	/* Remember previous active for Mod+Tab toggle. */
	m->prev_ws = m->active_ws;
	m->active_ws = target;
	m->ws_y_offset = (double)((new_idx - old_idx) * mon_h) +
			m->ws_y_offset;

	/* Force a frame so the anim tick advances soon. */
	if (m->wlr_output)
		wlr_output_schedule_frame(m->wlr_output);
}

void
workspace_focus_dir(Monitor *m, int dir)
{
	Workspace *cur, *target = NULL;

	if (!m || !m->active_ws || dir == 0)
		return;

	cur = m->active_ws;
	if (dir > 0) {
		/* Forward: only allow advance if the current ws has tiles
		 * OR we're moving into an existing ws.  This prevents
		 * creating a chain of empty workspaces. */
		if (cur->link.next != &m->workspaces) {
			target = wl_container_of(cur->link.next, target, link);
			/* Even when target exists, block jumping into an
			 * empty target if current is also empty (no two
			 * consecutive empties allowed). */
			if (target->n_columns == 0 && cur->n_columns == 0)
				return;
		} else {
			if (cur->n_columns == 0)
				return;  /* current empty → don't make another */
			target = workspace_create(m);
		}
	} else {
		if (cur->link.prev != &m->workspaces) {
			target = wl_container_of(cur->link.prev, target, link);
		}
	}

	if (target)
		workspace_switch(m, target);
}

/* Move focus left/right between columns within the active workspace.
 * Returns the newly-focused column (NULL if no movement possible). */
Column *
workspace_focus_col_dir(Workspace *ws, int dir)
{
	Column *cur, *target = NULL;

	if (!ws || dir == 0)
		return NULL;

	cur = ws->focused_col;
	if (!cur) {
		if (wl_list_empty(&ws->columns))
			return NULL;
		target = wl_container_of(ws->columns.next, target, link);
	} else if (dir > 0) {
		if (cur->link.next != &ws->columns)
			target = wl_container_of(cur->link.next, target, link);
	} else {
		if (cur->link.prev != &ws->columns)
			target = wl_container_of(cur->link.prev, target, link);
	}

	if (target)
		ws->focused_col = target;
	return target;
}

void
monitor_apply_positions(Monitor *m)
{
	Workspace *ws;
	Column *col;
	Client *c;
	int gap, ws_stride;
	int vertical_anim;

	if (!m || !m->wlr_output->enabled)
		return;

	gap = m->gaps ? (int)gappx : 0;
	ws_stride = m->m.height;
	vertical_anim = (fabs(m->ws_y_offset) > 0.5);

	wl_list_for_each(ws, &m->workspaces, link) {
		int ws_y_base;
		int row_scroll;
		int ws_visible = (ws == m->active_ws) || vertical_anim;

		/* Settled state: inactive ws clients live off-screen, but
		 * leaving their scene nodes ENABLED lets sub-pixel rounding,
		 * CSD shadows, or stale m->w shifts (waybar toggle after a
		 * settle) bleed a 1-px strip into the visible area.  Disable
		 * the scene tree of every tiled client on an inactive ws when
		 * no vertical anim is in flight — re-enabled below as soon as
		 * the next switch animation starts. */
		if (!ws_visible) {
			wl_list_for_each(col, &ws->columns, link) {
				wl_list_for_each(c, &col->clients, column_link) {
					if (c->scene && c->scene->node.enabled
							&& !c->isfloating)
						wlr_scene_node_set_enabled(
							&c->scene->node, 0);
				}
			}
			continue;
		}

		/* ws is active or mid-slide: ensure its tile scenes are
		 * enabled so the slide-in and steady state both render. */
		wl_list_for_each(col, &ws->columns, link) {
			wl_list_for_each(c, &col->clients, column_link) {
				if (c->scene && !c->scene->node.enabled
						&& !c->isfloating)
					wlr_scene_node_set_enabled(
						&c->scene->node, 1);
			}
		}

		ws_y_base = (ws->idx -
			(m->active_ws ? m->active_ws->idx : 0)) * ws_stride
			+ (int)m->ws_y_offset;

		/* Each ws keeps its own saved horizontal scroll position so
		 * during a vertical workspace slide the OUTGOING ws still
		 * renders at the camera-x it was last left at.  Without this,
		 * non-active ws fell back to row_scroll=0 which yanked the
		 * leftmost column into view for the duration of the slide —
		 * visible as the leftmost tile briefly overlapping the
		 * current view at switch start. */
		row_scroll = ws->scroll_x;

		/* col->x is driven by the spring tick in monitor_anim_tick
		 * — do NOT snap it here.  col->y stays 0 (no per-col vert
		 * anim). */
		wl_list_for_each(col, &ws->columns, link) {
			col->y = col->target_y;
		}

		wl_list_for_each(col, &ws->columns, link) {
			/* m->w is the LIVE (spring-lerped) tile bbox.  Use it
			 * for positions so a waybar toggle slides every tile's
			 * top edge in lock-step with m->w.y while keeping the
			 * bottom locked (m->w.y + m->w.height stays constant). */
			int col_abs_x = m->w.x + col->x - row_scroll;
			int col_abs_y = m->w.y + ws_y_base + col->y;
			int nc = col->n_clients;
			int j = 0;
			int per_h, used_h;

			if (nc == 0)
				continue;
			/* Use live m->w.height so per-client per_h lerps with
			 * the column-height spring (waybar toggle case). */
			used_h = m->w.height;
			per_h = (used_h - gap * (nc - 1)) / nc;

			wl_list_for_each(c, &col->clients, column_link) {
				struct wlr_box geo;
				geo.x = col_abs_x;
				geo.y = col_abs_y + j * (per_h + gap);
				geo.width = col->width;
				geo.height = (j == nc - 1)
					? (used_h - j * (per_h + gap))
					: per_h;
				client_set_target_geom(c, geo);
				j++;
			}
		}
	}
}
