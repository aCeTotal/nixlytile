/* Vault plaintext line format. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "calsync.h"

#define LEN(a)          (sizeof(a) / sizeof *(a))
#define LINE_SLACK      64
#define ACCOUNT_KIND    "a"
#define ACCOUNT_FIELDS  14
#define TODO_KIND       "t"
#define TODO_FIELDS     4
#define NO_DATE         "-"
#define TEXT_FIELD(f)   { offsetof(Account, f), sizeof ((Account *)0)->f }

typedef struct {
	size_t off, size;
} TextField;

static const TextField account_text[] = {
	TEXT_FIELD(id), TEXT_FIELD(provider), TEXT_FIELD(label),
	TEXT_FIELD(url), TEXT_FIELD(home), TEXT_FIELD(todo_url),
	TEXT_FIELD(user), TEXT_FIELD(secret), TEXT_FIELD(client_id),
	TEXT_FIELD(client_secret), TEXT_FIELD(access),
};

static int
fits(char *dst, size_t size, const char *s)
{
	return (size_t)snprintf(dst, size, "%s", s) < size;
}

static int
account_parsed(Account *a, char *const *f)
{
	size_t nt = LEN(account_text);
	char tail;

	for (size_t i = 0; i < nt; i++)
		if (!fits((char *)a + account_text[i].off, account_text[i].size, f[i]))
			return 0;
	return sscanf(f[nt], "%" SCNd64 "%c", &a->access_exp, &tail) == 1
			&& sscanf(f[nt + 1], "%d%c", &a->ncals, &tail) == 1;
}

static int
todo_parsed(LocalTodo *t, char *const *f)
{
	char tail;

	t->y = t->m = t->d = 0;
	if (!fits(t->id, sizeof t->id, f[0]) || !fits(t->title, sizeof t->title, f[2]))
		return 0;
	if (!strcmp(f[1], NO_DATE))
		return 1;
	return sscanf(f[1], "%4d-%2d-%2d%c", &t->y, &t->m, &t->d, &tail) == 3
			&& t->y > 0;
}

static void
parse_line(Vault *v, char *line)
{
	char *f[ACCOUNT_FIELDS];
	int n = tab_split(line, f, ACCOUNT_FIELDS);

	if (n == ACCOUNT_FIELDS && !strcmp(f[0], ACCOUNT_KIND) && v->nacc < ACCOUNT_MAX)
		v->nacc += account_parsed(&v->acc[v->nacc], f + 1);
	if (n == TODO_FIELDS && !strcmp(f[0], TODO_KIND) && v->ntodo < TODO_MAX)
		v->ntodo += todo_parsed(&v->todo[v->ntodo], f + 1);
}

/* Malformed lines are ignored. */
void
vault_parse(Vault *v, char *text)
{
	char *line;

	while ((line = strsep(&text, "\n")))
		parse_line(v, line);
}

static size_t
format_account(char *dst, size_t room, const Account *a)
{
	size_t n = (size_t)snprintf(dst, room, ACCOUNT_KIND);

	for (size_t i = 0; i < LEN(account_text); i++)
		n += (size_t)snprintf(dst + n, room - n, "\t%s",
				(const char *)a + account_text[i].off);
	return n + (size_t)snprintf(dst + n, room - n, "\t%" PRId64 "\t%d\n",
			a->access_exp, a->ncals);
}

static size_t
format_todo(char *dst, size_t room, const LocalTodo *t)
{
	if (!t->y)
		return (size_t)snprintf(dst, room, TODO_KIND "\t%s\t" NO_DATE "\t%s\n",
				t->id, t->title);
	return (size_t)snprintf(dst, room, TODO_KIND "\t%s\t%04d-%02d-%02d\t%s\n",
			t->id, t->y, t->m, t->d, t->title);
}

/* Single allocation keeps wiping complete. */
Plaintext
vault_format(const Vault *v)
{
	size_t cap = (size_t)v->nacc * (sizeof(Account) + LINE_SLACK)
			+ (size_t)v->ntodo * (sizeof(LocalTodo) + LINE_SLACK) + 1;
	Plaintext p = { xrealloc(NULL, cap), 0 };

	for (int i = 0; i < v->nacc; i++)
		p.len += format_account(p.buf + p.len, cap - p.len, &v->acc[i]);
	for (int i = 0; i < v->ntodo; i++)
		p.len += format_todo(p.buf + p.len, cap - p.len, &v->todo[i]);
	return p;
}
