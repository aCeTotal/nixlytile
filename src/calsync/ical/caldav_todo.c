/* CalDAV to-do writes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>

#include "dav.h"

#define UID_BYTES  16
#define UID_LEN    (2 * UID_BYTES + sizeof UID_SUFFIX)
#define UID_SUFFIX "@nixlytile"
#define ICS_EXT    ".ics"
#define ICS_CTYPE  "text/calendar; charset=utf-8"

static int
put_status(SyncRun *run, const HttpRes *res)
{
	switch (res->status) {
	case HTTP_OK:
	case HTTP_CREATED:
	case HTTP_NO_CONTENT:
		return 0;
	case HTTP_PRECONDITION:
		return dav_refuse(run, "To-do changed elsewhere — try again");
	default:
		return dav_fail(run, res);
	}
}

static int
send_put(const Account *a, HttpReq q, SyncRun *run)
{
	HttpRes res;
	int rc;

	q.method = "PUT";
	q.ctype = ICS_CTYPE;
	res = dav_send(a, q);
	rc = put_status(run, &res);
	http_res_free(&res);
	return rc;
}

int
caldav_todo_add(Account *a, const TodoNew *t, SyncRun *run)
{
	unsigned char raw[UID_BYTES];
	char uid[UID_LEN], url[sizeof a->todo_url + UID_LEN + sizeof ICS_EXT];
	size_t n = strlen(a->todo_url);
	char *ics;
	int rc;

	if (!n)
		return dav_refuse(run, "No to-do list on this account");
	if (getrandom(raw, sizeof raw, 0) != (ssize_t)sizeof raw)
		return dav_refuse(run, "No randomness available");
	for (size_t i = 0; i < sizeof raw; i++)
		sprintf(uid + 2 * i, "%02x", raw[i]);
	strcpy(uid + 2 * sizeof raw, UID_SUFFIX);
	snprintf(url, sizeof url, "%s%s%s" ICS_EXT, a->todo_url,
			a->todo_url[n - 1] == '/' ? "" : "/", uid);
	ics = ical_todo_new(t, uid);
	if (!ics)
		return dav_refuse(run, "Out of memory");
	rc = send_put(a, (HttpReq){ .url = url, .body = ics, .if_none_match = "*" },
			run);
	free(ics);
	return rc;
}

/* url lives on base's server. */
static int
same_origin(const char *base, const char *url)
{
	char *origin = http_resolve(base, "/");
	int same = origin && !strncmp(url, origin, strlen(origin));

	free(origin);
	return same;
}

int
caldav_todo_done(Account *a, const char *ref, SyncRun *run)
{
	HttpReq q = { .method = "GET", .url = ref };
	HttpRes got;
	char *ics;
	int rc;

	if (!same_origin(a->home, ref))
		return dav_refuse(run, "To-do is not on this account");
	got = dav_send(a, q);
	if (got.status != HTTP_OK) {
		rc = dav_fail(run, &got);
		http_res_free(&got);
		return rc;
	}
	ics = ical_todo_complete(got.body, time(NULL));
	q.body = ics;
	q.if_match = got.etag[0] ? got.etag : NULL;
	rc = ics ? send_put(a, q, run) : dav_refuse(run, "Unreadable to-do on the server");
	free(ics);
	http_res_free(&got);
	return rc;
}
