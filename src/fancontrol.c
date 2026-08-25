/*
 * fancontrol.c — fan enumeration and control for the statusbar fan
 * module (replaces mcontrolcenter).  Desktop fans come from hwmon
 * (fanN_input RPM, pwmN/pwmN_enable control); MSI laptops expose CPU
 * and GPU fan percent + temperature and the cooler-boost switch
 * through the msi-ec platform driver.
 *
 * Every function here that touches sysfs works on a caller-supplied
 * FanState and runs on fanwatch.c's worker thread — msi-ec reads go
 * through the same embedded controller as the battery, where a single
 * read costs ~100ms.  The compositor thread only reads fan_pub, the
 * snapshot fanwatch publishes.
 */
#include "nixlytile.h"

#include <dirent.h>

FanState fan_pub;

#define MSI_EC_DIR "/sys/devices/platform/msi-ec"

static int
sysfs_read_int(const char *path)
{
	FILE *f = fopen(path, "r");
	int val = -1;

	if (!f)
		return -1;
	if (fscanf(f, "%d", &val) != 1)
		val = -1;
	fclose(f);
	return val;
}

static int
sysfs_read_str(const char *path, char *buf, size_t len)
{
	FILE *f;
	char *nl;

	buf[0] = '\0';
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, (int)len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	nl = strchr(buf, '\n');
	if (nl)
		*nl = '\0';
	return 0;
}

static int
sysfs_write_str(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");
	int ok;

	if (!f)
		return -1;
	ok = fputs(val, f) >= 0;
	fclose(f);
	return ok ? 0 : -1;
}

static int
sysfs_write_int(const char *path, int val)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%d", val);
	return sysfs_write_str(path, buf);
}

static FanDevType
classify_hwmon(const char *name)
{
	if (!name || !*name)
		return FAN_DEV_UNKNOWN;
	if (strstr(name, "coretemp") || strstr(name, "k10temp"))
		return FAN_DEV_CPU;
	if (strstr(name, "it87") || strstr(name, "nct6") ||
			strstr(name, "nuvoton") || strstr(name, "w83"))
		return FAN_DEV_CASE;
	if (strstr(name, "amdgpu"))
		return FAN_DEV_GPU_AMD;
	if (strstr(name, "nvidia") || strstr(name, "nouveau"))
		return FAN_DEV_GPU_NVIDIA;
	if (strstr(name, "i915") || strstr(name, "xe"))
		return FAN_DEV_GPU_INTEL;
	return FAN_DEV_UNKNOWN;
}

static const char *
fan_dev_type_label(FanDevType type)
{
	switch (type) {
	case FAN_DEV_CPU:        return "CPU";
	case FAN_DEV_CASE:       return "Motherboard";
	case FAN_DEV_GPU_AMD:    return "GPU";
	case FAN_DEV_GPU_NVIDIA: return "GPU";
	case FAN_DEV_GPU_INTEL:  return "GPU";
	case FAN_DEV_MSI_EC:     return "Laptop";
	case FAN_DEV_UNKNOWN:    return "Fan";
	}
	return "Fan";
}

static void
scan_hwmon(FanState *fs)
{
	DIR *dir = opendir("/sys/class/hwmon");
	struct dirent *ent;
	char path[PATH_MAX], namebuf[64];

	if (!dir)
		return;

	while ((ent = readdir(dir)) && fs->ndevices < FAN_MAX_DEVICES) {
		FanDevice *dev = &fs->devices[fs->ndevices];
		int fan_count = 0;

		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name",
				ent->d_name);
		if (sysfs_read_str(path, namebuf, sizeof(namebuf)) != 0)
			continue;

		for (int fi = 1; fi <= 10 && fan_count < FAN_MAX_PER_DEV; fi++) {
			FanEntry *fe = &dev->fans[fan_count];
			int rpm;

			snprintf(path, sizeof(path),
					"/sys/class/hwmon/%s/fan%d_input",
					ent->d_name, fi);
			rpm = sysfs_read_int(path);
			if (rpm < 0)
				continue;

			snprintf(dev->hwmon_path, sizeof(dev->hwmon_path),
					"/sys/class/hwmon/%s", ent->d_name);
			snprintf(dev->name, sizeof(dev->name), "%s", namebuf);
			dev->type = classify_hwmon(namebuf);

			memset(fe, 0, sizeof(*fe));
			snprintf(fe->hwmon_path, sizeof(fe->hwmon_path),
					"%s", dev->hwmon_path);
			fe->fan_index = fi;
			fe->pwm_index = fi;
			fe->rpm = rpm;

			snprintf(path, sizeof(path), "%s/fan%d_label",
					fe->hwmon_path, fi);
			if (sysfs_read_str(path, fe->label,
						sizeof(fe->label)) != 0 ||
					!fe->label[0])
				snprintf(fe->label, sizeof(fe->label),
						"%s fan %d",
						fan_dev_type_label(dev->type), fi);

			snprintf(path, sizeof(path), "%s/pwm%d",
					fe->hwmon_path, fi);
			fe->pwm = sysfs_read_int(path);
			fe->has_pwm = fe->pwm >= 0;
			if (fe->has_pwm) {
				snprintf(path, sizeof(path), "%s/pwm%d_enable",
						fe->hwmon_path, fi);
				fe->pwm_enable = sysfs_read_int(path);
				if (fe->pwm_enable < 0)
					fe->pwm_enable = 2; /* assume auto */
			}

			snprintf(path, sizeof(path), "%s/temp1_input",
					fe->hwmon_path);
			fe->temp_mc = sysfs_read_int(path);

			fan_count++;
			fs->total_fans++;
		}

		if (fan_count > 0) {
			dev->fan_count = fan_count;
			fs->ndevices++;
		}
	}
	closedir(dir);
}

static void
scan_msi(FanState *fs)
{
	static const char *dirs[2] = { "cpu", "gpu" };
	static const char *labels[2] = { "CPU fan", "GPU fan" };
	FanDevice *dev;
	char path[PATH_MAX];

	if (fs->ndevices >= FAN_MAX_DEVICES)
		return;
	snprintf(path, sizeof(path), MSI_EC_DIR "/cpu/realtime_fan_speed");
	if (sysfs_read_int(path) < 0)
		return;

	dev = &fs->devices[fs->ndevices];
	snprintf(dev->name, sizeof(dev->name), "msi-ec");
	dev->type = FAN_DEV_MSI_EC;
	dev->fan_count = 0;

	for (int i = 0; i < 2; i++) {
		FanEntry *fe = &dev->fans[dev->fan_count];
		int val;

		snprintf(path, sizeof(path),
				MSI_EC_DIR "/%s/realtime_fan_speed", dirs[i]);
		val = sysfs_read_int(path);
		if (val < 0)
			continue;
		memset(fe, 0, sizeof(*fe));
		snprintf(fe->label, sizeof(fe->label), "%s", labels[i]);
		fe->msi_sysfs = 1;
		snprintf(fe->msi_sysfs_dir, sizeof(fe->msi_sysfs_dir),
				"%s", dirs[i]);
		fe->rpm = val; /* percent, not RPM */
		snprintf(path, sizeof(path),
				MSI_EC_DIR "/%s/realtime_temperature", dirs[i]);
		val = sysfs_read_int(path);
		fe->temp_mc = val > 0 ? val * 1000 : 0;
		dev->fan_count++;
		fs->total_fans++;
	}

	if (dev->fan_count > 0) {
		fs->ndevices++;
		fs->has_msi = 1;
		{
			char buf[16];

			fs->cooler_boost_on =
				sysfs_read_str(MSI_EC_DIR "/cooler_boost",
						buf, sizeof(buf)) == 0 &&
				strcmp(buf, "on") == 0;
		}
	}
}

int
fan_scan_state(FanState *fs)
{
	memset(fs, 0, sizeof(*fs));
	scan_hwmon(fs);
	scan_msi(fs);
	return fs->total_fans;
}

void
fan_refresh_state(FanState *fs)
{
	char path[PATH_MAX];

	for (int d = 0; d < fs->ndevices; d++) {
		FanDevice *dev = &fs->devices[d];

		for (int f = 0; f < dev->fan_count; f++) {
			FanEntry *fe = &dev->fans[f];
			int val;

			if (fe->msi_sysfs) {
				snprintf(path, sizeof(path),
						MSI_EC_DIR "/%s/realtime_fan_speed",
						fe->msi_sysfs_dir);
				val = sysfs_read_int(path);
				fe->rpm = val >= 0 ? val : 0;
				snprintf(path, sizeof(path),
						MSI_EC_DIR "/%s/realtime_temperature",
						fe->msi_sysfs_dir);
				val = sysfs_read_int(path);
				fe->temp_mc = val > 0 ? val * 1000 : 0;
				continue;
			}

			snprintf(path, sizeof(path), "%s/fan%d_input",
					fe->hwmon_path, fe->fan_index);
			val = sysfs_read_int(path);
			fe->rpm = val >= 0 ? val : 0;
			if (fe->has_pwm) {
				snprintf(path, sizeof(path), "%s/pwm%d",
						fe->hwmon_path, fe->pwm_index);
				val = sysfs_read_int(path);
				if (val >= 0)
					fe->pwm = val;
				snprintf(path, sizeof(path), "%s/pwm%d_enable",
						fe->hwmon_path, fe->pwm_index);
				val = sysfs_read_int(path);
				if (val >= 0)
					fe->pwm_enable = val;
			}
			snprintf(path, sizeof(path), "%s/temp1_input",
					fe->hwmon_path);
			fe->temp_mc = sysfs_read_int(path);
		}
	}

	if (fs->has_msi) {
		char buf[16];

		fs->cooler_boost_on =
			sysfs_read_str(MSI_EC_DIR "/cooler_boost",
					buf, sizeof(buf)) == 0 &&
			strcmp(buf, "on") == 0;
	}
}

static FanEntry *
fan_flat_in(FanState *fs, int idx)
{
	int n = 0;

	if (idx < 0)
		return NULL;
	for (int d = 0; d < fs->ndevices; d++)
		for (int f = 0; f < fs->devices[d].fan_count; f++)
			if (n++ == idx)
				return &fs->devices[d].fans[f];
	return NULL;
}

FanEntry *
fan_flat(int idx)
{
	return fan_flat_in(&fan_pub, idx);
}

/* Control writes — worker thread only (msi-ec writes stall like reads). */
void
fan_state_set_frac(FanState *fs, int flat, double frac)
{
	FanEntry *f = fan_flat_in(fs, flat);
	char path[PATH_MAX];
	int pwm;

	if (!f || !f->has_pwm)
		return;
	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	snprintf(path, sizeof(path), "%s/pwm%d_enable",
			f->hwmon_path, f->pwm_index);
	if (sysfs_write_int(path, 1) == 0)
		f->pwm_enable = 1;
	pwm = (int)lround(frac * 255.0);
	snprintf(path, sizeof(path), "%s/pwm%d",
			f->hwmon_path, f->pwm_index);
	if (sysfs_write_int(path, pwm) == 0)
		f->pwm = pwm;
}

void
fan_state_set_auto(FanState *fs, int flat)
{
	FanEntry *f = fan_flat_in(fs, flat);
	char path[PATH_MAX];

	if (!f || !f->has_pwm)
		return;
	snprintf(path, sizeof(path), "%s/pwm%d_enable",
			f->hwmon_path, f->pwm_index);
	if (sysfs_write_int(path, 2) == 0)
		f->pwm_enable = 2;
}

void
fan_state_set_boost(FanState *fs, int on)
{
	if (!fs->has_msi)
		return;
	if (sysfs_write_str(MSI_EC_DIR "/cooler_boost",
			on ? "on" : "off") == 0)
		fs->cooler_boost_on = on;
}

/* Bar text: MSI CPU-fan percent ("39%"), else the highest RPM. Returns
 * -1 when there are no fans (module hides). */
int
fan_primary_value(char *buf, size_t len)
{
	int best_rpm = -1;

	if (fan_pub.total_fans <= 0)
		return -1;
	for (int d = 0; d < fan_pub.ndevices; d++) {
		FanDevice *dev = &fan_pub.devices[d];

		for (int f = 0; f < dev->fan_count; f++) {
			FanEntry *fe = &dev->fans[f];

			if (fe->msi_sysfs) {
				snprintf(buf, len, "%d%%", fe->rpm);
				return 0;
			}
			if (fe->rpm > best_rpm)
				best_rpm = fe->rpm;
		}
	}
	snprintf(buf, len, "%d", best_rpm < 0 ? 0 : best_rpm);
	return 0;
}
