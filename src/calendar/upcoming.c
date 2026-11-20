/* Agenda candidates from a day. */
#include "calendar/view.h"

#define CAND_STEP 64

static Entry *cand;
static int ncand, cand_cap;

static void
push(Entry e)
{
	if (ncand == cand_cap) {
		cand_cap += CAND_STEP;
		cand = erealloc(cand, (size_t)cand_cap * sizeof(*cand));
	}
	e.seq = ncand;
	cand[ncand++] = e;
}

static int
ended(const CalItem *it, int64_t t)
{
	return it->start == it->end ? it->start < t : it->end <= t;
}

static int
is_task(const CalItem *it)
{
	return (it->flags & CAL_TASK) != 0;
}

/* Overdue to-dos lead today. */
static void
collect_overdue(int now_day, int64_t now, int need)
{
	int got = 0;

	for (size_t i = 0; i < store_task_count() && got < need; i++) {
		const CalItem *it = store_task(i);

		if (it->start >= now)
			return;
		if (!ended(it, now) || todo_hidden(store_str(it->ref)))
			continue;
		push((Entry){ .day = now_day, .task = 1, .start = it->start,
				.it = it });
		got++;
	}
}

/* Completes its last day. */
static void
collect_items(int64_t from, int need)
{
	int got = 0, last = 0;

	for (size_t i = store_seek(from); i < store_count(); i++) {
		const CalItem *it = store_item(i);
		int day = MAX(day_of(it->start), ui.sel);

		if (got >= need && day != last)
			return;
		if (ended(it, from) || todo_hidden(store_str(it->ref)))
			continue;
		push((Entry){ .day = day, .task = is_task(it), .start = it->start,
				.it = it });
		got++;
		last = day;
	}
}

static void
collect_pending(void)
{
	for (int i = 0; i < todo_pending_count(); i++) {
		const PendingTodo *p = todo_pending(i);

		if (p->day >= ui.sel)
			push((Entry){ .day = p->day, .task = 1,
					.start = day_start(p->day), .pend = p });
	}
}

static int
by_day(const void *a, const void *b)
{
	const Entry *x = a, *y = b;

	if (x->day != y->day)
		return x->day < y->day ? -1 : 1;
	if (x->task != y->task)
		return x->task - y->task;
	if (x->start != y->start)
		return x->start < y->start ? -1 : 1;
	return x->seq - y->seq;
}

/* Sorted candidates from selection. */
Upcoming
upcoming(int64_t now, int need)
{
	int now_day = day_of(now);
	int64_t from = ui.sel == now_day ? now : day_start(ui.sel);

	ncand = 0;
	if (ui.sel == now_day)
		collect_overdue(now_day, now, need);
	collect_items(from, need);
	collect_pending();
	qsort(cand, (size_t)ncand, sizeof(*cand), by_day);
	return (Upcoming){ cand, ncand };
}
