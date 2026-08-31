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
	if (strstr(name, "coretemp") || strstr(name, "k10temp") ||
			strstr(name, "thinkpad") || strstr(name, "dell_smm") ||
			strstr(name, "asus") || strstr(name, "applesmc"))
		return FAN_DEV_CPU; /* laptop EC fans cool the CPU first */
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

/* Popup grouping: 0 = CPU, 1 = GPU, 2 = other (case fans, AIO pumps,
 * anything unrecognized). */
int
fan_entry_section(const FanDevice *dev, const FanEntry *fe)
{
	if (dev->type == FAN_DEV_MSI_EC)
		return fe->ec_is_gpu ? 1 : 0;
	switch (dev->type) {
	case FAN_DEV_CPU:
		return 0;
	case FAN_DEV_GPU_AMD:
	case FAN_DEV_GPU_NVIDIA:
	case FAN_DEV_GPU_INTEL:
		return 1;
	default:
		return 2;
	}
}

int
fan_shows_pct(const FanEntry *fe)
{
	return fe->msi_sysfs || fe->ctl == FAN_CTL_NVML;
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
			fe->ctl = fe->has_pwm ? FAN_CTL_PWM : FAN_CTL_NONE;
			fe->mode = fe->pwm_enable == 1 ?
					FAN_MODE_MANUAL : FAN_MODE_AUTO;
			fe->manual_pct = fe->has_pwm ?
					(int)lround(fe->pwm * 100.0 / 255.0) : 50;
			fan_curve_default(&fe->curve,
					fan_entry_section(dev, fe));

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

	{
		char mode[24];
		int advanced;

		mode[0] = '\0';
		sysfs_read_str(MSI_EC_DIR "/fan_mode", mode, sizeof(mode));
		advanced = strcmp(mode, "advanced") == 0;

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

			fe->ec_is_gpu = i == 1;
			fan_curve_default(&fe->curve, i);
			if (fan_helper_available() &&
					fan_ec_read_curve(i, &fe->factory) == 0) {
				fe->ctl = FAN_CTL_MSI_EC;
				if (advanced)
					fe->curve = fe->factory;
			} else {
				fe->ctl = FAN_CTL_NONE; /* monitor only */
			}
			fe->mode = advanced && fe->ctl == FAN_CTL_MSI_EC ?
					FAN_MODE_CURVE : FAN_MODE_AUTO;
			fe->manual_pct = 50;
			dev->fan_count++;
			fs->total_fans++;
		}
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
	fs->helper_ok = fan_helper_available();
	scan_hwmon(fs);
	scan_msi(fs);
	fan_nvml_scan(fs);
	return fs->total_fans;
}

/* Hottest CPU temp in the state — curve source for fans whose own
 * hwmon has no temperature (typical for case-fan controllers). */
static int
fan_cpu_temp_mc(FanState *fs)
{
	int best = 0;

	for (int d = 0; d < fs->ndevices; d++) {
		FanDevice *dev = &fs->devices[d];

		for (int f = 0; f < dev->fan_count; f++) {
			FanEntry *fe = &dev->fans[f];

			if (fan_entry_section(dev, fe) == 0 &&
					fe->temp_mc > best)
				best = fe->temp_mc;
		}
	}
	return best;
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

	fan_nvml_refresh(fs);

	if (fs->has_msi) {
		char buf[16];

		fs->cooler_boost_on =
			sysfs_read_str(MSI_EC_DIR "/cooler_boost",
					buf, sizeof(buf)) == 0 &&
			strcmp(buf, "on") == 0;
	}
	fs->helper_ok = fan_helper_available();
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

/* fan_mode must be "advanced" for the EC to honour our tables; back to
 * "auto" only once every msi fan is in Auto mode again. */
static void
msi_sync_fan_mode(FanState *fs)
{
	int custom = 0;

	if (!fs->has_msi)
		return;
	for (int d = 0; d < fs->ndevices; d++)
		for (int f = 0; f < fs->devices[d].fan_count; f++) {
			FanEntry *fe = &fs->devices[d].fans[f];

			if (fe->msi_sysfs && fe->mode != FAN_MODE_AUTO)
				custom = 1;
		}
	sysfs_write_str(MSI_EC_DIR "/fan_mode", custom ? "advanced" : "auto");
}

/* One pwm write with helper fallback for root-owned pwm files.
 * val 0-255 manual, -1 auto. */
static int
pwm_write(FanEntry *f, int val)
{
	char path[PATH_MAX];
	int ok;

	if (val < 0) {
		snprintf(path, sizeof(path), "%s/pwm%d_enable",
				f->hwmon_path, f->pwm_index);
		ok = sysfs_write_int(path, 2) == 0;
		if (!ok)
			ok = fan_helper_pwm(f->hwmon_path, f->pwm_index,
					-1) == 0;
		if (ok)
			f->pwm_enable = 2;
		return ok ? 0 : -1;
	}
	snprintf(path, sizeof(path), "%s/pwm%d_enable",
			f->hwmon_path, f->pwm_index);
	if (sysfs_write_int(path, 1) == 0) {
		f->pwm_enable = 1;
		snprintf(path, sizeof(path), "%s/pwm%d",
				f->hwmon_path, f->pwm_index);
		if (sysfs_write_int(path, val) == 0) {
			f->pwm = val;
			return 0;
		}
	}
	if (fan_helper_pwm(f->hwmon_path, f->pwm_index, val) == 0) {
		f->pwm_enable = 1;
		f->pwm = val;
		return 0;
	}
	return -1;
}

/* Control writes — worker thread only (msi-ec writes stall like reads). */
void
fan_state_set_frac(FanState *fs, int flat, double frac)
{
	FanEntry *f = fan_flat_in(fs, flat);
	int pct;

	if (!f || f->ctl == FAN_CTL_NONE)
		return;
	if (frac < 0.0)
		frac = 0.0;
	if (frac > 1.0)
		frac = 1.0;
	pct = (int)lround(frac * 100.0);

	switch (f->ctl) {
	case FAN_CTL_PWM:
		if (pwm_write(f, (int)lround(frac * 255.0)) != 0)
			return;
		break;
	case FAN_CTL_MSI_EC:
		if (fan_ec_write_flat(f->ec_is_gpu, pct) != 0)
			return;
		break;
	case FAN_CTL_NVML:
		if (fan_nvml_set(f->nvml_gpu, f->nvml_fan, pct) != 0)
			return;
		break;
	default:
		return;
	}
	f->mode = FAN_MODE_MANUAL;
	f->manual_pct = pct;
	msi_sync_fan_mode(fs);
}

void
fan_state_set_auto(FanState *fs, int flat)
{
	FanEntry *f = fan_flat_in(fs, flat);

	if (!f || f->ctl == FAN_CTL_NONE)
		return;
	switch (f->ctl) {
	case FAN_CTL_PWM:
		if (pwm_write(f, -1) != 0)
			return;
		break;
	case FAN_CTL_MSI_EC:
		if (fan_ec_write_curve(f->ec_is_gpu, &f->factory) != 0)
			return;
		break;
	case FAN_CTL_NVML:
		if (fan_nvml_set(f->nvml_gpu, f->nvml_fan, -1) != 0)
			return;
		break;
	default:
		return;
	}
	f->mode = FAN_MODE_AUTO;
	msi_sync_fan_mode(fs);
}

void
fan_state_set_curve(FanState *fs, int flat, const FanCurve *c)
{
	FanEntry *f = fan_flat_in(fs, flat);

	if (!f || f->ctl == FAN_CTL_NONE)
		return;
	f->curve = *c;
	if (f->ctl == FAN_CTL_MSI_EC) {
		if (fan_ec_write_curve(f->ec_is_gpu, c) != 0)
			return;
	} else {
		f->curve_applied_pct = -1; /* force a write on next tick */
	}
	f->mode = FAN_MODE_CURVE;
	msi_sync_fan_mode(fs);
	fan_state_curve_tick(fs);
}

/* Software curve loop for hwmon/NVML fans (the MSI EC runs its table by
 * itself).  Called after every sample on fanwatch's thread; writes only
 * when the target moved ≥2% so idle ticks cost nothing. */
void
fan_state_curve_tick(FanState *fs)
{
	int cpu_mc = fan_cpu_temp_mc(fs);

	for (int d = 0; d < fs->ndevices; d++) {
		FanDevice *dev = &fs->devices[d];

		for (int f = 0; f < dev->fan_count; f++) {
			FanEntry *fe = &dev->fans[f];
			int temp_mc, pct;

			if (fe->mode != FAN_MODE_CURVE ||
					(fe->ctl != FAN_CTL_PWM &&
					 fe->ctl != FAN_CTL_NVML))
				continue;
			temp_mc = fe->temp_mc > 0 ? fe->temp_mc : cpu_mc;
			pct = (int)fan_curve_eval(&fe->curve,
					temp_mc / 1000);
			if (fe->curve_applied_pct >= 0 &&
					abs(pct - fe->curve_applied_pct) < 2)
				continue;
			if (fe->ctl == FAN_CTL_PWM ?
					pwm_write(fe, (int)lround(pct * 2.55)) == 0 :
					fan_nvml_set(fe->nvml_gpu,
							fe->nvml_fan, pct) == 0)
				fe->curve_applied_pct = pct;
		}
	}
}

/* Re-apply what fans.conf remembers, right after the initial scan.
 * Fans without a saved entry stay untouched (firmware default). */
void
fan_state_apply_saved(FanState *fs)
{
	int flat = 0;

	for (int d = 0; d < fs->ndevices; d++) {
		FanDevice *dev = &fs->devices[d];

		for (int f = 0; f < dev->fan_count; f++, flat++) {
			FanEntry *fe = &dev->fans[f];
			char key[96];
			int mode, pct;
			FanCurve c;

			if (fe->ctl == FAN_CTL_NONE)
				continue;
			fan_conf_key(dev, fe, key, sizeof(key));
			if (!fanconf_lookup(key, &mode, &pct, &c))
				continue;
			fe->curve = c;
			fe->manual_pct = pct;
			switch (mode) {
			case FAN_MODE_MANUAL:
				fan_state_set_frac(fs, flat, pct / 100.0);
				break;
			case FAN_MODE_CURVE:
				fan_state_set_curve(fs, flat, &c);
				break;
			default:
				break; /* auto = leave the firmware alone */
			}
		}
	}
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
