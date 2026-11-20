/* OAuth sign-in and token refresh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/random.h>
#include <json-c/json.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include "calsync.h"

#define VERIFIER_BYTES    64
#define STATE_BYTES       24
#define B64_SIZE(n)       (((n) + 2) / 3 * 4 + 1)
#define QUERY_MAX         16384
#define URL_MAX           4096
#define EXPIRES_DEFAULT_S 3600
#define FORM_TYPE         "application/x-www-form-urlencoded"

typedef struct {
	char s[QUERY_MAX];
	size_t len;
} Query;

typedef struct {
	const OAuthSpec *o;
	Account *a;
	SyncRun *run;
	Query form;
	int expired;
} Grant;

typedef struct {
	const OAuthSpec *o;
	Account *a;
	SyncRun *run;
	Loopback lb;
	char verifier[B64_SIZE(VERIFIER_BYTES)];
	char challenge[B64_SIZE(SHA256_DIGEST_LENGTH)];
} Login;

static void
b64url(char *dst, const unsigned char *src, size_t n)
{
	int len = EVP_EncodeBlock((unsigned char *)dst, src, (int)n);

	for (int i = 0; i < len; i++) {
		if (dst[i] == '+')
			dst[i] = '-';
		if (dst[i] == '/')
			dst[i] = '_';
	}
	while (len > 0 && dst[len - 1] == '=')
		dst[--len] = 0;
}

static void
query_add(Query *q, const char *key, const char *val)
{
	char *v = http_esc(val);
	int n = snprintf(q->s + q->len, sizeof q->s - q->len, "%s%s=%s",
			q->len ? "&" : "", key, v);

	q->len += n > 0 ? (size_t)n : 0;
	if (q->len >= sizeof q->s)
		q->len = sizeof q->s - 1;
	explicit_bzero(v, strlen(v));
	free(v);
}

static int
pkce(Login *l)
{
	unsigned char raw[VERIFIER_BYTES], state[STATE_BYTES];
	unsigned char md[SHA256_DIGEST_LENGTH];

	_Static_assert(sizeof l->lb.state >= B64_SIZE(STATE_BYTES), "state fits");
	if (getrandom(raw, sizeof raw, 0) != (ssize_t)sizeof raw
			|| getrandom(state, sizeof state, 0) != (ssize_t)sizeof state) {
		run_fail(l->run, "No secure random source");
		return -1;
	}
	b64url(l->verifier, raw, sizeof raw);
	b64url(l->lb.state, state, sizeof state);
	explicit_bzero(raw, sizeof raw);
	if (!EVP_Digest(l->verifier, strlen(l->verifier), md, NULL, EVP_sha256(), NULL)) {
		run_fail(l->run, "Could not prepare the sign-in");
		return -1;
	}
	b64url(l->challenge, md, sizeof md);
	return 0;
}

static int
listen_redirect(Login *l)
{
	if (!loopback_open(&l->lb, l->o->redirect_host))
		return 0;
	run_fail(l->run, "Could not listen for the sign-in reply");
	return -1;
}

static int
open_browser(Login *l)
{
	Query q = { .len = 0 };
	char url[URL_MAX];

	query_add(&q, "client_id", l->a->client_id);
	query_add(&q, "redirect_uri", l->lb.redirect);
	query_add(&q, "response_type", "code");
	query_add(&q, "scope", l->o->scope);
	query_add(&q, "code_challenge", l->challenge);
	query_add(&q, "code_challenge_method", "S256");
	query_add(&q, "state", l->lb.state);
	if (snprintf(url, sizeof url, "%s?%s&%s", l->o->auth_url, q.s,
			l->o->auth_extra) >= (int)sizeof url) {
		run_fail(l->run, "Sign-in address too long");
		return -1;
	}
	say("open %s", url);
	return 0;
}

static int
await_code(Login *l)
{
	if (loopback_wait(&l->lb)) {
		run_fail(l->run, "%s sign-in timed out", l->o->name);
		return -1;
	}
	if (!l->lb.error[0])
		return 0;
	run_fail(l->run, "%s sign-in failed: %s", l->o->name, l->lb.error);
	return -1;
}

static const char *
json_str(json_object *o, const char *key)
{
	json_object *v;

	if (!json_object_object_get_ex(o, key, &v) || !json_object_is_type(v, json_type_string))
		return NULL;
	return json_object_get_string(v);
}

static int
grant_store(Grant *g, json_object *root)
{
	const char *token = json_str(root, "access_token");
	const char *renewed = json_str(root, "refresh_token");
	json_object *exp;
	int64_t ttl = EXPIRES_DEFAULT_S;

	if (!token) {
		run_fail(g->run, "%s sign-in failed: no access token", g->o->name);
		return -1;
	}
	if (json_object_object_get_ex(root, "expires_in", &exp))
		ttl = json_object_get_int64(exp);
	snprintf(g->a->access, sizeof g->a->access, "%s", token);
	g->a->access_exp = time(NULL) + ttl;
	if (renewed)
		snprintf(g->a->secret, sizeof g->a->secret, "%s", renewed);
	return 0;
}

static int
grant_refused(Grant *g, json_object *root, long status)
{
	const char *err = json_str(root, "error");

	if (!err) {
		run_fail(g->run, "%s sign-in failed: HTTP %ld", g->o->name, status);
		return -1;
	}
	g->expired = !strcmp(err, "invalid_grant");
	run_fail(g->run, "%s sign-in failed: %s", g->o->name, err);
	return -1;
}

static int
grant_reply(Grant *g, const HttpRes *res)
{
	json_object *root;
	int rc;

	if (!res->status) {
		run_fail(g->run, "Offline: %s", res->error);
		return -1;
	}
	root = json_tokener_parse(res->body);
	rc = res->status == HTTP_OK ? grant_store(g, root)
			: grant_refused(g, root, res->status);
	json_object_put(root);
	return rc;
}

static int
grant_send(Grant *g)
{
	HttpReq q = { .method = "POST", .url = g->o->token_url,
			.ctype = FORM_TYPE, .body = g->form.s };
	HttpRes res = http_send(&q);
	int rc = grant_reply(g, &res);

	explicit_bzero(&g->form, sizeof g->form);
	explicit_bzero(res.body, res.len);
	http_res_free(&res);
	return rc;
}

static void
query_client(Grant *g)
{
	query_add(&g->form, "client_id", g->a->client_id);
	if (g->a->client_secret[0])
		query_add(&g->form, "client_secret", g->a->client_secret);
}

static int
exchange(Login *l)
{
	Grant g = { .o = l->o, .a = l->a, .run = l->run };

	query_add(&g.form, "grant_type", "authorization_code");
	query_add(&g.form, "code", l->lb.code);
	query_add(&g.form, "redirect_uri", l->lb.redirect);
	query_add(&g.form, "code_verifier", l->verifier);
	query_client(&g);
	l->a->secret[0] = 0;
	if (grant_send(&g))
		return -1;
	if (l->a->secret[0])
		return 0;
	run_fail(l->run, "%s did not grant offline access", l->o->name);
	return -1;
}

int
oauth_login(const OAuthSpec *o, Account *a, SyncRun *run)
{
	Login l = { .o = o, .a = a, .run = run };
	int rc = pkce(&l) || listen_redirect(&l) || open_browser(&l)
			|| await_code(&l) || exchange(&l) ? -1 : 0;

	loopback_close(&l.lb);
	explicit_bzero(&l, sizeof l);
	return rc;
}

static int
token_refresh(const OAuthSpec *o, Account *a, SyncRun *run)
{
	Grant g = { .o = o, .a = a, .run = run };
	int rc;

	query_add(&g.form, "grant_type", "refresh_token");
	query_add(&g.form, "refresh_token", a->secret);
	query_client(&g);
	if (o->refresh_scope)
		query_add(&g.form, "scope", o->refresh_scope);
	rc = grant_send(&g);
	if (g.expired)
		run_fail(run, "Sign-in expired — remove and add the account again");
	return rc;
}

int
oauth_token(const OAuthSpec *o, Account *a, SyncRun *run)
{
	if (a->access[0] && a->access_exp - TOKEN_SLACK_S > time(NULL))
		return 0;
	return token_refresh(o, a, run);
}
