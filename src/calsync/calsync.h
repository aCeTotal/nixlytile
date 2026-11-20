/* Shared calendar sync declarations. */
#ifndef CALSYNC_H
#define CALSYNC_H

#include <stddef.h>
#include <stdint.h>

#define EV_ALLDAY 1u
#define EV_TASK   2u

#define ACCOUNT_MAX     16
#define TOUCHED_MAX     256
#define NEAR_BACK_DAYS  1
#define NEAR_AHEAD_DAYS 31
#define FULL_BACK_DAYS  92
#define FULL_AHEAD_DAYS 400
#define FULL_REFRESH_S  3600
#define TOKEN_SLACK_S   300
#define TODO_MAX        512
#define PERCENT_DONE    100

#define HEX_DIGITS "0123456789abcdefABCDEF"
#define RGB_DIGITS 6

#define HTTP_OK           200
#define HTTP_CREATED      201
#define HTTP_NO_CONTENT   204
#define HTTP_MULTISTATUS  207
#define HTTP_NOT_MODIFIED 304
#define HTTP_CLIENT_ERROR 400
#define HTTP_UNAUTHORIZED 401
#define HTTP_FORBIDDEN    403
#define HTTP_NOT_FOUND    404
#define HTTP_PRECONDITION 412
#define HTTP_SERVER_ERROR 500

typedef struct {
	int64_t start, end;
	unsigned flags;
	uint32_t color;
	char title[128];
	char where[96];
	char cal[64];
	char ref[384];
} CalEvent;

typedef struct {
	CalEvent *v;
	size_t n, cap;
} EventList;

typedef struct {
	char id[12];
	char provider[16];
	char label[128];
	char url[512];
	char home[512];
	char todo_url[512];
	char user[128];
	char secret[4096];
	char client_id[192];
	char client_secret[128];
	char access[4096];
	int64_t access_exp;
	int ncals;
} Account;

typedef struct {
	int64_t now;
	int64_t near_from, near_to;
	int64_t full_from, full_to;
} SyncWindow;

typedef struct {
	SyncWindow w;
	int changed;
	char touched[TOUCHED_MAX][48];
	int ntouched;
	char error[192];
} SyncRun;

typedef struct {
	char file[48];
	char tag[256];
	int64_t full_at;
	int dirty;
	int changed;
	EventList ev;
} Source;

typedef struct {
	const char *method;
	const char *url;
	const char *bearer;
	const char *user, *pass;
	const char *ctype;
	const char *body;
	const char *depth;
	const char *if_none_match;
	const char *if_match;
	const char *prefer;
} HttpReq;

typedef struct {
	long status;
	char *body;
	size_t len;
	char etag[256];
	char url[1024];
	char error[160];
} HttpRes;

typedef struct {
	const char *name;
	const char *auth_url;
	const char *token_url;
	const char *scope;
	const char *redirect_host;
	const char *auth_extra;
	const char *refresh_scope;
} OAuthSpec;

typedef struct {
	const char *key;
	size_t off, len;
} FormKey;

typedef struct {
	const FormKey *keys;
	size_t n;
	void *dst;
} Form;

typedef struct {
	char redirect[64];
	char state[48];
	char code[4096];
	char error[64];
	int fd[2];
	int nfd;
} Loopback;

typedef struct {
	const char *name;
	uint32_t color;
} CalMeta;

typedef struct {
	char title[128];
	int y, m, d;
} TodoNew;

typedef struct {
	char id[12];
	int y, m, d;
	char title[128];
} LocalTodo;

typedef struct {
	Account acc[ACCOUNT_MAX];
	int nacc;
	LocalTodo todo[TODO_MAX];
	int ntodo;
} Vault;

typedef struct {
	char *buf;
	size_t len;
} Plaintext;

void *xrealloc(void *p, size_t n);
void run_fail(SyncRun *run, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

SyncWindow sync_window(void);
int vault_acquire(Vault *v);
void sync_all(Vault *v, SyncRun *run);
void sync_accounts(Vault *v, const char *only, SyncRun *run);
void sync_merge(const Vault *v, const SyncWindow *w);

int account_sync(Account *a, SyncRun *run);
int account_add(Vault *v, const Account *draft, SyncRun *run);
int account_remove(Vault *v, const char *id, SyncRun *run);

int form_read(const Form *f);

const char *dir_data(void);
const char *dir_cache(void);
const char *dir_sources(void);
int write_all(int fd, const char *buf, size_t len);
int fsync_path(const char *path);
int file_replace(const char *path, const char *buf, size_t len, int mode);

CalEvent *events_push(EventList *l);
void events_free(EventList *l);
void events_sort(EventList *l);
int events_equal(const EventList *a, const EventList *b);
char *events_format(const EventList *l, const char *head);
void events_load(EventList *l, const char *path);
int tab_split(char *line, char **f, int max);

void event_text(char *dst, size_t len, const char *s);
uint32_t palette_color(const char *name);
int parse_color(const char *s, uint32_t *rgb);
int64_t local_midnight(int y, int m, int d);
int64_t parse_rfc3339(const char *s);
uint64_t fnv64(const char *s);

void source_open(Source *s, const Account *a, const char *remote_id);
int source_full_due(const Source *s, const SyncWindow *w);
void source_take(Source *s, EventList *fresh);
void source_splice(Source *s, EventList *fresh, const SyncWindow *w);
void source_replace(Source *s, EventList *fresh, const SyncWindow *w);
void source_set_tag(Source *s, const char *tag);
void source_close(Source *s, SyncRun *run);
int sources_gc(const Account *a, const SyncRun *run);
void sources_drop(const char *account_id);
int sources_merge(const Vault *v, const SyncWindow *w);

int vault_lock(void);
void vault_unlock(int fd);
int vault_load(Vault *v);
int vault_save(const Vault *v);

void vault_parse(Vault *v, char *text);
Plaintext vault_format(const Vault *v);

void http_init(void);
void http_cleanup(void);
HttpRes http_send(const HttpReq *q);
void http_res_free(HttpRes *r);
char *http_esc(const char *s);
char *http_resolve(const char *base, const char *ref);

int oauth_login(const OAuthSpec *o, Account *a, SyncRun *run);
int oauth_token(const OAuthSpec *o, Account *a, SyncRun *run);

int loopback_open(Loopback *lb, const char *host);
int loopback_wait(Loopback *lb);
void loopback_close(Loopback *lb);

extern const OAuthSpec google_oauth;
int google_label(Account *a, SyncRun *run);
int google_sync(Account *a, SyncRun *run);

int google_todo_add(Account *a, const TodoNew *t, SyncRun *run);
int google_todo_done(Account *a, const char *ref, SyncRun *run);

extern const OAuthSpec graph_oauth;
int graph_label(Account *a, SyncRun *run);
int graph_sync(Account *a, SyncRun *run);

int graph_todo_add(Account *a, const TodoNew *t, SyncRun *run);
int graph_todo_done(Account *a, const char *ref, SyncRun *run);

int caldav_sync(Account *a, SyncRun *run);

int caldav_todo_add(Account *a, const TodoNew *t, SyncRun *run);
int caldav_todo_done(Account *a, const char *ref, SyncRun *run);

int ical_expand(const char *text, const CalMeta *m, const SyncWindow *w,
		EventList *out);

char *ical_todo_new(const TodoNew *t, const char *uid);
char *ical_todo_complete(const char *text, int64_t now);

int feed_sync(Account *a, SyncRun *run);

int todo_add(Vault *v, const char *target, const TodoNew *t, SyncRun *run);
int todo_done(Vault *v, const char *ref, SyncRun *run);
void local_todos(const Vault *v, EventList *out);

#endif
