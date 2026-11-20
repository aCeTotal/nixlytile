/* Synced events and accounts cache. */
#include "nixlytile.h"
#include "calstore/calstore.h"

#include <sys/stat.h>

#define RGB_HEX_LEN 6
#define MAX_SPAN_S  (400 * (int64_t)DAY_S)
#define FILE_MAX    (32 << 20)
#define ITEMS_MAX   200000
#define POOL_STEP   4096
#define ITEMS_STEP  256

enum { EV_START, EV_END, EV_FLAGS, EV_COLOR, EV_TITLE, EV_WHERE, EV_CAL,
	EV_REF, EV_FIELDS };

static CalItem *items;
static size_t nitems, items_cap;
static uint32_t *tasks;
static size_t ntasks;
static char *pool;
static size_t pool_len, pool_cap;
static int64_t max_span;

FILE *
cache_open(const char *name)
{
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	char path[PATH_MAX];
	struct stat st;
	FILE *fp;

	if (xdg && xdg[0] == '/')
		snprintf(path, sizeof(path), "%s/nixlytile/calendar/%s", xdg, name);
	else
		snprintf(path, sizeof(path), "%s/.cache/nixlytile/calendar/%s",
				home ? home : "", name);
	fp = fopen(path, "re");
	if (!fp || (fstat(fileno(fp), &st) == 0 && st.st_size <= FILE_MAX))
		return fp;
	fclose(fp);
	return NULL;
}

static uint32_t
pool_add(const char *s)
{
	size_t n = strlen(s) + 1;
	uint32_t off = (uint32_t)pool_len;

	if (pool_len + n > pool_cap) {
		pool_cap = MAX(pool_cap * 2, pool_len + n + POOL_STEP);
		pool = erealloc(pool, pool_cap);
	}
	memcpy(pool + pool_len, s, n);
	pool_len += n;
	return off;
}

Fields
split_tabs(char *line)
{
	Fields f = { .n = 1 };

	line[strcspn(line, "\n")] = '\0';
	f.v[0] = line;
	for (char *p = line; *p && f.n < FIELDS_MAX; p++) {
		if (*p != '\t')
			continue;
		*p = '\0';
		f.v[f.n++] = p + 1;
	}
	return f;
}

static void
add_item(char *const *f)
{
	CalItem it = { 0 };
	char *end;

	it.color = (uint32_t)strtoul(f[EV_COLOR], &end, 16);
	it.start = strtoll(f[EV_START], NULL, 10);
	it.end = strtoll(f[EV_END], NULL, 10);
	it.flags = (unsigned)strtoul(f[EV_FLAGS], NULL, 10);
	if (*end || strlen(f[EV_COLOR]) != RGB_HEX_LEN || it.end < it.start)
		return;
	if (nitems == items_cap) {
		items_cap = MAX(items_cap * 2, ITEMS_STEP);
		items = erealloc(items, items_cap * sizeof(*items));
	}
	it.title = pool_add(f[EV_TITLE]);
	it.where = pool_add(f[EV_WHERE]);
	it.cal = pool_add(f[EV_CAL]);
	it.ref = pool_add(f[EV_REF]);
	items[nitems++] = it;
}

static int
by_start(const void *a, const void *b)
{
	const CalItem *x = a, *y = b;

	if (x->start != y->start)
		return x->start < y->start ? -1 : 1;
	return (x->end > y->end) - (x->end < y->end);
}

static void
index_items(void)
{
	ntasks = 0;
	max_span = 0;
	tasks = erealloc(tasks, MAX(nitems, 1) * sizeof(*tasks));
	for (size_t i = 0; i < nitems; i++) {
		int64_t span = MIN(items[i].end - items[i].start, MAX_SPAN_S);

		if (items[i].flags & CAL_TASK)
			tasks[ntasks++] = (uint32_t)i;
		max_span = MAX(max_span, span);
	}
}

static void
read_events(FILE *fp)
{
	char *line = NULL;
	size_t cap = 0;

	while (nitems < ITEMS_MAX && getline(&line, &cap, fp) > 0) {
		Fields f = split_tabs(line);

		if (f.n == EV_FIELDS)
			add_item(f.v);
	}
	free(line);
}

void
store_load(void)
{
	FILE *fp;

	nitems = 0;
	pool_len = 0;
	if ((fp = cache_open("events"))) {
		read_events(fp);
		fclose(fp);
	}
	accounts_load();
	qsort(items, nitems, sizeof(*items), by_start);
	index_items();
	remind_arm();
}

size_t
store_count(void)
{
	return nitems;
}

const CalItem *
store_item(size_t i)
{
	return &items[i];
}

const char *
store_str(uint32_t off)
{
	return pool + off;
}

size_t
store_after(int64_t t)
{
	size_t lo = 0, hi = nitems;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (items[mid].start <= t)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

size_t
store_seek(int64_t t)
{
	size_t lo = store_after(t - max_span - 1);

	while (lo < nitems && !items[lo].start && !items[lo].end)
		lo++;
	return lo;
}

size_t
store_task_count(void)
{
	return ntasks;
}

const CalItem *
store_task(size_t i)
{
	return &items[tasks[i]];
}

static void
mark(DayMarks *m, uint32_t color)
{
	for (int i = 0; i < m->n; i++)
		if (m->color[i] == color)
			return;
	if (m->n < DOTS)
		m->color[m->n++] = color;
}

void
store_marks(int first, DayMarks *out, int n)
{
	int64_t from = day_start(first), to = day_start(first + n);

	memset(out, 0, (size_t)n * sizeof(*out));
	for (size_t i = store_seek(from); i < nitems && items[i].start < to; i++) {
		const CalItem *it = &items[i];
		int d0 = MAX(day_of(it->start), first);
		int d1 = MIN(day_of(it->end > it->start ? it->end - 1 : it->start),
				first + n - 1);

		for (int d = d0; d <= d1; d++)
			mark(&out[d - first], it->color);
	}
}
