/* Accounts listing from the helper. */
#include "nixlytile.h"
#include "calstore/calstore.h"

enum { AC_ID, AC_PROVIDER, AC_LABEL, AC_NCALS, AC_WRITABLE, AC_FIELDS };

static CalAccount accounts[ACCOUNTS_MAX];
static int naccounts;

static void
add_account(char *const *f)
{
	CalAccount *a = &accounts[naccounts++];

	snprintf(a->id, sizeof(a->id), "%s", f[AC_ID]);
	snprintf(a->provider, sizeof(a->provider), "%s", f[AC_PROVIDER]);
	snprintf(a->label, sizeof(a->label), "%s", f[AC_LABEL]);
	a->ncals = atoi(f[AC_NCALS]);
	a->writable = atoi(f[AC_WRITABLE]) == 1;
}

static void
read_accounts(FILE *fp)
{
	char *line = NULL;
	size_t cap = 0;

	while (naccounts < ACCOUNTS_MAX && getline(&line, &cap, fp) > 0) {
		Fields f = split_tabs(line);

		if (f.n == AC_FIELDS)
			add_account(f.v);
	}
	free(line);
}

void
accounts_load(void)
{
	FILE *fp = cache_open("accounts");

	naccounts = 0;
	if (!fp)
		return;
	read_accounts(fp);
	fclose(fp);
}

int
store_account_count(void)
{
	return naccounts;
}

const CalAccount *
store_account(int i)
{
	return &accounts[i];
}

const CalAccount *
store_account_by_id(const char *id)
{
	for (int i = 0; i < naccounts; i++)
		if (!strcmp(accounts[i].id, id))
			return &accounts[i];
	return NULL;
}
