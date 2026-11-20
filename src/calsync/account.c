/* Account providers, add and remove. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include "calsync.h"

#define SCHEME_SEP    "://"
#define SCHEME_CHARS  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-."
#define NEXTCLOUD_DAV "/remote.php/dav"
#define NO_STORAGE    "Secure storage unavailable"
#define LENGTH(a)     (sizeof (a) / sizeof *(a))

typedef struct Adding Adding;

typedef struct {
	const char *name;
	int (*fill)(Adding *ad);
	int (*sync)(Account *a, SyncRun *run);
	const OAuthSpec *oauth;
	int (*label)(Account *a, SyncRun *run);
	const char *url;
} Provider;

struct Adding {
	const Provider *p;
	const Account *draft;
	Vault *v;
	SyncRun *run;
	Account a;
	char replaced[sizeof ((Account *)0)->id];
};

static const struct {
	const char *from, *to;
} feed_schemes[] = {
	{ "https", "https" }, { "http", "http" },
	{ "webcals", "https" }, { "webcal", "https" },
};

static int
fail(SyncRun *run, const char *msg)
{
	run_fail(run, "%s", msg);
	return -1;
}

static int
fits(Adding *ad, int n)
{
	return n < (int)sizeof ad->a.url ? 0 : fail(ad->run, "Address too long");
}

static int
fill_oauth(Adding *ad)
{
	if (!ad->draft->client_id[0])
		return fail(ad->run, "Missing client ID — set it in the calendar block of config.kdl");
	memcpy(ad->a.client_id, ad->draft->client_id, sizeof ad->a.client_id);
	memcpy(ad->a.client_secret, ad->draft->client_secret, sizeof ad->a.client_secret);
	return 0;
}

static int
has_scheme(const char *url)
{
	size_t n = strspn(url, SCHEME_CHARS);

	return n && !strncmp(url + n, SCHEME_SEP, sizeof SCHEME_SEP - 1);
}

static int
dav_fill(Adding *ad, const char *suffix)
{
	const char *in = ad->draft->url[0] ? ad->draft->url : ad->p->url;
	int n = in ? (int)strlen(in) : 0;

	if (!n)
		return fail(ad->run, "Missing server address");
	if (!ad->draft->user[0] || !ad->draft->secret[0])
		return fail(ad->run, "Missing user name or app password");
	if (strstr(in, NEXTCLOUD_DAV))
		suffix = "";
	while (*suffix && n && in[n - 1] == '/')
		n--;
	if (fits(ad, snprintf(ad->a.url, sizeof ad->a.url, "%s%.*s%s",
			has_scheme(in) ? "" : "https://", n, in, suffix)))
		return -1;
	memcpy(ad->a.user, ad->draft->user, sizeof ad->a.user);
	memcpy(ad->a.secret, ad->draft->secret, sizeof ad->a.secret);
	memcpy(ad->a.label, ad->draft->user, sizeof ad->a.label);
	return 0;
}

static int
fill_dav(Adding *ad)
{
	return dav_fill(ad, "");
}

static int
fill_nextcloud(Adding *ad)
{
	return dav_fill(ad, NEXTCLOUD_DAV "/");
}

static int
fill_feed(Adding *ad)
{
	const char *in = ad->draft->url;
	const char *rest = strstr(in, SCHEME_SEP);
	size_t n = rest ? (size_t)(rest - in) : 0;

	for (size_t i = 0; i < LENGTH(feed_schemes); i++) {
		if (n != strlen(feed_schemes[i].from) || strncasecmp(in, feed_schemes[i].from, n))
			continue;
		return fits(ad, snprintf(ad->a.url, sizeof ad->a.url, "%s%s",
				feed_schemes[i].to, rest));
	}
	return fail(ad->run, "Feed address must start with https://");
}

static const Provider providers[] = {
	{ "google", fill_oauth, google_sync, &google_oauth, google_label, NULL },
	{ "microsoft", fill_oauth, graph_sync, &graph_oauth, graph_label, NULL },
	{ "icloud", fill_dav, caldav_sync, NULL, NULL, "https://caldav.icloud.com/" },
	{ "fastmail", fill_dav, caldav_sync, NULL, NULL, "https://caldav.fastmail.com/dav/" },
	{ "yahoo", fill_dav, caldav_sync, NULL, NULL, "https://caldav.calendar.yahoo.com/" },
	{ "nextcloud", fill_nextcloud, caldav_sync, NULL, NULL, NULL },
	{ "caldav", fill_dav, caldav_sync, NULL, NULL, NULL },
	{ "ics", fill_feed, feed_sync, NULL, NULL, NULL },
};

static const Provider *
provider_find(const char *name)
{
	for (size_t i = 0; i < LENGTH(providers); i++)
		if (!strcmp(name, providers[i].name))
			return &providers[i];
	return NULL;
}

int
account_sync(Account *a, SyncRun *run)
{
	const Provider *p = provider_find(a->provider);

	return p ? p->sync(a, run) : fail(run, "Unknown provider");
}

static int
fill(Adding *ad)
{
	uint32_t r;

	if (getrandom(&r, sizeof r, 0) != (ssize_t)sizeof r)
		return fail(ad->run, "No secure random source");
	snprintf(ad->a.id, sizeof ad->a.id, "%08" PRIx32, r);
	snprintf(ad->a.provider, sizeof ad->a.provider, "%s", ad->p->name);
	return ad->p->fill(ad);
}

/* Sign in and fetch once. */
static int
verify(Adding *ad)
{
	const Provider *p = ad->p;

	if (p->oauth && (oauth_login(p->oauth, &ad->a, ad->run)
			|| p->label(&ad->a, ad->run)))
		return -1;
	if (!p->sync(&ad->a, ad->run))
		return 0;
	sources_drop(ad->a.id);
	return -1;
}

/* Provider, label and address match. */
static int
same_login(const Account *x, const Account *y)
{
	return !strcmp(x->provider, y->provider) && !strcmp(x->label, y->label)
			&& !strcmp(x->url, y->url);
}

/* Replace same login, else append. */
static int
place(Adding *ad)
{
	Vault *v = ad->v;
	int i = 0;

	while (i < v->nacc && !same_login(&v->acc[i], &ad->a))
		i++;
	if (i == ACCOUNT_MAX)
		return fail(ad->run, "Too many accounts");
	if (i < v->nacc)
		memcpy(ad->replaced, v->acc[i].id, sizeof ad->replaced);
	v->acc[i] = ad->a;
	v->nacc += i == v->nacc;
	return vault_save(v) ? fail(ad->run, NO_STORAGE) : i;
}

static int
store(Adding *ad)
{
	int fd = vault_acquire(ad->v);
	int i;

	if (fd < 0) {
		sources_drop(ad->a.id);
		return fail(ad->run, NO_STORAGE);
	}
	i = place(ad);
	vault_unlock(fd);
	if (i < 0)
		sources_drop(ad->a.id);
	else if (ad->replaced[0])
		sources_drop(ad->replaced);
	return i;
}

int
account_add(Vault *v, const Account *draft, SyncRun *run)
{
	Adding ad = { .p = provider_find(draft->provider), .draft = draft,
			.v = v, .run = run };
	int i;

	if (!ad.p)
		return fail(run, "Unknown provider");
	i = fill(&ad) || verify(&ad) ? -1 : store(&ad);
	explicit_bzero(&ad.a, sizeof ad.a);
	return i;
}

static int
erase(Vault *v, const char *id, SyncRun *run)
{
	int i = 0;

	while (i < v->nacc && strcmp(v->acc[i].id, id))
		i++;
	if (i == v->nacc)
		return fail(run, "Account not found");
	v->nacc--;
	memmove(&v->acc[i], &v->acc[i + 1], (size_t)(v->nacc - i) * sizeof *v->acc);
	return vault_save(v) ? fail(run, NO_STORAGE) : 0;
}

/* Caller holds the vault lock. */
int
account_remove(Vault *v, const char *id, SyncRun *run)
{
	int rc = erase(v, id, run);

	if (!rc)
		sources_drop(id);
	return rc;
}
