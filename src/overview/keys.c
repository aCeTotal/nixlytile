/* Keys while overview is open. */
#include "overview/state.h"

#define SIDE_WEIGHT 2 /* off-axis distance counts double */

typedef struct {
	void (*bind)(const Arg *);
	void (*act)(int dir);
} Route;

static const struct {
	xkb_keysym_t sym;
	int dx, dy;
} steps[] = {
	{ XKB_KEY_Left,  -1,  0 }, { XKB_KEY_h, -1,  0 },
	{ XKB_KEY_Right,  1,  0 }, { XKB_KEY_l,  1,  0 },
	{ XKB_KEY_Up,     0, -1 }, { XKB_KEY_k,  0, -1 },
	{ XKB_KEY_Down,   0,  1 }, { XKB_KEY_j,  0,  1 },
};

static Client *
picked(void)
{
	return ov.cells[ov.sel].c;
}

/* Nearest cell centre that way. */
void
select_toward(int dx, int dy)
{
	struct nx_box at = ov.cells[ov.sel].slot;
	long best_score = LONG_MAX;
	int best = -1;

	for (int i = 0; i < ov.n_cells; i++) {
		struct nx_box b = ov.cells[i].slot;
		long ox = (b.x + b.width / 2) - (at.x + at.width / 2);
		long oy = (b.y + b.height / 2) - (at.y + at.height / 2);
		long ahead = ox * dx + oy * dy;
		long score = ahead + SIDE_WEIGHT * (labs(ox * dy) + labs(oy * dx));

		if (ahead <= 0 || score >= best_score)
			continue;
		best = i;
		best_score = score;
	}
	if (best >= 0)
		overview_select(best);
}

static void
select_columns(int dir)
{
	select_toward(dir, 0);
}

static void
select_rows(int dir)
{
	select_toward(0, dir);
}

static void
move_sideways(int dir)
{
	if (column_shift(picked()->column, dir))
		overview_reflow();
}

static void
move_in_stack(int dir)
{
	if (column_shift_client(picked(), dir))
		overview_reflow();
}

static void
move_to_workspace(int dir)
{
	Client *c = picked();
	Workspace *dst = workspace_neighbour(c, dir);

	if (!dst)
		return;
	workspace_detach_client(c);
	workspace_attach_client(dst, c);
	monitor_compact_workspaces(ov.mon);
	overview_reflow();
}

static const Route routes[] = {
	{ focus_column_dir,           select_columns },
	{ focus_window_in_column_dir, select_rows },
	{ focus_workspace_dir,        select_rows },
	{ move_column_dir,            move_sideways },
	{ swap_window_dir,            move_sideways },
	{ move_window_in_column_dir,  move_in_stack },
	{ move_client_to_ws_dir,      move_to_workspace },
};

static const Route *
route_of(const Key *k)
{
	for (size_t i = 0; i < LENGTH(routes); i++)
		if (routes[i].bind == k->func)
			return &routes[i];
	return NULL;
}

int
overview_bind(const Key *k)
{
	const Route *r = ov.mon ? route_of(k) : NULL;

	if (!r)
		return 0;
	if (!ov.closing && overview_revalidate())
		r->act(k->arg.i);
	return 1;
}

static int
is_modifier(xkb_keysym_t sym)
{
	switch (sym) {
	case XKB_KEY_Shift_L:   case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L: case XKB_KEY_Control_R:
	case XKB_KEY_Alt_L:     case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L:   case XKB_KEY_Super_R:
	case XKB_KEY_Meta_L:    case XKB_KEY_Meta_R:
	case XKB_KEY_ISO_Level3_Shift:
		return 1;
	default:
		return 0;
	}
}

/* Unmodified keys; chords reach binds. */
int
overview_handle_key(uint32_t mods, xkb_keysym_t sym)
{
	if (!ov.mon || mods & (NX_MODIFIER_LOGO | NX_MODIFIER_CTRL | NX_MODIFIER_ALT)
			|| is_modifier(sym))
		return 0;
	if (ov.closing)
		return 1;
	if (sym == XKB_KEY_Escape)
		overview_cancel();
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)
		overview_choose();
	for (size_t i = 0; i < LENGTH(steps); i++)
		if (steps[i].sym == sym)
			select_toward(steps[i].dx, steps[i].dy);
	return 1;
}
