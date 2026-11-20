/* Sync rounds over vault accounts. */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "calsync.h"

#define TM_YEAR_BASE 1900

static Vault fresh;
static Account before[ACCOUNT_MAX];

static int64_t
day(const struct tm *t, int offset)
{
	return local_midnight(t->tm_year + TM_YEAR_BASE, t->tm_mon + 1,
			t->tm_mday + offset);
}

SyncWindow
sync_window(void)
{
	time_t now = time(NULL);
	struct tm t;

	localtime_r(&now, &t);
	return (SyncWindow){
		.now = now,
		.near_from = day(&t, -NEAR_BACK_DAYS),
		.near_to = day(&t, NEAR_AHEAD_DAYS),
		.full_from = day(&t, -FULL_BACK_DAYS),
		.full_to = day(&t, FULL_AHEAD_DAYS),
	};
}

/* Locked load; fd or -1. */
int
vault_acquire(Vault *v)
{
	int fd = vault_lock();

	if (fd < 0 || !vault_load(v))
		return fd;
	vault_unlock(fd);
	return -1;
}

static int
account_index(const Vault *v, const char *id)
{
	for (int i = 0; i < v->nacc; i++)
		if (!strcmp(v->acc[i].id, id))
			return i;
	return -1;
}

static int
sync_one(Account *a, SyncRun *run)
{
	run->error[0] = 0;
	if (!account_sync(a, run)) {
		say("acct %s\tok", a->id);
		return 0;
	}
	say("acct %s\terror\t%s", a->id, run->error[0] ? run->error : "Sync failed");
	return -1;
}

/* Copy changed accounts into fresh. */
static int
patch(const Vault *v)
{
	int n = 0;

	for (int i = 0; i < v->nacc; i++) {
		int j = account_index(&fresh, v->acc[i].id);

		if (j < 0 || !memcmp(&before[i], &v->acc[i], sizeof before[i]))
			continue;
		fresh.acc[j] = v->acc[i];
		n++;
	}
	return n;
}

static int
events_missing(void)
{
	char path[PATH_MAX];

	snprintf(path, sizeof path, "%s/events", dir_cache());
	return access(path, F_OK) != 0;
}

void
sync_merge(const Vault *v, const SyncWindow *w)
{
	say("done %d", !sources_merge(v, w));
}

/* Store, merge on reloaded vault. */
static void
settle(const Vault *v, const SyncRun *run, int gone)
{
	int merge = run->changed || gone || events_missing();
	int fd;

	if (!merge && !memcmp(before, v->acc, (size_t)v->nacc * sizeof *before)) {
		say("done 0");
		return;
	}
	fd = vault_acquire(&fresh);
	if (fd < 0) {
		say("done 0");
		return;
	}
	if (patch(v))
		vault_save(&fresh);
	say("done %d", merge && !sources_merge(&fresh, &run->w));
	vault_unlock(fd);
}

void
sync_accounts(Vault *v, const char *only, SyncRun *run)
{
	int ok[ACCOUNT_MAX] = { 0 };
	int gone = 0;

	memcpy(before, v->acc, sizeof before);
	for (int i = 0; i < v->nacc; i++) {
		if (only && strcmp(v->acc[i].id, only))
			continue;
		ok[i] = !sync_one(&v->acc[i], run);
	}
	for (int i = 0; i < v->nacc; i++)
		gone += ok[i] ? sources_gc(&v->acc[i], run) : 0;
	settle(v, run, gone);
}

void
sync_all(Vault *v, SyncRun *run)
{
	int fd = vault_acquire(v);

	if (fd < 0) {
		say("error Secure storage unavailable");
		say("done 0");
		return;
	}
	vault_unlock(fd);
	sync_accounts(v, NULL, run);
}
