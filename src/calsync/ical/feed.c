/* ICS subscription feeds. */
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "calsync.h"

#define ETAG_TAG          "e:"
#define HASH_TAG_FMT      "h:%016llx"
#define META_LINE_MAX     512

typedef struct {
	char name[64];
	uint32_t color;
	int has_color;
} FeedMeta;

typedef struct {
	Account *a;
	SyncRun *run;
	Source src;
	HttpRes res;
	int full;
} Feed;

static int
feed_fail(SyncRun *run, const HttpRes *res)
{
	switch (res->status) {
	case 0:
		run_fail(run, "Offline: %s", res->error);
		return -1;
	case HTTP_UNAUTHORIZED:
	case HTTP_FORBIDDEN:
		run_fail(run, "Feed needs sign-in — use the calendar's secret iCal address");
		return -1;
	case HTTP_NOT_FOUND:
		run_fail(run, "Feed not found");
		return -1;
	default:
		run_fail(run, "Feed: HTTP %ld", res->status);
		return -1;
	}
}

static const char *
cached_etag(const Feed *f)
{
	if (f->full || strncmp(f->src.tag, ETAG_TAG, sizeof ETAG_TAG - 1))
		return NULL;
	return f->src.tag + sizeof ETAG_TAG - 1;
}

/* Server ETag, else body hash. */
static void
body_tag(char *dst, size_t len, const HttpRes *res)
{
	int n = res->etag[0] ? snprintf(dst, len, ETAG_TAG "%s", res->etag) : -1;

	if (n < 0 || (size_t)n >= len)
		snprintf(dst, len, HASH_TAG_FMT, (unsigned long long)fnv64(res->body));
}

static void
host_label(Account *a)
{
	CURLU *u = curl_url();
	char *host = NULL;

	if (u && !curl_url_set(u, CURLUPART_URL, a->url, 0)
			&& !curl_url_get(u, CURLUPART_HOST, &host, 0))
		snprintf(a->label, sizeof a->label, "%s", host);
	curl_free(host);
	curl_url_cleanup(u);
}

static size_t
line_break(const char *p)
{
	if (p[0] == '\r' && p[1] == '\n')
		return 2;
	return p[0] == '\n';
}

/* Next logical line, unfolded. */
static const char *
unfold(const char *p, char *line, size_t cap)
{
	size_t n = 0, brk;

	for (; *p; p++) {
		brk = line_break(p);
		if (brk && p[brk] != ' ' && p[brk] != '\t')
			break;
		if (brk) {
			p += brk;
			continue;
		}
		if (n + 1 < cap)
			line[n++] = *p;
	}
	line[n] = '\0';
	return p + line_break(p);
}

static const char *
value_of(const char *line, const char *name)
{
	size_t n = strlen(name);

	if (strncasecmp(line, name, n) || (line[n] != ':' && line[n] != ';'))
		return NULL;
	line = strchr(line + n, ':');
	return line ? line + 1 : NULL;
}

/* Name and color before components. */
static FeedMeta
feed_meta(const char *text)
{
	FeedMeta m = { 0 };
	char line[META_LINE_MAX];
	const char *v;

	while (*text) {
		text = unfold(text, line, sizeof line);
		if (!strcasecmp(line, "BEGIN:VEVENT") || !strcasecmp(line, "BEGIN:VTODO"))
			break;
		if ((v = value_of(line, "X-WR-CALNAME")))
			event_text(m.name, sizeof m.name, v);
		if ((v = value_of(line, "X-APPLE-CALENDAR-COLOR")))
			m.has_color = !parse_color(v, &m.color);
	}
	return m;
}

static int
absorb(Feed *f)
{
	char tag[sizeof f->src.tag];
	FeedMeta fm;
	CalMeta meta;
	EventList fresh = { 0 };

	body_tag(tag, sizeof tag, &f->res);
	if (!f->full && !strcmp(tag, f->src.tag))
		return 0;
	fm = feed_meta(f->res.body);
	if (!f->a->label[0] && fm.name[0])
		snprintf(f->a->label, sizeof f->a->label, "%s", fm.name);
	if (!f->a->label[0])
		host_label(f->a);
	meta.name = fm.name[0] ? fm.name : f->a->label;
	meta.color = fm.has_color ? fm.color : palette_color(meta.name);
	if (ical_expand(f->res.body, &meta, &f->run->w, &fresh) < 0) {
		events_free(&fresh);
		run_fail(f->run, "Not a valid calendar feed");
		return -1;
	}
	source_set_tag(&f->src, tag);
	source_replace(&f->src, &fresh, &f->run->w);
	return 0;
}

int
feed_sync(Account *a, SyncRun *run)
{
	Feed f = { .a = a, .run = run };
	char id[sizeof "feed:" + sizeof a->url];
	int rc = 0;

	snprintf(id, sizeof id, "feed:%s", a->url);
	source_open(&f.src, a, id);
	f.full = source_full_due(&f.src, &run->w);
	f.res = http_send(&(HttpReq){ .method = "GET", .url = a->url,
			.if_none_match = cached_etag(&f) });
	if (f.res.status == HTTP_OK)
		rc = absorb(&f);
	else if (f.res.status != HTTP_NOT_MODIFIED)
		rc = feed_fail(run, &f.res);
	source_close(&f.src, run);
	http_res_free(&f.res);
	a->ncals = 1;
	return rc;
}
