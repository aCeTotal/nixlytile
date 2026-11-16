#include "nixlytile.h"

#define WS_CELL_PAD 2

typedef struct {
	Workspace *at[TAGCOUNT];
	int count;
	int pos[TAGCOUNT];
	int n, active;
} WsRow;

static int
ws_index(const WsRow *row, const Workspace *ws)
{
	for (int i = 0; i < row->count; i++)
		if (row->at[i] == ws)
			return i;
	return -1;
}

/* Workspaces holding a fullscreen client. */
static uint32_t
fullscreen_mask(const WsRow *row)
{
	uint32_t mask = 0;
	Client *c;

	wl_list_for_each(c, &clients, link) {
		int i = c->fs_ws ? ws_index(row, c->fs_ws) : -1;

		if (i >= 0)
			mask |= 1u << i;
	}
	return mask;
}

/* Occupied, active and first workspaces. */
static void
collect_row(Monitor *m, WsRow *row)
{
	Workspace *ws;
	uint32_t fs;

	row->count = row->n = 0;
	row->active = -1;
	wl_list_for_each(ws, &m->workspaces, link) {
		if (row->count == TAGCOUNT)
			break;
		row->at[row->count++] = ws;
	}
	fs = fullscreen_mask(row);
	for (int i = 0; i < row->count; i++) {
		int active = row->at[i] == m->active_ws;

		if (i && !active && !workspace_has_clients(row->at[i])
				&& !(fs & (1u << i)))
			continue;
		if (active)
			row->active = row->n;
		row->pos[row->n++] = i;
	}
}

static uint64_t
row_sig(const WsRow *row, int bar_h)
{
	uint64_t sig = FNV_OFFSET;

	sig = sig_mix(sig, (uint64_t)bar_h);
	sig = sig_mix(sig, (uint64_t)row->active);
	for (int i = 0; i < row->n; i++)
		sig = sig_mix(sig, (uint64_t)row->pos[i]);
	return sig;
}

static struct nx_buffer *
row_buffer(const WsRow *row, int cell, int h)
{
	struct fcft_font *f = bar_font_get(h);
	struct PixmanBuffer *buf;
	int baseline;

	if (!f || !row->n || !(buf = bar_canvas_new(row->n * cell, h)))
		return NULL;
	baseline = (h + f->ascent - f->descent) / 2;
	for (int i = 0; i < row->n; i++) {
		char label[4];
		int w;

		snprintf(label, sizeof(label), "%d", row->pos[i] + 1);
		w = nixly_text_width(f, label);
		nixly_text_draw(buf->image, f, label, i * cell + (cell - w) / 2,
				baseline, i == row->active ? statusbar_ws_active_fg
					: statusbar_dim_fg, 0);
	}
	return &buf->base;
}

/* Numbered cells over sliding pill. */
void
renderworkspaces(Monitor *m, StatusModule *module, int bar_height)
{
	int cell = bar_height - 2 * BAR_INSET + 2 * WS_CELL_PAD;
	struct nx_buffer *buf;
	WsRow row;
	uint64_t sig;

	if (!module->tree || bar_height <= 0)
		return;
	collect_row(m, &row);
	sig = row_sig(&row, bar_height);
	if (sig == module->render_sig && module->width > 0)
		return;
	buf = row_buffer(&row, cell, bar_height);
	if (!buf || !bar_module_attach(module, buf)) {
		bar_module_clear(module);
		return;
	}
	module->render_sig = sig;
	module->box_count = row.n;
	module->active_box = row.active;
	for (int i = 0; i < row.n; i++) {
		module->box_x[i] = i * cell;
		module->box_w[i] = cell;
		module->box_tag[i] = row.pos[i];
	}
	module->width = row.n * cell;
}
