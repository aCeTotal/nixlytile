/* WebDAV requests and home discovery. */
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dav.h"

#define HOME_TRIES  2
#define XML_OPTIONS (XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING)
#define XML_CTYPE   "application/xml; charset=utf-8"
#define WELL_KNOWN  "/.well-known/caldav"
#define NO_SERVICE  "No CalDAV service found at this address"
#define NS_CS       "http://calendarserver.org/ns/"
#define NS_IC       "http://apple.com/ns/ical/"

static const char *const namespaces[][2] = {
	{ "d", NS_DAV }, { "c", NS_CALDAV }, { "cs", NS_CS }, { "ic", NS_IC },
};

static const char principal_body[] = "<d:propfind xmlns:d=\"" NS_DAV "\">"
	"<d:prop><d:current-user-principal/></d:prop></d:propfind>";
static const char home_body[] = "<d:propfind xmlns:d=\"" NS_DAV "\" xmlns:c=\""
	NS_CALDAV "\"><d:prop><c:calendar-home-set/></d:prop></d:propfind>";
static const char list_body[] = "<d:propfind xmlns:d=\"" NS_DAV "\" xmlns:c=\""
	NS_CALDAV "\" xmlns:cs=\"" NS_CS "\" xmlns:ic=\"" NS_IC "\"><d:prop>"
	"<d:resourcetype/><d:displayname/><cs:getctag/><d:sync-token/>"
	"<ic:calendar-color/><c:supported-calendar-component-set/>"
	"</d:prop></d:propfind>";

int
dav_refuse(SyncRun *run, const char *why)
{
	run_fail(run, "%s", why);
	return -1;
}

int
dav_fail(SyncRun *run, const HttpRes *res)
{
	switch (res->status) {
	case 0:
		run_fail(run, "Offline: %s", res->error);
		return -1;
	case HTTP_UNAUTHORIZED:
		return dav_refuse(run, "Wrong user name or app password");
	default:
		run_fail(run, "CalDAV: HTTP %ld", res->status);
		return -1;
	}
}

static int
fatal(const HttpRes *res)
{
	return !res->status || res->status == HTTP_UNAUTHORIZED;
}

HttpRes
dav_send(const Account *a, HttpReq q)
{
	q.user = a->user;
	q.pass = a->secret;
	return http_send(&q);
}

/* Redirects embed credentials here. */
static void
strip_userinfo(char *url, size_t len)
{
	CURLU *u = curl_url();
	char *clean = NULL;

	if (u && !curl_url_set(u, CURLUPART_URL, url, 0)
			&& !curl_url_set(u, CURLUPART_USER, NULL, 0)
			&& !curl_url_set(u, CURLUPART_PASSWORD, NULL, 0))
		curl_url_get(u, CURLUPART_URL, &clean, 0);
	snprintf(url, len, "%s", clean ? clean : "");
	curl_free(clean);
	curl_url_cleanup(u);
}

void
dav_reset(Dav *d)
{
	xmlXPathFreeContext(d->xp);
	xmlFreeDoc(d->doc);
	http_res_free(&d->res);
	d->xp = NULL;
	d->doc = NULL;
}

/* Send q, parse the multistatus. */
int
dav_query(Dav *d, HttpReq q)
{
	dav_reset(d);
	q.ctype = XML_CTYPE;
	d->res = dav_send(d->a, q);
	strip_userinfo(d->res.url, sizeof d->res.url);
	if (d->res.status != HTTP_MULTISTATUS)
		return dav_fail(d->run, &d->res);
	d->doc = xmlReadMemory(d->res.body, (int)d->res.len, NULL, NULL, XML_OPTIONS);
	d->xp = d->doc ? xmlXPathNewContext(d->doc) : NULL;
	if (!d->xp)
		return dav_refuse(d->run, "Unreadable reply from the server");
	for (size_t i = 0; i < sizeof namespaces / sizeof *namespaces; i++)
		xmlXPathRegisterNs(d->xp, BAD_CAST namespaces[i][0],
				BAD_CAST namespaces[i][1]);
	return 0;
}

xmlChar *
dav_text(const Dav *d, xmlNodePtr at, const char *expr)
{
	xmlXPathObjectPtr o = xmlXPathNodeEval(at, BAD_CAST expr, d->xp);
	xmlNodeSetPtr set = o ? o->nodesetval : NULL;
	xmlChar *s = xmlXPathNodeSetGetLength(set)
			? xmlNodeGetContent(xmlXPathNodeSetItem(set, 0)) : NULL;

	xmlXPathFreeObject(o);
	return s;
}

int
dav_count(const Dav *d, xmlNodePtr at, const char *expr)
{
	xmlXPathObjectPtr o = xmlXPathNodeEval(at, BAD_CAST expr, d->xp);
	int n = o ? xmlXPathNodeSetGetLength(o->nodesetval) : 0;

	xmlXPathFreeObject(o);
	return n;
}

char *
dav_href(const Dav *d, xmlNodePtr at, const char *expr)
{
	xmlChar *href = dav_text(d, at, expr);
	char *url = href ? http_resolve(d->res.url, (const char *)href) : NULL;

	xmlFree(href);
	return url;
}

static char *
principal_at(Dav *d, const char *url)
{
	HttpReq q = { .method = "PROPFIND", .url = url, .depth = "0",
			.body = principal_body };

	if (dav_query(d, q) < 0)
		return NULL;
	return dav_href(d, xmlDocGetRootElement(d->doc),
			"//d:current-user-principal/d:href");
}

static char *
find_principal(Dav *d)
{
	char *p = principal_at(d, d->a->url);
	char *known;

	if (p || fatal(&d->res))
		return p;
	known = http_resolve(d->a->url, WELL_KNOWN);
	p = known ? principal_at(d, known) : NULL;
	free(known);
	if (!p && !fatal(&d->res))
		dav_refuse(d->run, NO_SERVICE);
	return p;
}

static int
discover(Dav *d)
{
	char *p = find_principal(d);
	HttpReq q = { .method = "PROPFIND", .url = p, .depth = "0",
			.body = home_body };
	char *home = NULL;

	if (p && !dav_query(d, q))
		home = dav_href(d, xmlDocGetRootElement(d->doc),
				"//c:calendar-home-set/d:href");
	free(p);
	if (home && strlen(home) < sizeof d->a->home)
		strcpy(d->a->home, home);
	free(home);
	if (!d->a->home[0] && d->res.status == HTTP_MULTISTATUS)
		dav_refuse(d->run, NO_SERVICE);
	return d->a->home[0] ? 0 : -1;
}

static int
stale(long status)
{
	return status >= HTTP_CLIENT_ERROR && status < HTTP_SERVER_ERROR
		&& status != HTTP_UNAUTHORIZED;
}

/* Stale homes are rediscovered once. */
int
dav_list_home(Dav *d)
{
	HttpReq q = { .method = "PROPFIND", .depth = "1", .body = list_body };

	for (int tries = 0; tries < HOME_TRIES; tries++) {
		if (!d->a->home[0] && discover(d) < 0)
			return -1;
		q.url = d->a->home;
		if (!dav_query(d, q))
			return 0;
		if (!stale(d->res.status))
			return -1;
		d->a->home[0] = '\0';
	}
	return -1;
}
