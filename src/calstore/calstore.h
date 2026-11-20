/* Synced calendar data, jobs. */
#ifndef CALSTORE_H
#define CALSTORE_H

/* Include after nixlytile.h. */

#define CAL_ALLDAY   1u
#define CAL_TASK     2u
#define DAY_S        86400
#define HOUR_S       3600
#define MIN_S        60
#define REF_LEN      384
#define ACCOUNTS_MAX 16
#define DOTS         3
#define YEAR_MIN     1
#define YEAR_MAX     9999
#define MONTHS       12
#define TARGET_LOCAL "local"
#define ID_LEN       12
#define ERROR_LEN    160
#define MSG_LEN      200
#define FIELDS_MAX   8

typedef struct {
	int y, m, d;
} Civil;

typedef struct {
	int64_t start, end;
	uint32_t color;
	unsigned flags;
	uint32_t title, where, cal, ref;
} CalItem;

typedef struct {
	char id[ID_LEN];
	char provider[16];
	char label[128];
	int ncals;
	int writable;
} CalAccount;

typedef struct {
	uint32_t color[DOTS];
	int n;
} DayMarks;

typedef struct {
	char id[ID_LEN];
	char error[ERROR_LEN];
	int64_t ok_at;
} AccountState;

typedef struct {
	int token;
	int day;
	char title[128];
} PendingTodo;

typedef struct {
	char *v[FIELDS_MAX];
	int n;
} Fields;

typedef enum { JOB_SYNC, JOB_ADD, JOB_REMOVE, JOB_TODO } JobKind;

typedef struct {
	JobKind kind;
	const char *cmd, *arg, *payload, *ref;
	int token;
} JobSpec;

typedef struct {
	pid_t pid;
	int fd;
} Launch;

int day_from_civil(Civil c);
Civil civil_from_day(int day);
int weekday(int day);
const char *weekday_name(int day);
const char *month_name(int m);
int iso_week(int day);
int64_t day_start(int day);
int day_of(int64_t t);
int today(void);
/* INT_MIN when unreadable. */
int parse_date(const char *s, int base);

void store_load(void);
/* Helper cache file, size-capped. */
FILE *cache_open(const char *name);
Fields split_tabs(char *line);
void accounts_load(void);
size_t store_count(void);
const CalItem *store_item(size_t i);
const char *store_str(uint32_t off);
/* First item overlapping t. */
size_t store_seek(int64_t t);
/* First item starting after t. */
size_t store_after(int64_t t);
size_t store_task_count(void);
const CalItem *store_task(size_t i);
void store_marks(int first, DayMarks *out, int n);
int store_account_count(void);
const CalAccount *store_account(int i);
const CalAccount *store_account_by_id(const char *id);

int job_spawn(const JobSpec *s);
/* Running or queued. */
int job_busy(JobKind kind);
void job_cancel(JobKind kind);

/* Secrets go via stdin. */
Launch launch_calsync(const JobSpec *s);

int queue_push(const JobSpec *s);
const JobSpec *queue_peek(void);
void queue_drop(void);
int queue_holds(JobKind kind);

void sync_init(void);
void sync_start(void);
/* Syncs unless one ran recently. */
void sync_maybe(void);
void sync_account_line(char *args);
void sync_finished(const char *error);
int sync_running(void);
int64_t sync_last_ok(void);
const char *sync_last_error(void);
const AccountState *account_state(const char *id);
void spin_start(void);

void remind_init(void);
void remind_arm(void);

void todo_add(const char *title, int day);
void todo_done(const char *ref);
void todo_finished(int token, const char *ref, const char *error);
int todo_pending_count(void);
const PendingTodo *todo_pending(int i);
int todo_hidden(const char *ref);
/* Account id or TARGET_LOCAL. */
const char *todo_target(void);
void todo_target_next(void);

/* Implemented by the popup. */
void calendar_changed(void);
void calendar_account_added(const char *label);
void calendar_add_failed(const char *error);
/* Spinner frame while signing in. */
void calendar_tick(void);

#endif
