#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "nixlytile.h"
#include "client.h"
#include "diag.h"

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

	/* Unbind clients from dying workspace. */
	wl_list_for_each(c, &clients, link) {
		if (c->fs_ws == ws)
			c->fs_ws = NULL;
		if (c->float_ws == ws)
			c->float_ws = NULL;
	}

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

	/* Clear any in-flight mouse column-resize referencing this column
	 * — the next motion event would write into freed memory. */
	input_column_gone(col);

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

/* Membership change invalidates the column's height weights (set by
 * Mod+RightDrag as pixel values for a specific member set) — reset to
 * even split. */
static void
column_reset_weights(Column *col)
{
	Client *c;
	wl_list_for_each(c, &col->clients, column_link)
		c->col_weight = 0.0;
}

void
column_add_client(Column *col, Client *c)
{
	if (!col || !c)
		return;

	if (c->column)
		column_remove_client(c);

	c->column = col;
	c->col_weight = 0.0;
	wl_list_insert(col->clients.prev, &c->column_link);
	col->n_clients++;
	column_reset_weights(col);
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
	c->col_weight = 0.0;
	col->n_clients--;

	/* Empty columns auto-destruct (Niri behavior). */
	if (col->n_clients == 0) {
		column_destroy(col);
		return;
	}
	column_reset_weights(col);
}

/* Position of a column in its workspace's left→right order (-1: none). */
int
column_index(Column *col)
{
	Column *other;
	int i = 0;

	if (!col || !col->ws)
		return -1;
	wl_list_for_each(other, &col->ws->columns, link) {
		if (other == col)
			return i;
		i++;
	}
	return -1;
}

/* Move a column to slot `idx` in the row.  Used to put a tile back where
 * it was: entering fullscreen destroys its column, and reattaching just
 * appends next to whatever is focused — which reorders the row. */
void
column_move_to_index(Column *col, int idx)
{
	Column *other;
	int i = 0;

	if (!col || !col->ws || idx < 0 || idx == column_index(col))
		return;

	wl_list_remove(&col->link);
	wl_list_for_each(other, &col->ws->columns, link) {
		if (i++ == idx) {
			wl_list_insert(other->link.prev, &col->link);
			return;
		}
	}
	wl_list_insert(col->ws->columns.prev, &col->link);
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
	refreshworkspacemodule(ws->mon);
}

void
workspace_detach_client(Client *c)
{
	Workspace *ws = c && c->column ? c->column->ws : NULL;

	txn_forget(c);
	c->float_want_set = 0;
	column_remove_client(c);
	if (ws)
		refreshworkspacemodule(ws->mon);
}

/* Move a column to another workspace, clients and stacking intact.
 * Appends at the end so source order is preserved across repeated calls. */
void
workspace_adopt_column(Workspace *dst, Column *col)
{
	Workspace *src;
	Client *c;

	if (!dst || !col || col->ws == dst)
		return;

	src = col->ws;
	wl_list_remove(&col->link);
	if (src) {
		src->n_columns--;
		if (src->focused_col == col)
			src->focused_col = wl_list_empty(&src->columns) ? NULL
				: wl_container_of(src->columns.next,
					src->focused_col, link);
	}

	col->ws = dst;
	wl_list_insert(dst->columns.prev, &col->link);
	dst->n_columns++;
	if (!dst->focused_col)
		dst->focused_col = col;

	/* Springs are meaningless across monitors — snap on next layout */
	col->just_created = 1;

	if (dst->mon) {
		wl_list_for_each(c, &col->clients, column_link) {
			c->mon = dst->mon;
			c->tags = dst->mon->tagset[dst->mon->seltags];
		}
	}
}

/* Move a whole workspace to another monitor, columns intact. */
void
workspace_move_to_monitor(Workspace *ws, Monitor *dst)
{
	Monitor *src;
	Column *col;
	Client *c;

	if (!ws || !dst || ws->mon == dst)
		return;

	src = ws->mon;
	wl_list_remove(&ws->link);
	if (src) {
		src->n_workspaces--;
		if (src->active_ws == ws)
			src->active_ws = wl_list_empty(&src->workspaces) ? NULL
				: wl_container_of(src->workspaces.next, ws, link);
		if (src->prev_ws == ws)
			src->prev_ws = NULL;
	}

	ws->mon = dst;
	ws->idx = dst->next_ws_id++;
	wl_list_insert(dst->workspaces.prev, &ws->link);
	dst->n_workspaces++;
	if (!dst->active_ws)
		dst->active_ws = ws;

	wl_list_for_each(col, &ws->columns, link) {
		col->just_created = 1;
		wl_list_for_each(c, &col->clients, column_link) {
			c->mon = dst;
			c->tags = dst->tagset[dst->seltags];
		}
	}
	wl_list_for_each(c, &clients, link) {
		if (client_float_ws(c) != ws)
			continue;
		c->mon = dst;
		c->tags = dst->tagset[dst->seltags];
	}
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
/* Tiles or bound detached clients. */
int
workspace_has_clients(Workspace *ws)
{
	Client *c;
	if (!ws)
		return 0;
	if (ws->n_columns > 0)
		return 1;
	wl_list_for_each(c, &clients, link)
		if ((c->isfullscreen && c->fs_ws == ws) || client_float_ws(c) == ws)
			return 1;
	return 0;
}

/* Workspace a floating window lives on; NULL = shown everywhere. */
Workspace *
client_float_ws(Client *c)
{
	if (!c->isfloating || c->isfullscreen || c->issticky || c->is_notif
			|| client_is_unmanaged(c))
		return NULL;
	return c->float_ws;
}

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
		if (workspace_has_clients(ws) && ws->idx > max_idx)
			max_idx = ws->idx;
	}
	return max_idx;
}

/* Workspace with index idx on m, creating trailing workspaces as needed —
 * window-rule `workspace N` may address a workspace before it exists. */
Workspace *
workspace_get_or_create_idx(Monitor *m, int idx)
{
	Workspace *ws;

	if (!m || idx < 0)
		return NULL;
	for (;;) {
		wl_list_for_each(ws, &m->workspaces, link)
			if (ws->idx == idx)
				return ws;
		if (m->n_workspaces > idx || !workspace_create(m))
			return NULL;
	}
}

/* Close index gaps left by emptied workspaces.  When a workspace
 * becomes empty but there's still a populated workspace below it,
 * destroy the empty one and shift everything below it up by one slot.
 * Always leaves at most one trailing empty workspace (Niri convention).
 *
 * If the empty workspace was the active one, the next workspace takes
 * its place — the user is never stranded on a workspace they can't
 * leave to reach tiles. */
/* active_ws must live on m's own list: a stale cross-monitor pointer
 * walks out of the list in workspace_focus_dir and hands workspace_layout
 * the list head cast as a Workspace (SEGV, whole session dies).  Repair
 * to m's first workspace instead of trusting the pointer. */
Workspace *
monitor_active_ws(Monitor *m)
{
	Workspace *ws;

	if (!m)
		return NULL;
	wl_list_for_each(ws, &m->workspaces, link)
		if (ws == m->active_ws)
			return ws;

	if (m->active_ws)
		wlr_log(WLR_ERROR, "active_ws not on %s — repairing",
			m->wlr_output ? m->wlr_output->name : "?");
	m->active_ws = wl_list_empty(&m->workspaces) ? NULL
		: wl_container_of(m->workspaces.next, ws, link);
	m->prev_ws = NULL;
	return m->active_ws;
}

void
monitor_compact_workspaces(Monitor *m)
{
	int changed = 1;

	if (!m)
		return;
	/* HTPC: workspaces are a fixed app grid (1=Steam 2=RetroArch
	 * 3=GeForce NOW 4=nixlymedia). A crashed app leaves its workspace
	 * empty for a second before the supervisor loop respawns it —
	 * compacting would shift every app one slot up in that window. */
	if (htpc_mode_active)
		return;

	while (changed) {
		Workspace *ws, *other, *target;
		int gap_idx, was_active;

		changed = 0;
		wl_list_for_each(ws, &m->workspaces, link) {
			int has_filled_after = 0;

			if (workspace_has_clients(ws))
				continue;
			wl_list_for_each(other, &m->workspaces, link) {
				if (other != ws && other->idx > ws->idx
						&& workspace_has_clients(other)) {
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
		if (workspace_has_clients(last) || workspace_has_clients(prev))
			break;
		if (m->active_ws == last)
			m->active_ws = prev;
		if (m->prev_ws == last)
			m->prev_ws = NULL;
		workspace_destroy(last);
		if (m->next_ws_id > 0)
			m->next_ws_id--;
	}

	refreshworkspacemodule(m);
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

/* Default tiles squeeze down to this fraction. */
#define FLEX_MIN_DIVISOR 2

/* Target width of a column. */
static int
column_target_width_px(Column *col, int mon_w, int flex_w)
{
	if (col->fullscreen)
		return mon_w;
	if (col->width_px_override > 0)
		return MAX(50, MIN(col->width_px_override, mon_w));
	if (col->width_idx >= 0 && col->width_idx < n_preset_column_widths)
		return (int)((double)mon_w *
				preset_column_widths[col->width_idx]);
	return flex_w;
}

/* Column has no user-chosen width. */
static int
column_is_flex(Column *col)
{
	return !col->fullscreen && col->width_px_override <= 0 &&
		col->width_idx < 0;
}

/* Default width that keeps N tiles visible. */
static int
flex_column_width(Workspace *ws, int mon_w, int gap, int n_new)
{
	Column *col;
	int n_default = default_tiles_per_row(ws->mon);
	int std_w = (mon_w - (n_default - 1) * gap) / n_default;
	int n = ws->n_columns + n_new;
	int n_flex = n_new, fixed = 0, fit;

	if (n > n_default)
		return std_w;
	wl_list_for_each(col, &ws->columns, link) {
		if (column_is_flex(col))
			n_flex++;
		else
			fixed += column_target_width_px(col, mon_w, std_w);
	}
	if (n_flex == 0)
		return std_w;
	fit = (mon_w - fixed - (n - 1) * gap) / n_flex;
	if (fit < std_w / FLEX_MIN_DIVISOR)
		return std_w;
	if (fit > std_w && n < n_default)
		return std_w;
	return fit;
}

/* Inner size of a new default column. */
void
workspace_new_column_inner_size(Workspace *ws, int bw, int *out_w, int *out_h)
{
	Monitor *m;
	int mon_w, mon_h, gap, w, h;

	if (out_w) *out_w = 0;
	if (out_h) *out_h = 0;
	if (!ws || !ws->mon)
		return;

	m = ws->mon;
	gap = m->gaps ? (int)gappx : 0;
	mon_w = m->w_initialized ? m->w_target.width : m->w.width;
	mon_h = m->w_initialized ? m->w_target.height : m->w.height;
	if (mon_w <= 0 || mon_h <= 0) {
		mon_w = m->m.width - 2 * gap;
		mon_h = m->m.height - 2 * gap;
	}
	if (mon_w <= 0 || mon_h <= 0)
		return;

	w = flex_column_width(ws, mon_w, gap, 1) - 2 * bw;
	h = mon_h - 2 * bw;
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	if (out_w) *out_w = w;
	if (out_h) *out_h = h;
}

/* Narrowest a column may be dragged. */
#define COLUMN_MIN_PX 100

/* Widest client minimum in the column. */
int
column_min_width(Column *col)
{
	Client *c;
	int w, h, min = COLUMN_MIN_PX;

	wl_list_for_each(c, &col->clients, column_link) {
		client_get_min_size(c, &w, &h);
		min = MAX(min, w + 2 * (int)c->bw);
	}
	return min;
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
	if (!m->wlr_output || !m->wlr_output->enabled)
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

	{
		int flex_w = flex_column_width(ws, mon_w, gap, 0);
		x_cursor = 0;
		wl_list_for_each(col, &ws->columns, link) {
			int w = column_target_width_px(col, mon_w, flex_w);
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

	/* Text protocol on stdout only when something consumes it —
	 * otherwise this is printf+fflush per monitor on every state
	 * change (including every title change) for nothing. */
	if (!status_stdout_enabled)
		goto publish;

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output)
			continue;
		occ = sel = urg = 0;

		wl_list_for_each(ws, &m->workspaces, link) {
			uint32_t bit;
			if (ws->idx >= 31)
				continue;
			bit = 1u << ws->idx;
			/* workspace_has_clients also counts fullscreen
			 * clients bound via fs_ws (detached from columns) —
			 * a workspace holding only a fullscreen game must
			 * not show as empty. */
			if (workspace_has_clients(ws))
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

publish:
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
	int mon_w, step;
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
	step = mon_w / 20;
	if (step < 40) step = 40;

	if (col->link.prev != &col->ws->columns)
		left_nbr = wl_container_of(col->link.prev, left_nbr, link);
	if (col->link.next != &col->ws->columns)
		right_nbr = wl_container_of(col->link.next, right_nbr, link);

	cur_w = col->target_width;

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
		left_cur = left_nbr->target_width;
		left_new = left_cur - dir * step;
		if (left_new < min_w)
			return;
		delta_total += step;
	}
	if (right_nbr) {
		right_cur = right_nbr->target_width;
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
	/* Snap into place on first layout instead of springing the
	 * column in from x=0 with width 0. */
	dst->just_created = 1;
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
	 * exclusively — don't hand it to a tile behind it.  Lift so the
	 * cursor warps onto it: selecting this tag gives the fullscreen
	 * client mouse + keyboard focus (gamepads feed it via evdev). */
	if (ws->mon) {
		Client *fsc = fullscreen_visible_on(ws->mon);
		if (fsc && fsc->fs_ws == ws) {
			exclusive_focus = NULL;
			focusclient(fsc, 1);
			return;
		}
	}

	col = ws->focused_col;
	if (!col || wl_list_empty(&col->clients)) {
		if (wl_list_empty(&ws->columns)) {
			/* Empty workspace: clear keyboard focus.  Leaving it
			 * on the previous ws's (now hidden) client — e.g. a
			 * fullscreen video — sends every keystroke to an
			 * invisible surface and reads as a frozen desktop. */
			focusclient(NULL, 0);
			return;
		}
		col = wl_container_of(ws->columns.next, col, link);
		ws->focused_col = col;
	}
	if (wl_list_empty(&col->clients)) {
		focusclient(NULL, 0);
		return;
	}

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
	Client *moved[64];
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
			if (n >= (int)LENGTH(moved))
				break;
			moved[n++] = c;
		}
	}
	if (n == 0)
		return;
	focus_target = focustop(src_mon);
	if (!focus_target || focus_target->column != cur)
		focus_target = moved[0];

	/* setmon detaches from old column (auto-destroying it when empty)
	 * and attaches to dst_ws as a fresh column.  We migrate every
	 * client, then merge them back into a single column so multi-client
	 * groupings survive the hop. */
	for (i = 0; i < n; i++) {
		Client *cc = moved[i];
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
	 * allowed empty trailing slot).  HTPC is exempt: its workspaces are
	 * a fixed app grid, so the guide must reach ws 3 (GeForce NOW) even
	 * when nothing below it has mapped yet. */
	if (!htpc_mode_active) {
		max_filled = max_nonempty_ws_idx(selmon);
		if (n > max_filled + 1)
			n = max_filled + 1;
	}
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

	if (!arg || !selmon || !monitor_active_ws(selmon))
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

	if (!arg || !selmon || !(cur = monitor_active_ws(selmon)))
		return;

	c = focustop(selmon);
	if (!c || c->isfloating || c->isfullscreen)
		return;

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

	/* foreign target strands active_ws */
	if (!m || !target || target == m->active_ws || target->mon != m)
		return;

	/* Same invalidation as the tag-switch paths (view/tag/...): the
	 * outgoing ws may hold a fullscreen video whose cadence/VRR state
	 * and classify cache would otherwise survive the switch and keep
	 * rendermon in the video hold path with the video hidden. */
	invalidate_video_pacing(m);

	old_idx = m->active_ws ? m->active_ws->idx : 0;
	new_idx = target->idx;
	mon_h = m->m.height;
	diag_logf("WS", "%s: %d -> %d", m->wlr_output->name, old_idx, new_idx);

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

	refreshworkspacemodule(m);

	/* HTPC: the incoming workspace's fullscreen app was mapped hidden
	 * with its output side effects skipped — apply them now, and drop
	 * the outgoing app's holds. No-op outside htpc mode. */
	htpc_ws_refresh_fx(m);
}

void
workspace_focus_dir(Monitor *m, int dir)
{
	Workspace *cur, *target = NULL;

	if (!m || dir == 0 || !(cur = monitor_active_ws(m)))
		return;
	if (dir > 0) {
		/* Forward: only allow advance if the current ws is occupied
		 * (tiles or a fullscreen client) OR we're moving into an
		 * existing ws.  This prevents creating a chain of empty
		 * workspaces while still letting the user leave a
		 * fullscreen-only workspace. */
		if (cur->link.next != &m->workspaces) {
			target = wl_container_of(cur->link.next, target, link);
			/* Even when target exists, block jumping into an
			 * empty target if current is also empty (no two
			 * consecutive empties allowed). */
			if (!workspace_has_clients(target)
					&& !workspace_has_clients(cur))
				return;
		} else {
			if (!workspace_has_clients(cur))
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
	/* Re-check gave-up clients. */
	m->converge_dirty = 1;
	m->tile_pass++;

	gap = m->gaps ? (int)gappx : 0;
	ws_stride = m->m.height;
	vertical_anim = (fabs(m->ws_y_offset) > 0.5);

	/* Fullscreen client owning the active workspace: it shares the ws
	 * with its tiles, and while settled those tiles must stay disabled
	 * (arrange() hid them for exclusivity / direct scanout) — the
	 * enable-loop below would otherwise resurrect them every frame. */
	Client *active_fsc = NULL;
	wl_list_for_each(c, &clients, link) {
		if (c->mon == m && c->isfullscreen
				&& c->fs_ws == m->active_ws
				&& client_surface(c) && client_surface(c)->mapped) {
			active_fsc = c;
			break;
		}
	}

	wl_list_for_each(ws, &m->workspaces, link) {
		int ws_y_base;
		int row_scroll;
		int ox, oy;
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
		 * enabled so the slide-in and steady state both render.
		 * Exception: tiles under a settled fullscreen client stay
		 * disabled (fullscreen exclusivity). */
		wl_list_for_each(col, &ws->columns, link) {
			wl_list_for_each(c, &col->clients, column_link) {
				int want = 1;
				if (ws == m->active_ws && active_fsc
						&& !vertical_anim
						&& !client_is_fs_companion(c, active_fsc))
					want = 0;
				if (c->scene && !c->isfloating
						&& c->scene->node.enabled != want)
					wlr_scene_node_set_enabled(
						&c->scene->node, want);
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

		/* Live camera offset, never held. */
		ox = m->w.x - (int)lround(ws == m->active_ws
				? ws->scroll_x_f : (double)row_scroll);
		oy = m->w.y + ws_y_base;

		wl_list_for_each(col, &ws->columns, link) {
			/* Edges rounded independently: no jitter. */
			int col_x = (int)lround(col->x_f);
			int col_w = (int)lround(col->x_f + col->width_f) - col_x;
			int nc = col->n_clients;
			int j = 0;
			int avail, y_cursor = 0;
			double sumw = 0.0;

			if (nc == 0)
				continue;
			/* Weighted heights, weight <= 0 means 1. */
			avail = m->w.height - gap * (nc - 1);
			wl_list_for_each(c, &col->clients, column_link)
				sumw += c->col_weight > 0.0 ? c->col_weight : 1.0;
			if (sumw <= 0.0)
				sumw = (double)nc;

			wl_list_for_each(c, &col->clients, column_link) {
				struct wlr_box geo;
				double w = c->col_weight > 0.0 ? c->col_weight : 1.0;
				geo.x = col_x;
				geo.y = col->y + y_cursor;
				geo.width = col_w;
				geo.height = (j == nc - 1)
					? (avail - y_cursor + gap * j)
					: (int)((double)avail * w / sumw);
				txn_set_target(c, geo, ox, oy);
				y_cursor += geo.height + gap;
				j++;
			}
		}
	}
	txn_place(m);

	/* Detached clients slide with their workspace. */
	wl_list_for_each(c, &clients, link) {
		Workspace *bound;
		int y_base, visible;

		if (c->mon != m || !c->scene)
			continue;
		bound = c->isfullscreen ? c->fs_ws : client_float_ws(c);
		if (!bound || !client_surface(c) || !client_surface(c)->mapped)
			continue;

		visible = bound == m->active_ws || vertical_anim;
		if (visible && active_fsc && c != active_fsc && !vertical_anim
				&& !client_is_fs_companion(c, active_fsc))
			visible = 0;
		if (!visible) {
			if (c->scene->node.enabled)
				wlr_scene_node_set_enabled(&c->scene->node, 0);
			continue;
		}

		y_base = (bound->idx - (m->active_ws ? m->active_ws->idx : 0))
				* ws_stride + (int)m->ws_y_offset;
		if (!c->scene->node.enabled)
			wlr_scene_node_set_enabled(&c->scene->node, 1);
		wlr_scene_node_set_position(&c->scene->node,
				c->geom.x, c->geom.y + y_base);

		/* Fullscreen slides over the bar. */
		if (c->isfullscreen && c->scene_surface) {
			struct wlr_box wg;
			client_get_clip(c, &wg);
			wlr_scene_subsurface_tree_set_clip(
					&c->scene_surface->node, &wg);
		}
	}
}
