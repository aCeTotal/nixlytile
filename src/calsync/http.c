/* Blocking HTTP, one reused handle. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <curl/curl.h>
#include "calsync.h"

#define USER_AGENT        "nixly-calsync/1"
#define WEB_PROTOCOLS     "https,http"
#define BODY_MIN          ((size_t)16 << 10)
#define BODY_MAX          ((size_t)64 << 20)
#define CONNECT_TIMEOUT_S 10L
#define TIMEOUT_S         60L
#define REDIRECTS_MAX     5L
#define HEADER_LINE_MAX   4352
#define ESC_WIDTH         3
#define UNRESERVED        "-._~"
#define STATUS_PREFIX     "HTTP/"
#define ETAG_PREFIX       "ETag:"
#define TLS_PREFIX        "https://"
#define LENGTH(a)         (sizeof (a) / sizeof *(a))

typedef struct {
	HttpRes *res;
	size_t cap;
	int overflow;
} Sink;

static CURL *curl;

void
http_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl = curl_easy_init();
}

void
http_cleanup(void)
{
	curl_easy_cleanup(curl);
	curl_global_cleanup();
}

static void
reserve(Sink *s, size_t need)
{
	if (need <= s->cap)
		return;
	s->cap = s->cap ? s->cap : BODY_MIN;
	while (s->cap < need)
		s->cap *= 2;
	s->res->body = xrealloc(s->res->body, s->cap);
}

static size_t
on_body(char *ptr, size_t size, size_t n, void *userdata)
{
	Sink *s = userdata;
	HttpRes *r = s->res;

	n *= size;
	if (r->len + n > BODY_MAX) {
		s->overflow = 1;
		return 0;
	}
	reserve(s, r->len + n + 1);
	memcpy(r->body + r->len, ptr, n);
	r->len += n;
	r->body[r->len] = 0;
	return n;
}

static int
starts(const char *line, size_t n, const char *prefix)
{
	size_t len = strlen(prefix);

	return n >= len && !strncasecmp(line, prefix, len);
}

/* Last response's ETag, verbatim. */
static size_t
on_header(char *line, size_t size, size_t n, void *userdata)
{
	HttpRes *r = userdata;
	size_t len = size * n;
	size_t skip = strlen(ETAG_PREFIX);

	if (starts(line, len, STATUS_PREFIX))
		r->etag[0] = 0;
	if (!starts(line, len, ETAG_PREFIX))
		return len;
	while (skip < len && (line[skip] == ' ' || line[skip] == '\t'))
		skip++;
	while (len > skip && isspace((unsigned char)line[len - 1]))
		len--;
	if (len - skip < sizeof r->etag)
		snprintf(r->etag, sizeof r->etag, "%.*s", (int)(len - skip), line + skip);
	return size * n;
}

static struct curl_slist *
headers(const HttpReq *q)
{
	const char *const pairs[][2] = {
		{ "Authorization: Bearer ", q->bearer },
		{ "Content-Type: ", q->ctype },
		{ "Depth: ", q->depth },
		{ "If-None-Match: ", q->if_none_match },
		{ "If-Match: ", q->if_match },
		{ "Prefer: ", q->prefer },
	};
	struct curl_slist *list = NULL;
	char line[HEADER_LINE_MAX];

	for (size_t i = 0; i < LENGTH(pairs); i++) {
		if (!pairs[i][1])
			continue;
		snprintf(line, sizeof line, "%s%s", pairs[i][0], pairs[i][1]);
		list = curl_slist_append(list, line);
	}
	return list;
}

/* Preemptive Basic avoids 401 probes. */
static unsigned long
auth_methods(const char *url)
{
	if (starts(url, strlen(url), TLS_PREFIX))
		return CURLAUTH_BASIC;
	return CURLAUTH_BASIC | CURLAUTH_DIGEST;
}

/* Custom methods keep redirect bodies. */
static int
custom(const HttpReq *q)
{
	return q->method && strcmp(q->method, "GET") && strcmp(q->method, "POST");
}

static void
configure(const HttpReq *q, Sink *sink)
{
	curl_easy_setopt(curl, CURLOPT_URL, q->url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, REDIRECTS_MAX);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, WEB_PROTOCOLS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, WEB_PROTOCOLS);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_S);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT_S);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, sink);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, sink->res);
	if (custom(q))
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, q->method);
	if (custom(q) && q->body)
		curl_easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
	if (q->body) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, q->body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
				(curl_off_t)strlen(q->body));
	}
	if (!q->user)
		return;
	curl_easy_setopt(curl, CURLOPT_HTTPAUTH, auth_methods(q->url));
	curl_easy_setopt(curl, CURLOPT_USERNAME, q->user);
	curl_easy_setopt(curl, CURLOPT_PASSWORD, q->pass);
}

static void
finish(Sink *sink, CURLcode rc)
{
	HttpRes *r = sink->res;
	char *url = NULL;

	if (rc != CURLE_OK) {
		snprintf(r->error, sizeof r->error, "%s", sink->overflow
				? "Response too large" : curl_easy_strerror(rc));
		return;
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r->status);
	curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
	snprintf(r->url, sizeof r->url, "%s", url ? url : "");
}

/* Body never NULL, always terminated. */
HttpRes
http_send(const HttpReq *q)
{
	HttpRes res = { 0 };
	Sink sink = { &res, 0, 0 };
	struct curl_slist *list;

	reserve(&sink, BODY_MIN);
	res.body[0] = 0;
	if (!curl) {
		finish(&sink, CURLE_FAILED_INIT);
		return res;
	}
	curl_easy_reset(curl);
	configure(q, &sink);
	list = headers(q);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
	finish(&sink, curl_easy_perform(curl));
	curl_slist_free_all(list);
	return res;
}

void
http_res_free(HttpRes *r)
{
	free(r->body);
	r->body = NULL;
	r->len = 0;
}

char *
http_esc(const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	char *out = xrealloc(NULL, ESC_WIDTH * strlen(s) + 1);
	char *p = out;

	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (isalnum(c) || strchr(UNRESERVED, c)) {
			*p++ = (char)c;
			continue;
		}
		*p++ = '%';
		*p++ = hex[c >> 4];
		*p++ = hex[c & 0xf];
	}
	*p = 0;
	return out;
}

char *
http_resolve(const char *base, const char *ref)
{
	CURLU *u = curl_url();
	char *joined = NULL;
	char *out = NULL;

	if (u && !curl_url_set(u, CURLUPART_URL, base, 0)
			&& !curl_url_set(u, CURLUPART_URL, ref, 0)
			&& !curl_url_get(u, CURLUPART_URL, &joined, 0)) {
		out = xrealloc(NULL, strlen(joined) + 1);
		strcpy(out, joined);
	}
	curl_free(joined);
	curl_url_cleanup(u);
	return out;
}
