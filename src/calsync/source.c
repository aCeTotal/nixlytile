/* Per-calendar event caches on disk. */
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "calsync.h"

#define FILE_MODE  0600
#define HEADER_MAX (sizeof ((Source *)0)->tag + 32)

static void
source_path(char *dst, const char *file)
{
	snprintf(dst, PATH_MAX, "%s/%s", dir_sources(), file);
}

/* Header is "#\t<full_at>\t<tag>". */
static void
read_header(Source *s, const char *path)
{
	char line[HEADER_MAX];
	FILE *f = fopen(path, "re");
	char *tag;
	int ok;

	if (!f)
		return;
	ok = fgets(line, sizeof line, f) && line[0] == '#' && line[1] == '\t';
	fclose(f);
	if (!ok)
		return;
	s->full_at = strtoll(line + 2, &tag, 10);
	if (*tag != '\t') {
		s->full_at = 0;
		return;
	}
	tag[strcspn(tag, "\n")] = '\0';
	snprintf(s->tag, sizeof s->tag, "%s", tag + 1);
}

void
source_open(Source *s, const Account *a, const char *remote_id)
{
	char path[PATH_MAX];

	memset(s, 0, sizeof *s);
	snprintf(s->file, sizeof s->file, "%s-%016llx", a->id,
			(unsigned long long)fnv64(remote_id));
	source_path(path, s->file);
	read_header(s, path);
	events_load(&s->ev, path);
}

int
source_full_due(const Source *s, const SyncWindow *w)
{
	return !s->full_at || w->now - s->full_at >= FULL_REFRESH_S
			|| w->now < s->full_at;
}

/* Zero-length events match their instant. */
static int
overlaps(const CalEvent *e, int64_t from, int64_t to)
{
	if (e->start == e->end)
		return e->start >= from && e->start < to;
	return e->end > from && e->start < to;
}

/* Server rule: end after from. */
static int
near(const CalEvent *e, const SyncWindow *w)
{
	return e->end > w->near_from && e->start < w->near_to;
}

/* Adopt sorted fresh, flag changes. */
void
source_take(Source *s, EventList *fresh)
{
	events_sort(fresh);
	if (!events_equal(&s->ev, fresh))
		s->changed = s->dirty = 1;
	events_free(&s->ev);
	s->ev = *fresh;
	memset(fresh, 0, sizeof *fresh);
}

/* Fresh owns the near window. */
void
source_splice(Source *s, EventList *fresh, const SyncWindow *w)
{
	size_t n = 0;

	for (size_t i = 0; i < fresh->n; i++)
		if (near(&fresh->v[i], w))
			memmove(&fresh->v[n++], &fresh->v[i], sizeof *fresh->v);
	fresh->n = n;
	for (size_t i = 0; i < s->ev.n; i++)
		if (!near(&s->ev.v[i], w))
			memcpy(events_push(fresh), &s->ev.v[i], sizeof *s->ev.v);
	source_take(s, fresh);
}

void
source_replace(Source *s, EventList *fresh, const SyncWindow *w)
{
	source_take(s, fresh);
	s->full_at = w->now;
	s->dirty = 1;
}

/* Tags stay one header line. */
void
source_set_tag(Source *s, const char *tag)
{
	char clean[sizeof s->tag];

	event_text(clean, sizeof clean, tag);
	if (!strcmp(s->tag, clean))
		return;
	memcpy(s->tag, clean, sizeof clean);
	s->dirty = 1;
}

/* Failure only forces a refetch. */
static void
source_write(const Source *s)
{
	char head[HEADER_MAX], path[PATH_MAX];
	char *text;

	snprintf(head, sizeof head, "#\t%lld\t%s", (long long)s->full_at, s->tag);
	text = events_format(&s->ev, head);
	source_path(path, s->file);
	file_replace(path, text, strlen(text), FILE_MODE);
	free(text);
}

void
source_close(Source *s, SyncRun *run)
{
	if (s->dirty)
		source_write(s);
	if (run->ntouched < TOUCHED_MAX)
		snprintf(run->touched[run->ntouched++], sizeof run->touched[0],
				"%s", s->file);
	run->changed |= s->changed;
	events_free(&s->ev);
}

static int
was_touched(const SyncRun *run, const char *file)
{
	for (int i = 0; i < run->ntouched; i++)
		if (!strcmp(run->touched[i], file))
			return 1;
	return 0;
}

static int
owned_by(const char *file, const char *account_id)
{
	size_t n = strlen(account_id);

	return !strncmp(file, account_id, n) && file[n] == '-';
}

/* Spare in-flight dotted temp files. */
static int
spared(const char *file, const SyncRun *keep)
{
	return keep && (strchr(file, '.') || was_touched(keep, file));
}

/* Unlink account files not kept. */
static int
sweep(const char *account_id, const SyncRun *keep)
{
	DIR *d = opendir(dir_sources());
	struct dirent *de;
	int n = 0;

	if (!d)
		return 0;
	while ((de = readdir(d))) {
		if (!owned_by(de->d_name, account_id) || spared(de->d_name, keep))
			continue;
		n += !unlinkat(dirfd(d), de->d_name, 0);
	}
	closedir(d);
	return n;
}

int
sources_gc(const Account *a, const SyncRun *run)
{
	if (run->ntouched >= TOUCHED_MAX)
		return 0;
	return sweep(a->id, run);
}

void
sources_drop(const char *account_id)
{
	sweep(account_id, NULL);
}

static int
known(const Vault *v, const char *file)
{
	for (int i = 0; i < v->nacc; i++)
		if (owned_by(file, v->acc[i].id))
			return 1;
	return 0;
}

/* Load sources, unlink removed accounts'. */
static int
gather(const Vault *v, EventList *all)
{
	DIR *d = opendir(dir_sources());
	struct dirent *de;
	char path[PATH_MAX];

	if (!d)
		return -1;
	while ((de = readdir(d))) {
		if (strchr(de->d_name, '.'))
			continue;
		if (!known(v, de->d_name)) {
			unlinkat(dirfd(d), de->d_name, 0);
			continue;
		}
		source_path(path, de->d_name);
		events_load(all, path);
	}
	closedir(d);
	return 0;
}

static void
keep_window(EventList *l, const SyncWindow *w)
{
	size_t n = 0;

	for (size_t i = 0; i < l->n; i++) {
		const CalEvent *e = &l->v[i];

		if (!(e->flags & EV_TASK) && !overlaps(e, w->full_from, w->full_to))
			continue;
		memmove(&l->v[n++], e, sizeof *e);
	}
	l->n = n;
}

/* Caller holds the vault lock. */
int
sources_merge(const Vault *v, const SyncWindow *w)
{
	EventList all = { 0 };
	char path[PATH_MAX];
	char *text;
	int rc;

	if (gather(v, &all) < 0)
		return -1;
	local_todos(v, &all);
	keep_window(&all, w);
	events_sort(&all);
	text = events_format(&all, NULL);
	snprintf(path, sizeof path, "%s/events", dir_cache());
	rc = file_replace(path, text, strlen(text), FILE_MODE);
	free(text);
	events_free(&all);
	return rc;
}
