/*
 * battwatch.c — every /sys/class/power_supply (and power-profile) read
 * happens on this thread, never on the compositor thread.
 *
 * Why: on EC-backed laptops a single read of BATx/status walks ACPI AML
 * with embedded Sleep() calls — measured 111ms on MSI hardware.  The old
 * code read it synchronously from powersave (5s), the statusbar battery
 * task (45s) and the battery hover popup, freezing the cursor for the
 * duration every time.  Here a worker thread polls the same files on its
 * own clock, publishes a snapshot under a mutex, and pokes the event
 * loop through a pipe; the compositor thread only ever copies the
 * snapshot.
 */
#include "nixlytile.h"
#include <pthread.h>

#define BW_POLL_MS 2000

static pthread_t bw_thread;
static pthread_mutex_t bw_lock = PTHREAD_MUTEX_INITIALIZER;
static BattSnapshot bw_state;          /* guarded by bw_lock */
static int bw_pipe[2] = { -1, -1 };
static struct wl_event_source *bw_src;
static volatile int bw_run;
static volatile int bw_poke;           /* force an immediate re-sample */

/* Thread-private copies of the tiny file helpers: the statusbar versions
 * are compositor-thread code and must stay free to change. */
static int
bw_line(const char *path, char *buf, size_t len)
{
	FILE *fp = fopen(path, "r");
	char *nl;

	if (!fp)
		return -1;
	if (!fgets(buf, len, fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	nl = strchr(buf, '\n');
	if (nl)
		*nl = '\0';
	return 0;
}

static int
bw_ull(const char *path, unsigned long long *out)
{
	char buf[64];
	char *end = NULL;
	unsigned long long val;

	if (bw_line(path, buf, sizeof(buf)) != 0)
		return -1;
	errno = 0;
	val = strtoull(buf, &end, 10);
	if (errno != 0 || end == buf)
		return -1;
	*out = val;
	return 0;
}

/* Estimated charge cycles: EC firmware on MSI laptops never counts —
 * cycle_count reads 0 forever.  Track discharge ourselves: every drop
 * in energy_now/charge_now accumulates into a lifetime µWh counter
 * persisted under XDG state; one cycle = design capacity discharged.
 * Worker-thread only. */
static unsigned long long bw_acc_uwh;
static unsigned long long bw_acc_saved_uwh;
static unsigned long long bw_prev_uwh;
static int bw_prev_valid;
static char bw_acc_path[PATH_MAX];

static void
bw_acc_init(void)
{
	const char *st = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	char dir[PATH_MAX - 32];

	if (st && *st)
		snprintf(dir, sizeof(dir), "%s/nixlyos", st);
	else if (home && *home)
		snprintf(dir, sizeof(dir), "%s/.local/state/nixlyos", home);
	else
		return;
	mkdir(dir, 0755);
	snprintf(bw_acc_path, sizeof(bw_acc_path), "%s/battery_discharge_uwh",
			dir);
	if (bw_ull(bw_acc_path, &bw_acc_uwh) != 0)
		bw_acc_uwh = 0;
	bw_acc_saved_uwh = bw_acc_uwh;
}

static void
bw_acc_save_maybe(void)
{
	FILE *fp;

	/* persist every 0.05Wh (~once per percent on a 50Wh pack) */
	if (!bw_acc_path[0] || bw_acc_uwh - bw_acc_saved_uwh < 50000ull)
		return;
	fp = fopen(bw_acc_path, "w");
	if (!fp)
		return;
	fprintf(fp, "%llu\n", bw_acc_uwh);
	fclose(fp);
	bw_acc_saved_uwh = bw_acc_uwh;
}

/* System battery discovery — same rules as the old findbatterydevice():
 * type Battery, scope System or none (skips mouse/headset batteries). */
static int
bw_find_device(char *dir_out, size_t dir_len)
{
	DIR *dir = opendir("/sys/class/power_supply");
	struct dirent *ent;
	int found = 0;

	if (!dir)
		return 0;
	while ((ent = readdir(dir))) {
		char path[PATH_MAX];
		char type[32], scope[32];

		if (ent->d_name[0] == '.')
			continue;
		if (snprintf(path, sizeof(path),
				"/sys/class/power_supply/%s/type", ent->d_name)
				>= (int)sizeof(path))
			continue;
		if (bw_line(path, type, sizeof(type)) != 0 ||
				strcmp(type, "Battery") != 0)
			continue;
		snprintf(path, sizeof(path),
				"/sys/class/power_supply/%s/scope", ent->d_name);
		if (bw_line(path, scope, sizeof(scope)) == 0 &&
				strcmp(scope, "System") != 0)
			continue;
		snprintf(path, sizeof(path),
				"/sys/class/power_supply/%s/capacity", ent->d_name);
		if (access(path, R_OK) != 0)
			continue;
		if (snprintf(dir_out, dir_len, "/sys/class/power_supply/%s",
				ent->d_name) >= (int)dir_len)
			continue;
		found = 1;
		break;
	}
	closedir(dir);
	return found;
}

static void
bw_read_profile(BattSnapshot *s)
{
	char buf[64];
	FILE *fp;

	s->has_profile = 0;
	s->profile_backend = PROFILE_BACKEND_NONE;
	s->profile[0] = '\0';
	s->choices[0] = '\0';

	if (bw_line("/sys/firmware/acpi/platform_profile", s->profile,
			sizeof(s->profile)) == 0) {
		s->has_profile = 1;
		s->profile_backend = PROFILE_BACKEND_ACPI;
		if (bw_line("/sys/firmware/acpi/platform_profile_choices",
				s->choices, sizeof(s->choices)) != 0)
			s->choices[0] = '\0';
		return;
	}

	if (bw_line("/sys/devices/platform/msi-ec/shift_mode", s->profile,
			sizeof(s->profile)) == 0) {
		s->has_profile = 1;
		s->profile_backend = PROFILE_BACKEND_MSI_EC;
		fp = fopen("/sys/devices/platform/msi-ec/available_shift_modes",
				"r");
		if (fp) {
			size_t len = 0;

			while (fgets(buf, sizeof(buf), fp)) {
				char *nl = strchr(buf, '\n');
				int n;

				if (nl)
					*nl = '\0';
				if (!buf[0])
					continue;
				n = snprintf(s->choices + len,
						sizeof(s->choices) - len,
						"%s%s", len ? " " : "", buf);
				if (n < 0 || (size_t)n >=
						sizeof(s->choices) - len)
					break;
				len += (size_t)n;
			}
			fclose(fp);
		}
	}
}

static void
bw_sample(BattSnapshot *s)
{
	char path[PATH_MAX];
	unsigned long long val;
	double vmin = -1.0;
	const char *d = s->device_dir;

	snprintf(path, sizeof(path), "%s/status", d);
	if (bw_line(path, s->status, sizeof(s->status)) != 0)
		s->status[0] = '\0';

	s->percent = -1.0;
	snprintf(path, sizeof(path), "%s/capacity", d);
	if (bw_ull(path, &val) == 0)
		s->percent = val > 100 ? 100.0 : (double)val;

	s->voltage_v = -1.0;
	snprintf(path, sizeof(path), "%s/voltage_now", d);
	if (bw_ull(path, &val) == 0)
		s->voltage_v = val / 1000000.0;

	s->power_w = -1.0;
	snprintf(path, sizeof(path), "%s/power_now", d);
	if (bw_ull(path, &val) == 0) {
		s->power_w = val / 1000000.0;
	} else {
		snprintf(path, sizeof(path), "%s/current_now", d);
		if (bw_ull(path, &val) == 0 && s->voltage_v > 0)
			s->power_w = (val / 1000000.0) * s->voltage_v;
	}

	s->time_remaining_h = -1.0;
	if (strcmp(s->status, "Discharging") == 0 && s->power_w > 0.1) {
		snprintf(path, sizeof(path), "%s/energy_now", d);
		if (bw_ull(path, &val) == 0) {
			s->time_remaining_h = (val / 1000000.0) / s->power_w;
		} else {
			snprintf(path, sizeof(path), "%s/charge_now", d);
			if (bw_ull(path, &val) == 0 && s->voltage_v > 0)
				s->time_remaining_h = (val / 1000000.0)
					* s->voltage_v / s->power_w;
		}
	}

	snprintf(path, sizeof(path), "%s/voltage_min_design", d);
	if (bw_ull(path, &val) == 0)
		vmin = val / 1000000.0;

	s->design_wh = -1.0;
	snprintf(path, sizeof(path), "%s/energy_full_design", d);
	if (bw_ull(path, &val) == 0) {
		s->design_wh = val / 1000000.0;
	} else {
		snprintf(path, sizeof(path), "%s/charge_full_design", d);
		if (bw_ull(path, &val) == 0 && vmin > 0)
			s->design_wh = (val / 1000000.0) * vmin;
	}

	s->full_wh = -1.0;
	snprintf(path, sizeof(path), "%s/energy_full", d);
	if (bw_ull(path, &val) == 0) {
		s->full_wh = val / 1000000.0;
	} else {
		snprintf(path, sizeof(path), "%s/charge_full", d);
		if (bw_ull(path, &val) == 0 && vmin > 0)
			s->full_wh = (val / 1000000.0) * vmin;
	}

	s->cycles = -1;
	snprintf(path, sizeof(path), "%s/cycle_count", d);
	if (bw_ull(path, &val) == 0)
		s->cycles = (int)val;

	s->cycles_est = -1;
	{
		unsigned long long now_uwh = 0;
		int have = 0;

		snprintf(path, sizeof(path), "%s/energy_now", d);
		if (bw_ull(path, &val) == 0) {
			now_uwh = val;
			have = 1;
		} else {
			snprintf(path, sizeof(path), "%s/charge_now", d);
			if (bw_ull(path, &val) == 0 && vmin > 0) {
				now_uwh = (unsigned long long)
					((double)val * vmin);
				have = 1;
			}
		}
		if (have) {
			/* only count drops; >5Wh between 2s samples is a
			 * sysfs glitch, not real discharge */
			if (bw_prev_valid && now_uwh < bw_prev_uwh &&
					bw_prev_uwh - now_uwh < 5000000ull)
				bw_acc_uwh += bw_prev_uwh - now_uwh;
			bw_prev_uwh = now_uwh;
			bw_prev_valid = 1;
		}
		if (s->design_wh > 0)
			s->cycles_est = (int)(bw_acc_uwh /
					(unsigned long long)
					(s->design_wh * 1e6));
		bw_acc_save_maybe();
	}

	s->thr_start = s->thr_end = -1;
	snprintf(path, sizeof(path), "%s/charge_control_start_threshold", d);
	if (bw_ull(path, &val) == 0)
		s->thr_start = (int)val;
	snprintf(path, sizeof(path), "%s/charge_control_end_threshold", d);
	if (bw_ull(path, &val) == 0)
		s->thr_end = (int)val;

	bw_read_profile(s);
}

static void *
bw_worker(void *data)
{
	BattSnapshot local = {0};

	(void)data;
	pthread_setname_np(pthread_self(), "nixly-battwatch");

	local.available = bw_find_device(local.device_dir,
			sizeof(local.device_dir));
	if (local.available)
		bw_acc_init();

	while (bw_run) {
		int changed;

		if (local.available)
			bw_sample(&local);
		local.stamp_ms = monotonic_msec();

		pthread_mutex_lock(&bw_lock);
		changed = memcmp(&bw_state, &local, sizeof(local)) != 0;
		bw_state = local;
		pthread_mutex_unlock(&bw_lock);

		if (changed && bw_pipe[1] >= 0)
			(void)!write(bw_pipe[1], "b", 1);

		/* Desktops without a battery: one publish, then done. */
		if (!local.available)
			break;

		for (int i = 0; i < BW_POLL_MS / 100 && bw_run; i++) {
			struct timespec ts = { 0, 100 * 1000000 };

			if (bw_poke) {
				bw_poke = 0;
				break;
			}
			nanosleep(&ts, NULL);
		}
	}
	return NULL;
}

static int
bw_event(int fd, uint32_t mask, void *data)
{
	char buf[16];

	(void)mask;
	(void)data;
	while (read(fd, buf, sizeof(buf)) > 0)
		;
	powersave_battery_event();
	statusbar_battery_event();
	return 0;
}

int
battwatch_get(BattSnapshot *out)
{
	pthread_mutex_lock(&bw_lock);
	*out = bw_state;
	pthread_mutex_unlock(&bw_lock);
	return out->stamp_ms != 0;
}

/* Ask the worker for a fresh sample soon (profile click, popup open). */
void
battwatch_refresh(void)
{
	bw_poke = 1;
}

void
battwatch_init(void)
{
	if (pipe2(bw_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
		return;
	bw_src = wl_event_loop_add_fd(event_loop, bw_pipe[0],
			WL_EVENT_READABLE, bw_event, NULL);
	bw_run = 1;
	if (pthread_create(&bw_thread, NULL, bw_worker, NULL) != 0) {
		bw_run = 0;
		wlr_log(WLR_ERROR, "battwatch: thread start failed — "
			"battery state unavailable");
	}
}
