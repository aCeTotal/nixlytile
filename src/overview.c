/*
 * overview.c — zoomed-out view of every tile on every workspace.
 * See overview.h.
 */
#include <fcntl.h>
#include <unistd.h>

#include "nixlytile.h"
#include "client.h"
#include "overview.h"

/* Layout constants.  All in logical pixels at scale 1; the grid divides
 * whatever the monitor gives us, so these only set proportions. */
#define OV_MARGIN        48   /* outer margin around the whole grid */
#define OV_ROW_GAP       28   /* between workspace rows */
#define OV_CELL_GAP      20   /* between tiles inside a row */
#define OV_HEADER_H      46   /* label strip above each thumbnail */
#define OV_LABEL_PAD     10
#define OV_RAIL_W        4    /* workspace-number rail on the left */
#define OV_RAIL_GAP      14
#define OV_SEL_BORDER    3

/* Spring for the zoom.  Slightly softer than the tile springs — this is
 * a deliberate "step back and look" gesture, not a snap. */
static const SpringParams SPRING_OVERVIEW = { 1.0, 1.0, 1100.0 };

typedef struct {
	Client *c;
	Workspace *ws;

	/* Where the tile really is on screen right now (zoom origin). */
	struct wlr_box live;
	/* Where it belongs in the grid (zoom target). */
	struct wlr_box slot;

	struct wlr_scene_tree      *tree;   /* per-cell: header + thumb + sel */
	struct wlr_scene_surface   *thumb;  /* mirror of the live surface */
	struct wlr_scene_rect      *ghost;  /* stand-in when there is no buffer */
	struct wlr_scene_buffer    *header; /* rendered label card */
	struct wlr_scene_rect      *sel[4]; /* selection border */

	int header_w, header_h;
	int row;                            /* index of the workspace row */
} Cell;

static struct {
	int open;                 /* 1 while the overview owns the screen */
	int closing;              /* animating back out */
	Monitor *mon;
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *backdrop;

	Cell *cells;
	int n_cells;
	int n_rows;
	int sel;                  /* index into cells */

	/* 0 = tiles sit exactly where they really are, 1 = full grid. */
	double t, t_vel, t_target;

	/* Set when Enter picked a target; applied once the zoom-in ends. */
	Client *activate;
} ov;

/* ── process introspection ──────────────────────────────────────────
 * A terminal's interesting state is not in the toplevel — it is in
 * whatever the shell is running.  /proc/<pid>/task/<pid>/children gives
 * the child list directly, so this never scans all of /proc (which is
 * what makes is_pid_descendant_of expensive).  Runs once per cell when
 * the overview opens, never per frame. */

static pid_t
proc_first_child(pid_t pid)
{
	char path[64], buf[256];
	int fd;
	ssize_t n;
	pid_t child = 0;

	snprintf(path, sizeof(path), "/proc/%d/task/%d/children",
			(int)pid, (int)pid);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	child = (pid_t)atoi(buf);
	return child > 0 ? child : 0;
}

/* Deepest descendant along the first-child chain — for a terminal that
 * is the shell, or whatever the shell is currently running (ssh, vim…). */
static pid_t
proc_leaf(pid_t pid)
{
	int depth;

	for (depth = 0; depth < 8; depth++) {
		pid_t next = proc_first_child(pid);
		if (!next)
			break;
		pid = next;
	}
	return pid;
}

static void
proc_comm(pid_t pid, char *out, size_t sz)
{
	char path[64];
	FILE *f;

	out[0] = '\0';
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
	if (!(f = fopen(path, "r")))
		return;
	if (fgets(out, (int)sz, f)) {
		size_t l = strlen(out);
		if (l && out[l - 1] == '\n')
			out[l - 1] = '\0';
	}
	fclose(f);
}

static void
proc_cwd(pid_t pid, char *out, size_t sz)
{
	char path[64];
	ssize_t n;

	out[0] = '\0';
	snprintf(path, sizeof(path), "/proc/%d/cwd", (int)pid);
	n = readlink(path, out, sz - 1);
	out[n > 0 ? n : 0] = '\0';
}

/* argv of pid, NUL-separated, copied into buf. */
static int
proc_cmdline(pid_t pid, char *buf, size_t sz)
{
	char path[64];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
	if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
		return 0;
	n = read(fd, buf, sz - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = '\0';
	return (int)n;
}

/* "user@host" / "host" out of an ssh command line, skipping options and
 * their arguments.  Empty when this isn't ssh. */
static void
ssh_target(pid_t pid, char *out, size_t sz)
{
	char buf[4096];
	int len, i = 0;
	const char *arg;
	int skip_val = 0;

	out[0] = '\0';
	if (!(len = proc_cmdline(pid, buf, sizeof(buf))))
		return;

	arg = buf;
	if (strcmp(arg, "ssh") && !strstr(arg, "/ssh"))
		return;

	i = (int)strlen(arg) + 1;
	while (i < len) {
		arg = buf + i;
		i += (int)strlen(arg) + 1;
		if (skip_val) {
			skip_val = 0;
			continue;
		}
		if (arg[0] == '-') {
			/* Options that take a value; everything else is a flag. */
			if (strchr("bcDEeFIiJLlmOopQRSWw", arg[1]) && !arg[2])
				skip_val = 1;
			continue;
		}
		snprintf(out, sz, "%s", arg);
		return;
	}
}

/* ── label card ─────────────────────────────────────────────────────── */

static struct wlr_buffer *
header_buffer(Cell *cell, int w, int h)
{
	struct PixmanBuffer *buf;
	pixman_image_t *img;
	void *data;
	int stride, base;
	char sub[512] = "";
	char path[PATH_MAX] = "";
	char host[256] = "";
	char comm[64] = "";
	const char *title, *appid;
	static const float col_title[4] = { 0.96f, 0.96f, 0.97f, 1.0f };
	static const float col_app[4]   = { 0.62f, 0.66f, 0.72f, 1.0f };
	static const float col_ssh[4]   = { 0.45f, 0.78f, 0.55f, 1.0f };
	pid_t pid, leaf;

	if (w <= 0 || h <= 0 || !statusfont.font)
		return NULL;

	stride = w * 4;
	data = ecalloc(1, (size_t)stride * (size_t)h);
	img = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, data, stride);
	if (!img) {
		free(data);
		return NULL;
	}

	title = client_get_title(cell->c);
	appid = client_get_appid(cell->c);
	if (!title || !*title)
		title = appid && *appid ? appid : "(uten tittel)";

	/* Second line: app id, plus the shell's cwd (and ssh host) when the
	 * toplevel turns out to be a terminal running something. */
	pid = client_get_pid(cell->c);
	if (pid > 0) {
		leaf = proc_leaf(pid);
		if (leaf > 0 && leaf != pid) {
			proc_comm(leaf, comm, sizeof(comm));
			proc_cwd(leaf, path, sizeof(path));
			ssh_target(leaf, host, sizeof(host));
		}
	}

	if (host[0])
		snprintf(sub, sizeof(sub), "%s  ·  ssh %s",
				appid && *appid ? appid : "?", host);
	else if (path[0])
		snprintf(sub, sizeof(sub), "%s  ·  %s",
				appid && *appid ? appid : "?", path);
	else if (comm[0])
		snprintf(sub, sizeof(sub), "%s  ·  %s",
				appid && *appid ? appid : "?", comm);
	else
		snprintf(sub, sizeof(sub), "%s",
				appid && *appid ? appid : "?");

	base = statusfont.font->ascent;
	nixly_text_draw(img, statusfont.font, title, OV_LABEL_PAD, base,
			col_title, w - 2 * OV_LABEL_PAD);
	nixly_text_draw(img, statusfont.font, sub, OV_LABEL_PAD,
			base + statusfont.font->height,
			host[0] ? col_ssh : col_app, w - 2 * OV_LABEL_PAD);

	pixman_image_unref(img);

	buf = ecalloc(1, sizeof(*buf));
	buf->image = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h,
			data, stride);
	buf->data = data;
	buf->drm_format = DRM_FORMAT_ARGB8888;
	buf->stride = stride;
	buf->owns_data = 1;
	wlr_buffer_init(&buf->base, &pixman_buffer_impl, w, h);
	return &buf->base;
}

/* ── geometry ───────────────────────────────────────────────────────── */

/* Absolute on-screen rectangle of c right now, including the vertical
 * offset of the workspace it lives on.  This is the zoom origin, so a
 * tile two workspaces down flies in from below instead of appearing. */
static struct wlr_box
cell_live_box(Monitor *m, Client *c, Workspace *ws)
{
	struct wlr_box b = c->geom;
	int active_idx = m->active_ws ? m->active_ws->idx : 0;

	b.y += (ws->idx - active_idx) * m->m.height + (int)m->ws_y_offset;
	return b;
}

static int
lerp_i(int from, int to, double t)
{
	return (int)lround((double)from + ((double)to - (double)from) * t);
}

/* Grid layout: one row per non-empty workspace, tiles sized to the row's
 * share of the screen and centred.  Every tile of every workspace gets a
 * slot — nothing is elided, so a busy workspace simply gets smaller
 * thumbnails rather than a scrollbar. */
static void
layout_cells(void)
{
	Monitor *m = ov.mon;
	int area_x, area_y, area_w, area_h;
	int row_h, row;

	if (!m || ov.n_rows <= 0)
		return;

	area_x = m->m.x + OV_MARGIN + OV_RAIL_W + OV_RAIL_GAP;
	area_y = m->m.y + OV_MARGIN;
	area_w = m->m.width  - 2 * OV_MARGIN - OV_RAIL_W - OV_RAIL_GAP;
	area_h = m->m.height - 2 * OV_MARGIN;
	if (area_w < 100 || area_h < 100)
		return;

	row_h = (area_h - (ov.n_rows - 1) * OV_ROW_GAP) / ov.n_rows;
	if (row_h < OV_HEADER_H + 40)
		row_h = OV_HEADER_H + 40;

	for (row = 0; row < ov.n_rows; row++) {
		int n = 0, i, thumb_h, thumb_w_budget, total_w, x;
		int row_y = area_y + row * (row_h + OV_ROW_GAP);
		double scale = 1.0;

		for (i = 0; i < ov.n_cells; i++)
			if (ov.cells[i].row == row)
				n++;
		if (!n)
			continue;

		thumb_h = row_h - OV_HEADER_H;
		thumb_w_budget = (area_w - (n - 1) * OV_CELL_GAP) / n;

		/* Uniform scale across the row: pick the factor that makes the
		 * widest tile fit its share and the tallest fit the row.  Every
		 * thumbnail then keeps its own aspect ratio AND its relative
		 * size, so a half-width column still reads as half-width. */
		for (i = 0; i < ov.n_cells; i++) {
			Cell *cl = &ov.cells[i];
			double sw, sh;

			if (cl->row != row || cl->live.width <= 0 ||
					cl->live.height <= 0)
				continue;
			sw = (double)thumb_w_budget / (double)cl->live.width;
			sh = (double)thumb_h / (double)cl->live.height;
			if (sw < scale) scale = sw;
			if (sh < scale) scale = sh;
		}
		if (scale <= 0.0)
			scale = 0.1;

		total_w = 0;
		for (i = 0; i < ov.n_cells; i++) {
			Cell *cl = &ov.cells[i];
			if (cl->row != row)
				continue;
			total_w += (int)(cl->live.width * scale);
		}
		total_w += (n - 1) * OV_CELL_GAP;

		x = area_x + (area_w - total_w) / 2;
		if (x < area_x)
			x = area_x;

		for (i = 0; i < ov.n_cells; i++) {
			Cell *cl = &ov.cells[i];
			int tw, th;

			if (cl->row != row)
				continue;
			tw = (int)(cl->live.width  * scale);
			th = (int)(cl->live.height * scale);
			if (tw < 1) tw = 1;
			if (th < 1) th = 1;

			cl->slot.x = x;
			cl->slot.y = row_y + OV_HEADER_H + (thumb_h - th) / 2;
			cl->slot.width = tw;
			cl->slot.height = th;
			x += tw + OV_CELL_GAP;
		}
	}
}

/* ── scene ──────────────────────────────────────────────────────────── */

static void
cell_apply(Cell *cell, double t)
{
	struct wlr_box b;
	int hx, hy;

	b.x      = lerp_i(cell->live.x,      cell->slot.x,      t);
	b.y      = lerp_i(cell->live.y,      cell->slot.y,      t);
	b.width  = lerp_i(cell->live.width,  cell->slot.width,  t);
	b.height = lerp_i(cell->live.height, cell->slot.height, t);
	if (b.width < 1)  b.width = 1;
	if (b.height < 1) b.height = 1;

	if (cell->thumb) {
		wlr_scene_buffer_set_dest_size(cell->thumb->buffer,
				b.width, b.height);
		wlr_scene_node_set_position(&cell->thumb->buffer->node,
				b.x, b.y);
	}
	if (cell->ghost) {
		wlr_scene_rect_set_size(cell->ghost, b.width, b.height);
		wlr_scene_node_set_position(&cell->ghost->node, b.x, b.y);
	}

	/* Header rides above the thumbnail and fades in with the zoom —
	 * at t=0 it would otherwise sit on top of the real window. */
	if (cell->header) {
		hx = b.x;
		hy = b.y - cell->header_h;
		wlr_scene_node_set_position(&cell->header->node, hx, hy);
		wlr_scene_buffer_set_opacity(cell->header, (float)(t * t));
		wlr_scene_node_set_enabled(&cell->header->node, t > 0.02);
	}

	if (cell->sel[0]) {
		int on = (cell == &ov.cells[ov.sel]) && t > 0.5;
		int i;
		for (i = 0; i < 4; i++)
			wlr_scene_node_set_enabled(&cell->sel[i]->node, on);
		if (on) {
			int w = b.width  + 2 * OV_SEL_BORDER;
			int h = b.height + 2 * OV_SEL_BORDER;
			int x = b.x - OV_SEL_BORDER, y = b.y - OV_SEL_BORDER;
			wlr_scene_rect_set_size(cell->sel[0], w, OV_SEL_BORDER);
			wlr_scene_node_set_position(&cell->sel[0]->node, x, y);
			wlr_scene_rect_set_size(cell->sel[1], w, OV_SEL_BORDER);
			wlr_scene_node_set_position(&cell->sel[1]->node, x,
					y + h - OV_SEL_BORDER);
			wlr_scene_rect_set_size(cell->sel[2], OV_SEL_BORDER, h);
			wlr_scene_node_set_position(&cell->sel[2]->node, x, y);
			wlr_scene_rect_set_size(cell->sel[3], OV_SEL_BORDER, h);
			wlr_scene_node_set_position(&cell->sel[3]->node,
					x + w - OV_SEL_BORDER, y);
		}
	}
}

static void
apply_all(void)
{
	int i;

	if (ov.backdrop) {
		wlr_scene_rect_set_size(ov.backdrop, ov.mon->m.width,
				ov.mon->m.height);
		wlr_scene_node_set_position(&ov.backdrop->node,
				ov.mon->m.x, ov.mon->m.y);
		/* Dim behind, but hard enough that the real windows do not
		 * ghost through their own thumbnails — the mirrors sit
		 * directly on top of them at t=0 and drift away as the zoom
		 * proceeds, so any transparency here reads as a double image. */
		wlr_scene_rect_set_color(ov.backdrop, (float[4]){
				0.04f, 0.05f, 0.07f, (float)(0.95 * ov.t) });
	}
	for (i = 0; i < ov.n_cells; i++)
		cell_apply(&ov.cells[i], ov.t);
}

static void
overview_teardown(void)
{
	if (ov.tree) {
		/* One destroy takes the mirrors, headers, rects and backdrop —
		 * the mirrored SURFACES belong to the clients and are
		 * untouched. */
		wlr_scene_node_destroy(&ov.tree->node);
		ov.tree = NULL;
	}
	free(ov.cells);
	ov.cells = NULL;
	ov.n_cells = ov.n_rows = ov.sel = 0;
	ov.backdrop = NULL;
	ov.open = ov.closing = 0;
	ov.mon = NULL;
	ov.t = ov.t_vel = ov.t_target = 0.0;
}

static int
build_cells(Monitor *m)
{
	Workspace *ws;
	Column *col;
	Client *c;
	int row = 0, n = 0;

	/* Pass 1: count. */
	wl_list_for_each(ws, &m->workspaces, link) {
		int ws_n = 0;
		wl_list_for_each(col, &ws->columns, link)
			wl_list_for_each(c, &col->clients, column_link)
				if (client_surface(c) && client_surface(c)->mapped)
					ws_n++;
		if (ws_n)
			n += ws_n;
	}
	if (!n)
		return 0;

	ov.cells = ecalloc((size_t)n, sizeof(Cell));
	ov.n_cells = 0;

	/* Pass 2: fill, one row per NON-EMPTY workspace so an empty slot in
	 * the middle never leaves a gap in the grid. */
	wl_list_for_each(ws, &m->workspaces, link) {
		int had = 0;

		wl_list_for_each(col, &ws->columns, link) {
			wl_list_for_each(c, &col->clients, column_link) {
				Cell *cell;

				if (!client_surface(c) ||
						!client_surface(c)->mapped)
					continue;
				cell = &ov.cells[ov.n_cells++];
				cell->c = c;
				cell->ws = ws;
				cell->row = row;
				cell->live = cell_live_box(m, c, ws);
				if (cell->live.width < 1)  cell->live.width = 1;
				if (cell->live.height < 1) cell->live.height = 1;
				had = 1;
			}
		}
		if (had)
			row++;
	}
	ov.n_rows = row;
	return ov.n_cells;
}

static void
build_scene(void)
{
	static const float sel_col[4] = { 0.35f, 0.62f, 0.95f, 1.0f };
	static const float ghost_col[4] = { 0.13f, 0.14f, 0.17f, 1.0f };
	int i, j;

	ov.tree = wlr_scene_tree_create(layers[LyrOverlay]);
	if (!ov.tree)
		return;

	ov.backdrop = wlr_scene_rect_create(ov.tree, 1, 1,
			(float[4]){ 0.04f, 0.05f, 0.07f, 0.0f });

	for (i = 0; i < ov.n_cells; i++) {
		Cell *cell = &ov.cells[i];
		struct wlr_surface *surf = client_surface(cell->c);
		struct wlr_buffer *hb;

		/* A client parked on an inactive workspace keeps its last
		 * buffer, so the mirror shows real content.  One that never
		 * drew (FREEZE-SKIP case: X11 hidden since map) has none —
		 * give it a flat card so the slot is still selectable rather
		 * than an invisible hole in the grid. */
		if (surf && surf->buffer)
			cell->thumb = wlr_scene_surface_create(ov.tree, surf);
		if (!cell->thumb)
			cell->ghost = wlr_scene_rect_create(ov.tree, 1, 1,
					ghost_col);

		/* Header spans exactly its thumbnail — layout_cells() has
		 * already run, so the slot width is known.  A fixed width
		 * would overhang the neighbouring tile on a busy row and the
		 * labels would read as belonging to the wrong window. */
		cell->header_w = cell->slot.width;
		if (cell->header_w < 120)
			cell->header_w = 120;
		cell->header_h = OV_HEADER_H - 6;
		hb = header_buffer(cell, cell->header_w, cell->header_h);
		if (hb) {
			cell->header = wlr_scene_buffer_create(ov.tree, hb);
			wlr_buffer_drop(hb);
		}

		for (j = 0; j < 4; j++) {
			cell->sel[j] = wlr_scene_rect_create(ov.tree, 1, 1,
					sel_col);
			if (cell->sel[j])
				wlr_scene_node_set_enabled(&cell->sel[j]->node, 0);
		}
	}
}

/* ── open / close ───────────────────────────────────────────────────── */

static void
overview_open(Monitor *m)
{
	Client *foc;
	int i;

	if (!m)
		return;
	if (!build_cells(m))
		return;

	ov.mon = m;
	ov.open = 1;
	ov.closing = 0;
	ov.t = 0.0;
	ov.t_vel = 0.0;
	ov.t_target = 1.0;
	ov.activate = NULL;

	/* Start selected on the tile the user was actually using, so Escape
	 * and Enter both land back where they came from. */
	foc = focustop(m);
	ov.sel = 0;
	for (i = 0; i < ov.n_cells; i++)
		if (ov.cells[i].c == foc)
			ov.sel = i;

	layout_cells();
	build_scene();
	apply_all();
	monitor_wake(m);
}

/* dir: +1 activate the selection, 0 just return to where we were. */
static void
overview_begin_close(int activate)
{
	if (!ov.open || ov.closing)
		return;
	ov.closing = 1;
	ov.t_target = 0.0;
	ov.activate = activate && ov.n_cells ? ov.cells[ov.sel].c : NULL;

	/* Zoom back INTO the chosen tile: retarget every thumbnail's origin
	 * to where it will be once we have switched to that workspace, so
	 * the animation lands exactly on the real window instead of racing
	 * it. */
	if (ov.activate) {
		Workspace *dest = ov.cells[ov.sel].ws;
		int i;
		for (i = 0; i < ov.n_cells; i++) {
			Cell *cell = &ov.cells[i];
			cell->live = cell->c->geom;
			cell->live.y += (cell->ws->idx - dest->idx) *
					ov.mon->m.height;
			if (cell->live.width < 1)  cell->live.width = 1;
			if (cell->live.height < 1) cell->live.height = 1;
		}
	}
	if (ov.mon)
		monitor_wake(ov.mon);
}

static void
overview_finish_close(void)
{
	Client *target = ov.activate;
	Monitor *m = ov.mon;

	overview_teardown();

	if (target && m) {
		Workspace *ws = target->column ? target->column->ws : NULL;
		if (ws && ws != m->active_ws) {
			workspace_switch(m, ws);
			arrange(m);
		}
		focusclient(target, 1);
		printstatus();
	}
}

/* ── public ─────────────────────────────────────────────────────────── */

int
overview_is_open(void)
{
	return ov.open;
}

void
overview_toggle(const Arg *arg)
{
	(void)arg;
	if (ov.open) {
		overview_begin_close(0);
		return;
	}
	overview_open(selmon);
}

/* Selection movement.  Left/right walk the row, up/down jump to the
 * nearest tile in the neighbouring row by horizontal centre, so the
 * cursor tracks where the eye is rather than snapping to index 0. */
static void
sel_move_horiz(int dir)
{
	int next = ov.sel + dir;

	/* Cells are stored in row order, so the neighbour in the array is
	 * the neighbour on screen — as long as it is still the same row. */
	if (next < 0 || next >= ov.n_cells)
		return;
	if (ov.cells[next].row != ov.cells[ov.sel].row)
		return;
	ov.sel = next;
}

static void
sel_move_vert(int dir)
{
	int row = ov.cells[ov.sel].row + dir;
	int cx = ov.cells[ov.sel].slot.x + ov.cells[ov.sel].slot.width / 2;
	int i, best = -1, best_d = INT_MAX;

	if (row < 0 || row >= ov.n_rows)
		return;
	for (i = 0; i < ov.n_cells; i++) {
		int c, d;
		if (ov.cells[i].row != row)
			continue;
		c = ov.cells[i].slot.x + ov.cells[i].slot.width / 2;
		d = c > cx ? c - cx : cx - c;
		if (d < best_d) {
			best_d = d;
			best = i;
		}
	}
	if (best >= 0)
		ov.sel = best;
}

int
overview_handle_key(uint32_t mods, xkb_keysym_t sym)
{
	if (!ov.open)
		return 0;

	/* Anything held with Super/Ctrl/Alt belongs to the bind table, not
	 * to the grid — otherwise the very chord that opened the overview
	 * (Super+O) would be swallowed here and only Escape could close it.
	 * The grid's own keys are all unmodified, so nothing is lost. */
	if (mods & (WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT))
		return 0;

	/* Bare modifier presses carry no intent; let them pass so the mask
	 * above can form. */
	switch (sym) {
	case XKB_KEY_Shift_L:   case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L: case XKB_KEY_Control_R:
	case XKB_KEY_Alt_L:     case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:   case XKB_KEY_Super_R:
	case XKB_KEY_Meta_L:    case XKB_KEY_Meta_R:
	case XKB_KEY_ISO_Level3_Shift:
		return 0;
	default:
		break;
	}

	/* While closing the keyboard is already handed back — swallow keys
	 * so a fast Enter+Escape can't tear down twice. */
	if (ov.closing)
		return 1;

	switch (sym) {
	case XKB_KEY_Escape:
		overview_begin_close(0);
		return 1;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		overview_begin_close(1);
		return 1;
	case XKB_KEY_Left:
	case XKB_KEY_h:
		sel_move_horiz(-1);
		break;
	case XKB_KEY_Right:
	case XKB_KEY_l:
		sel_move_horiz(+1);
		break;
	case XKB_KEY_Up:
	case XKB_KEY_k:
		sel_move_vert(-1);
		break;
	case XKB_KEY_Down:
	case XKB_KEY_j:
		sel_move_vert(+1);
		break;
	default:
		/* Everything else is swallowed: the overview is modal, and a
		 * stray keystroke must not reach the client underneath. */
		return 1;
	}
	apply_all();
	if (ov.mon)
		monitor_wake(ov.mon);
	return 1;
}

void
overview_tick(Monitor *m, double dt, int *still)
{
	if (!ov.open || m != ov.mon)
		return;

	/* Only ever RAISE *still: the caller passes the same flag through
	 * notify_tick/osd_tick/notifyd_tick, so clearing it here would
	 * cancel a toast animation that had just asked for another frame. */
	if (spring_tick(&ov.t, &ov.t_vel, ov.t_target, SPRING_OVERVIEW, dt)) {
		if (ov.t < 0.0) ov.t = 0.0;
		if (ov.t > 1.0) ov.t = 1.0;
		apply_all();
		if (still)
			*still = 1;
		return;
	}

	ov.t = ov.t_target;
	apply_all();
	if (ov.closing)
		overview_finish_close();
}

void
overview_purge_mon(Monitor *m)
{
	if (ov.open && ov.mon == m) {
		ov.activate = NULL;
		overview_teardown();
	}
}

/* A client can die while its thumbnail is on screen (crash, or the user
 * closing it from elsewhere).  The mirror node holds no reference that
 * keeps the surface alive, so the whole overview is dropped rather than
 * left pointing at freed memory. */
void
overview_purge_client(Client *c)
{
	int i;

	if (!ov.open)
		return;
	for (i = 0; i < ov.n_cells; i++) {
		if (ov.cells[i].c != c)
			continue;
		ov.activate = NULL;
		overview_teardown();
		return;
	}
}
