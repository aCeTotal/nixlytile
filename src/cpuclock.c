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

static void
read_choices(const char *path, char *buf, size_t len)
{
	FILE *fp = fopen(path, "r");

	buf[0] = '\0';
	if (!fp)
		return;
	if (!fgets(buf, (int)len, fp))
		buf[0] = '\0';
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
