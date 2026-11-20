/* Top-left hot corner. */
#include "overview/state.h"

#define CORNER_PX 2
#define REARM_PX  48

static int armed;

static int
near_corner(const Monitor *m, double x, double y, int px)
{
	return x < m->m.x + px && y < m->m.y + px;
}

static int
overlap(int a, int a_len, int b, int b_len)
{
	return a < b + b_len && b < a + a_len;
}

/* Other lies left or above. */
static int
shadows(const Monitor *other, const Monitor *m)
{
	struct nx_box o = other->m, b = m->m;

	if (o.x + o.width <= b.x && overlap(o.y, o.height, b.y, b.height))
		return 1;
	return o.y + o.height <= b.y && overlap(o.x, o.width, b.x, b.width);
}

static int
outer_corner(const Monitor *m)
{
	const Monitor *other;

	wl_list_for_each(other, &mons, link)
		if (other != m && shadows(other, m))
			return 0;
	return 1;
}

static int
blocked(Monitor *m)
{
	return locked || cursor_mode != CurNormal || active_constraint
			|| fullscreen_visible_on(m) || !outer_corner(m);
}

void
overview_corner(Monitor *m, double x, double y)
{
	if (!m)
		return;
	if (!near_corner(m, x, y, REARM_PX))
		armed = 1;
	if (!armed || !near_corner(m, x, y, CORNER_PX) || blocked(m))
		return;
	armed = 0;
	selmon = m;
	overview_toggle(NULL);
}
