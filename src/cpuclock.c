/*
 * cpuclock.c — direct CPU-clock and power-profile control via sysfs.
 *
 * power-profiles-daemon is intentionally disabled on nixlyos (perf.nix
 * owns the governor), so the compositor's power features write sysfs
 * directly: scaling_max_freq per cpufreq policy for down/up-clocking,
 * turbo/boost, and the ACPI platform profile (or MSI shift_mode) that
 * the battery popup already uses.  All files are made group-writable by
 * the nixlyos platform-profile-perms boot service.
 */
#include "nixlytile.h"

#include <dirent.h>
#include <pthread.h>

#define CPUFREQ_DIR "/sys/devices/system/cpu/cpufreq"
#define NO_TURBO    "/sys/devices/system/cpu/intel_pstate/no_turbo"
#define BOOST       "/sys/devices/system/cpu/cpufreq/boost"
#define ACPI_PROFILE "/sys/firmware/acpi/platform_profile"
#define ACPI_CHOICES "/sys/firmware/acpi/platform_profile_choices"
#define MSI_PROFILE  "/sys/devices/platform/msi-ec/shift_mode"
#define MSI_CHOICES  "/sys/devices/platform/msi-ec/available_shift_modes"

static int
read_ul(const char *path, unsigned long *out)
{
	FILE *fp = fopen(path, "r");
	int ok;

	if (!fp)
		return -1;
	ok = fscanf(fp, "%lu", out) == 1;
	fclose(fp);
	return ok ? 0 : -1;
}

static int
write_str(const char *path, const char *s)
{
	FILE *fp = fopen(path, "w");
	int ok;

	if (!fp)
		return -1;
	ok = fputs(s, fp) >= 0;
	fclose(fp);
	return ok ? 0 : -1;
}

static void
write_ul(const char *path, unsigned long v)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "%lu", v);
	write_str(path, buf);
}

/* Cap every policy's max clock at min + frac*(range).  frac 0 pins the
 * CPU to its lowest clock, 1.0 releases the cap. */
void
cpuclock_cap(double frac)
{
	DIR *d = opendir(CPUFREQ_DIR);
	struct dirent *e;

	if (!d)
		return;
	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	while ((e = readdir(d))) {
		char path[PATH_MAX];
		unsigned long lo, hi;

		if (strncmp(e->d_name, "policy", 6) != 0)
			continue;
		snprintf(path, sizeof(path), CPUFREQ_DIR "/%s/cpuinfo_min_freq",
				e->d_name);
		if (read_ul(path, &lo) != 0)
			continue;
		snprintf(path, sizeof(path), CPUFREQ_DIR "/%s/cpuinfo_max_freq",
				e->d_name);
		if (read_ul(path, &hi) != 0 || hi <= lo)
			continue;
		snprintf(path, sizeof(path), CPUFREQ_DIR "/%s/scaling_max_freq",
				e->d_name);
		write_ul(path, lo + (unsigned long)((hi - lo) * frac));
	}
	closedir(d);
}

void
cpuclock_restore(void)
{
	cpuclock_cap(1.0);
}

/* Turbo/boost on or off; intel_pstate's knob is inverted. */
void
cpuclock_boost(int on)
{
	if (access(NO_TURBO, W_OK) == 0)
		write_str(NO_TURBO, on ? "0" : "1");
	else if (access(BOOST, W_OK) == 0)
		write_str(BOOST, on ? "1" : "0");
}

/* Per-policy scaling governor + energy-performance preference.  On
 * battery the freq cap alone is not enough: with the performance
 * governor the CPU pins to the capped ceiling and never idles down, and
 * EPP=performance disables the pstate driver's power heuristics.  So on
 * battery drop to the powersave governor (lets the core race-to-idle at
 * low clocks) and EPP=power; on AC restore performance/performance.
 * Both files are group-writable via the nixlyos perms boot service; if
 * EPP isn't writable (older perms), the governor change still lands. */
void
cpuclock_perf(int on_ac)
{
	DIR *d = opendir(CPUFREQ_DIR);
	struct dirent *e;
	const char *gov = on_ac ? "performance" : "powersave";
	const char *epp = on_ac ? "performance" : "power";

	if (!d)
		return;
	while ((e = readdir(d))) {
		char path[PATH_MAX];

		if (strncmp(e->d_name, "policy", 6) != 0)
			continue;
		snprintf(path, sizeof(path), CPUFREQ_DIR "/%s/scaling_governor",
				e->d_name);
		if (access(path, W_OK) == 0)
			write_str(path, gov);
		snprintf(path, sizeof(path),
				CPUFREQ_DIR "/%s/energy_performance_preference",
				e->d_name);
		if (access(path, W_OK) == 0)
			write_str(path, epp);
	}
	closedir(d);
}

static const char *
profile_path(void)
{
	if (access(ACPI_PROFILE, F_OK) == 0)
		return ACPI_PROFILE;
	if (access(MSI_PROFILE, F_OK) == 0)
		return MSI_PROFILE;
	return NULL;
}

int
power_profile_get(char *buf, size_t len)
{
	const char *path = profile_path();
	FILE *fp;

	if (!path || !(fp = fopen(path, "r")))
		return -1;
	if (!fgets(buf, (int)len, fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	buf[strcspn(buf, "\n")] = '\0';
	return buf[0] ? 0 : -1;
}

int
power_profile_set(const char *value)
{
	const char *path = profile_path();

	if (!path || !value || !value[0])
		return -1;
	return write_str(path, value);
}

/* Whole file, not one line: msi-ec's available_shift_modes is one mode
 * per line, so a single fgets saw only "eco" and power_profile_high
 * never found "turbo" — AC landed on comfort instead of max. */
static void
read_choices(const char *path, char *buf, size_t len)
{
	FILE *fp = fopen(path, "r");
	size_t n;

	buf[0] = '\0';
	if (!fp)
		return;
	n = fread(buf, 1, len - 1, fp);
	buf[n] = '\0';
	fclose(fp);
}

/* Lowest-power profile the backend offers. */
void
power_profile_low(void)
{
	const char *path = profile_path();
	char choices[256];

	if (!path)
		return;
	if (strcmp(path, MSI_PROFILE) == 0) {
		write_str(path, "eco");
		return;
	}
	read_choices(ACPI_CHOICES, choices, sizeof(choices));
	if (!choices[0] || strstr(choices, "low-power"))
		write_str(path, "low-power");
	else if (strstr(choices, "quiet"))
		write_str(path, "quiet");
	else
		write_str(path, "balanced");
}

/* ── async regime worker ─────────────────────────────────────────────
 * A full regime switch is ~100+ sysfs syscalls (per-policy governor +
 * EPP + cap writes, EC platform-profile write measured at ~100 ms) and
 * used to run on the compositor thread — landing exactly on unlock/
 * wake/AC-plug, where the stall is maximally visible.  Serialize the
 * writes on one worker with a latest-wins mailbox: rapid transitions
 * can't interleave their sysfs writes out of order. */
enum cc_job { CC_NONE = -1, CC_BATTERY, CC_AC, CC_DARK };

static pthread_mutex_t cc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cc_cond = PTHREAD_COND_INITIALIZER;
static int cc_pending = CC_NONE;
static double cc_pending_cap = 1.0;
static int cc_started;

static void *
cc_worker(void *arg)
{
	(void)arg;
	pthread_setname_np(pthread_self(), "nixly-cpuclock");
	for (;;) {
		int job;
		double cap;

		pthread_mutex_lock(&cc_lock);
		while (cc_pending == CC_NONE)
			pthread_cond_wait(&cc_cond, &cc_lock);
		job = cc_pending;
		cap = cc_pending_cap;
		cc_pending = CC_NONE;
		pthread_mutex_unlock(&cc_lock);

		switch (job) {
		case CC_BATTERY:
			power_profile_low();
			cpuclock_perf(0);
			cpuclock_boost(0);
			cpuclock_cap(cap);
			break;
		case CC_AC:
			power_profile_high();
			cpuclock_perf(1);
			cpuclock_boost(1);
			cpuclock_cap(cap);
			break;
		case CC_DARK:
			power_profile_low();
			cpuclock_boost(0);
			cpuclock_cap(cap);
			break;
		}
	}
	return NULL;
}

static void
cc_submit(int job, double cap)
{
	pthread_mutex_lock(&cc_lock);
	if (!cc_started) {
		pthread_t thr;

		if (pthread_create(&thr, NULL, cc_worker, NULL) == 0) {
			pthread_detach(thr);
			cc_started = 1;
		}
	}
	if (cc_started) {
		cc_pending = job;
		cc_pending_cap = cap;
		pthread_cond_signal(&cc_cond);
		pthread_mutex_unlock(&cc_lock);
		return;
	}
	pthread_mutex_unlock(&cc_lock);
	/* Thread start failed — fall back to the synchronous path. */
	switch (job) {
	case CC_BATTERY:
		power_profile_low();
		cpuclock_perf(0);
		cpuclock_boost(0);
		cpuclock_cap(cap);
		break;
	case CC_AC:
		power_profile_high();
		cpuclock_perf(1);
		cpuclock_boost(1);
		cpuclock_cap(cap);
		break;
	case CC_DARK:
		power_profile_low();
		cpuclock_boost(0);
		cpuclock_cap(cap);
		break;
	}
}

void
cpuclock_regime_battery_async(double cap)
{
	cc_submit(CC_BATTERY, cap);
}

void
cpuclock_regime_ac_async(void)
{
	cc_submit(CC_AC, 1.0);
}

void
cpuclock_regime_dark_async(void)
{
	cc_submit(CC_DARK, 0.0);
}

/* Highest-performance profile the backend offers. */
void
power_profile_high(void)
{
	const char *path = profile_path();
	char choices[256];

	if (!path)
		return;
	if (strcmp(path, MSI_PROFILE) == 0) {
		read_choices(MSI_CHOICES, choices, sizeof(choices));
		if (!choices[0] || strstr(choices, "turbo"))
			write_str(path, "turbo");
		else if (strstr(choices, "sport"))
			write_str(path, "sport");
		else
			write_str(path, "comfort");
		return;
	}
	read_choices(ACPI_CHOICES, choices, sizeof(choices));
	if (!choices[0] || strstr(choices, "performance"))
		write_str(path, "performance");
	else
		write_str(path, "balanced");
}
