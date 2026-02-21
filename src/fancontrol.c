#include "nixlytile.h"
#include "client.h"
#include <sys/io.h>

static int
sysfs_read_int(const char *path)
{
	FILE *f;
	int val = -1;

	f = fopen(path, "r");
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

	if (!buf || len == 0)
		return -1;
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
sysfs_write_int(const char *path, int val)
{
	FILE *f;

	f = fopen(path, "w");
	if (!f)
		return -1;
	if (fprintf(f, "%d\n", val) < 0) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

/* MSI EC register map */
#define EC_CMD_PORT    0x66
#define EC_DATA_PORT   0x62
#define EC_CMD_READ    0x80
#define EC_REG_CPU_TEMP  0x68
#define EC_REG_CPU_FAN   0x71
#define EC_REG_GPU_TEMP  0x80
#define EC_REG_GPU_FAN   0x89
/* RPM tachometer registers (16-bit, big-endian) */
#define EC_REG_CPU_RPM_H 0xC8
#define EC_REG_CPU_RPM_L 0xC9
#define EC_REG_GPU_RPM_H 0xCA
#define EC_REG_GPU_RPM_L 0xCB

static int ec_io_ready;

static int
ec_init_io(void)
{
	if (ec_io_ready)
		return 0;
	if (ioperm(EC_DATA_PORT, 1, 1) != 0)
		return -1;
	if (ioperm(EC_CMD_PORT, 1, 1) != 0)
		return -1;
	ec_io_ready = 1;
	return 0;
}

static int
ec_wait_ibf_clear(void)
{
	for (int i = 0; i < 5000; i++) {
		if (!(inb(EC_CMD_PORT) & 0x02))
			return 0;
	}
	return -1;
}

static int
ec_wait_obf_set(void)
{
	for (int i = 0; i < 5000; i++) {
		if (inb(EC_CMD_PORT) & 0x01)
			return 0;
	}
	return -1;
}

static int
ec_read_reg(uint8_t addr)
{
	if (!ec_io_ready)
		return -1;
	if (ec_wait_ibf_clear() != 0)
		return -1;
	outb(EC_CMD_READ, EC_CMD_PORT);
	if (ec_wait_ibf_clear() != 0)
		return -1;
	outb(addr, EC_DATA_PORT);
	if (ec_wait_obf_set() != 0)
		return -1;
	return inb(EC_DATA_PORT);
}

/* Read 16-bit RPM from tachometer register pair (big-endian).
 * Returns actual RPM if valid, -1 otherwise. */
static int
ec_read_rpm(uint8_t reg_h, uint8_t reg_l)
{
	int h, l, rpm;

	if (!ec_io_ready)
		return -1;
	h = ec_read_reg(reg_h);
	l = ec_read_reg(reg_l);
	if (h < 0 || l < 0)
		return -1;
	rpm = (h << 8) | l;
	/* Validate: 0 means fan off, 100-20000 is reasonable RPM range */
	if (rpm == 0)
		return 0;
	if (rpm >= 100 && rpm <= 20000)
		return rpm;
	return -1;
}

static int
is_msi_laptop(void)
{
	char vendor[64];

	if (sysfs_read_str("/sys/class/dmi/id/sys_vendor", vendor, sizeof(vendor)) != 0)
		return 0;
	return strstr(vendor, "Micro-Star") != NULL;
}

static void
fan_scan_ec(FanPopup *p)
{
	FanDevice *dev;
	FanEntry *fe;
	int val;

	if (!p || !is_msi_laptop())
		return;
	if (ec_init_io() != 0)
		return;

	/* Verify we can read at least one register */
	val = ec_read_reg(EC_REG_CPU_FAN);
	if (val < 0)
		return;

	dev = &p->devices[p->device_count];
	snprintf(dev->name, sizeof(dev->name), "EC");
	dev->type = FAN_DEV_MSI_EC;
	dev->fan_count = 0;

	/* CPU fan */
	fe = &dev->fans[dev->fan_count];
	snprintf(fe->label, sizeof(fe->label), "CPU Fan");
	fe->ec_reg_rpm = EC_REG_CPU_FAN;
	fe->ec_reg_rpm_h = EC_REG_CPU_RPM_H;
	fe->ec_reg_rpm_l = EC_REG_CPU_RPM_L;
	fe->ec_reg_temp = EC_REG_CPU_TEMP;
	{
		int rpm = ec_read_rpm(EC_REG_CPU_RPM_H, EC_REG_CPU_RPM_L);
		fe->rpm = rpm >= 0 ? rpm : val * 100;
	}
	fe->has_pwm = 0;
	val = ec_read_reg(EC_REG_CPU_TEMP);
	fe->temp_mc = val > 0 ? val * 1000 : 0;
	dev->fan_count++;
	p->total_fans++;

	/* GPU fan */
	val = ec_read_reg(EC_REG_GPU_FAN);
	if (val >= 0) {
		fe = &dev->fans[dev->fan_count];
		snprintf(fe->label, sizeof(fe->label), "GPU Fan");
		fe->ec_reg_rpm = EC_REG_GPU_FAN;
		fe->ec_reg_rpm_h = EC_REG_GPU_RPM_H;
		fe->ec_reg_rpm_l = EC_REG_GPU_RPM_L;
		fe->ec_reg_temp = EC_REG_GPU_TEMP;
		{
			int rpm = ec_read_rpm(EC_REG_GPU_RPM_H, EC_REG_GPU_RPM_L);
			fe->rpm = rpm >= 0 ? rpm : val * 100;
		}
		fe->has_pwm = 0;
		val = ec_read_reg(EC_REG_GPU_TEMP);
		fe->temp_mc = val > 0 ? val * 1000 : 0;
		dev->fan_count++;
		p->total_fans++;
	}

	p->device_count++;
}

static int
msi_sysfs_read_fan_mode(void)
{
	char buf[32];

	if (sysfs_read_str("/sys/devices/platform/msi-ec/fan_mode", buf, sizeof(buf)) != 0)
		return 0;
	if (strcmp(buf, "silent") == 0)
		return 1;
	if (strcmp(buf, "advanced") == 0)
		return 2;
	return 0; /* auto */
}

static int
msi_sysfs_read_shift_mode(void)
{
	char buf[32];

	if (sysfs_read_str("/sys/devices/platform/msi-ec/shift_mode", buf, sizeof(buf)) != 0)
		return 0;
	if (strcmp(buf, "comfort") == 0)
		return 1;
	if (strcmp(buf, "sport") == 0)
		return 2;
	if (strcmp(buf, "turbo") == 0)
		return 3;
	return 0; /* eco */
}

static int
msi_sysfs_read_cooler_boost(void)
{
	char buf[32];

	if (sysfs_read_str("/sys/devices/platform/msi-ec/cooler_boost", buf, sizeof(buf)) != 0)
		return 0;
	return strcmp(buf, "on") == 0;
}

static int
msi_sysfs_write_str(const char *path, const char *val)
{
	FILE *f;

	f = fopen(path, "w");
	if (!f)
		return -1;
	if (fputs(val, f) == EOF) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static void
fan_scan_msi_sysfs(FanPopup *p)
{
	FanDevice *dev;
	FanEntry *fe;
	int val;

	if (!p || !is_msi_laptop())
		return;

	/* Check if msi-ec sysfs exists */
	val = sysfs_read_int("/sys/devices/platform/msi-ec/cpu/realtime_fan_speed");
	if (val < 0)
		return;

	dev = &p->devices[p->device_count];
	snprintf(dev->name, sizeof(dev->name), "msi-ec");
	dev->type = FAN_DEV_MSI_EC;
	dev->fan_count = 0;

	/* Try EC direct access for precise RPM tachometer */
	ec_init_io();

	/* CPU fan */
	fe = &dev->fans[dev->fan_count];
	snprintf(fe->label, sizeof(fe->label), "CPU Fan");
	fe->msi_sysfs = 1;
	snprintf(fe->msi_sysfs_dir, sizeof(fe->msi_sysfs_dir), "cpu");
	fe->ec_reg_rpm_h = EC_REG_CPU_RPM_H;
	fe->ec_reg_rpm_l = EC_REG_CPU_RPM_L;
	{
		int rpm = ec_read_rpm(EC_REG_CPU_RPM_H, EC_REG_CPU_RPM_L);
		fe->rpm = rpm >= 0 ? rpm : val * 100;
	}
	fe->has_pwm = 0;
	val = sysfs_read_int("/sys/devices/platform/msi-ec/cpu/realtime_temperature");
	fe->temp_mc = val > 0 ? val * 1000 : 0;
	dev->fan_count++;
	p->total_fans++;

	/* GPU fan */
	val = sysfs_read_int("/sys/devices/platform/msi-ec/gpu/realtime_fan_speed");
	if (val >= 0) {
		fe = &dev->fans[dev->fan_count];
		snprintf(fe->label, sizeof(fe->label), "GPU Fan");
		fe->msi_sysfs = 1;
		snprintf(fe->msi_sysfs_dir, sizeof(fe->msi_sysfs_dir), "gpu");
		fe->ec_reg_rpm_h = EC_REG_GPU_RPM_H;
		fe->ec_reg_rpm_l = EC_REG_GPU_RPM_L;
		{
			int rpm = ec_read_rpm(EC_REG_GPU_RPM_H, EC_REG_GPU_RPM_L);
			fe->rpm = rpm >= 0 ? rpm : val * 100;
		}
		fe->has_pwm = 0;
		val = sysfs_read_int("/sys/devices/platform/msi-ec/gpu/realtime_temperature");
		fe->temp_mc = val > 0 ? val * 1000 : 0;
		dev->fan_count++;
		p->total_fans++;
	}

	p->device_count++;

	/* Read system-wide MSI EC controls */
	p->msi_ec = 1;
	p->fan_mode = msi_sysfs_read_fan_mode();
	p->shift_mode = msi_sysfs_read_shift_mode();
	p->cooler_boost = msi_sysfs_read_cooler_boost();

	/* Default to silent fan mode */
	if (p->fan_mode != 1) {
		if (msi_sysfs_write_str("/sys/devices/platform/msi-ec/fan_mode",
					"silent") == 0)
			p->fan_mode = 1;
	}
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
	if (strstr(name, "msi"))
		return FAN_DEV_MSI_EC;
	return FAN_DEV_UNKNOWN;
}

static const char *
fan_dev_type_label(FanDevType type)
{
	switch (type) {
	case FAN_DEV_CPU:          return "CPU";
	case FAN_DEV_CASE:         return "Motherboard";
	case FAN_DEV_GPU_AMD:      return "GPU AMD";
	case FAN_DEV_GPU_NVIDIA:   return "GPU NVIDIA";
	case FAN_DEV_GPU_INTEL:    return "GPU Intel";
	case FAN_DEV_MSI_EC:       return "MSI EC";
	case FAN_DEV_UNKNOWN:      return "Fan";
	}
	return "Fan";
}

void
fan_scan_hwmon(FanPopup *p)
{
	DIR *dir;
	struct dirent *ent;
	char path[256], namebuf[64];
	int dev_idx = 0;

	if (!p)
		return;

	memset(p->devices, 0, sizeof(p->devices));
	p->device_count = 0;
	p->total_fans = 0;

	dir = opendir("/sys/class/hwmon");
	if (!dir)
		return;

	while ((ent = readdir(dir)) != NULL && dev_idx < FAN_MAX_DEVICES) {
		int fan_count = 0;

		if (ent->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name", ent->d_name);
		if (sysfs_read_str(path, namebuf, sizeof(namebuf)) != 0)
			continue;

		/* Check if this hwmon has any fans */
		for (int fi = 1; fi <= 10 && fan_count < FAN_MAX_PER_DEV; fi++) {
			char fan_path[256];
			int rpm;

			snprintf(fan_path, sizeof(fan_path),
					"/sys/class/hwmon/%s/fan%d_input", ent->d_name, fi);
			rpm = sysfs_read_int(fan_path);
			if (rpm < 0)
				continue;

			FanDevice *dev = &p->devices[dev_idx];
			FanEntry *fe = &dev->fans[fan_count];

			snprintf(dev->hwmon_path, sizeof(dev->hwmon_path),
					"/sys/class/hwmon/%s", ent->d_name);
			snprintf(dev->name, sizeof(dev->name), "%s", namebuf);
			dev->type = classify_hwmon(namebuf);

			snprintf(fe->hwmon_path, sizeof(fe->hwmon_path),
					"/sys/class/hwmon/%s", ent->d_name);
			fe->fan_index = fi;
			fe->rpm = rpm;

			/* Try to read fan label */
			snprintf(fan_path, sizeof(fan_path),
					"/sys/class/hwmon/%s/fan%d_label", ent->d_name, fi);
			if (sysfs_read_str(fan_path, fe->label, sizeof(fe->label)) != 0)
				snprintf(fe->label, sizeof(fe->label), "%s Fan %d",
						fan_dev_type_label(dev->type), fi);

			/* Check for PWM control */
			fe->pwm_index = fi;
			snprintf(fan_path, sizeof(fan_path),
					"/sys/class/hwmon/%s/pwm%d", ent->d_name, fi);
			fe->pwm = sysfs_read_int(fan_path);
			fe->has_pwm = (fe->pwm >= 0);

			if (fe->has_pwm) {
				snprintf(fan_path, sizeof(fan_path),
						"/sys/class/hwmon/%s/pwm%d_enable", ent->d_name, fi);
				fe->pwm_enable = sysfs_read_int(fan_path);
				if (fe->pwm_enable < 0)
					fe->pwm_enable = 2; /* assume auto */
			}

			/* Read associated temperature */
			snprintf(fan_path, sizeof(fan_path),
					"/sys/class/hwmon/%s/temp1_input", ent->d_name);
			fe->temp_mc = sysfs_read_int(fan_path);

			fan_count++;
			p->total_fans++;
		}

		if (fan_count > 0) {
			p->devices[dev_idx].fan_count = fan_count;
			dev_idx++;
		}
	}

	closedir(dir);
	p->device_count = dev_idx;

	if (p->total_fans == 0)
		fan_scan_msi_sysfs(p);
	if (p->total_fans == 0)
		fan_scan_ec(p);
}

void
fan_read_all(FanPopup *p)
{
	char path[256];

	if (!p)
		return;

	if (p->msi_ec) {
		p->fan_mode = msi_sysfs_read_fan_mode();
		p->shift_mode = msi_sysfs_read_shift_mode();
		p->cooler_boost = msi_sysfs_read_cooler_boost();
	}

	for (int d = 0; d < p->device_count; d++) {
		FanDevice *dev = &p->devices[d];

		for (int f = 0; f < dev->fan_count; f++) {
			FanEntry *fe = &dev->fans[f];

			if (fe->msi_sysfs) {
				int val, rpm;
				/* Try EC tachometer for precise RPM */
				rpm = ec_read_rpm(fe->ec_reg_rpm_h, fe->ec_reg_rpm_l);
				if (rpm >= 0) {
					fe->rpm = rpm;
				} else {
					snprintf(path, sizeof(path),
							"/sys/devices/platform/msi-ec/%s/realtime_fan_speed",
							fe->msi_sysfs_dir);
					val = sysfs_read_int(path);
					fe->rpm = val > 0 ? val * 100 : 0;
				}
				snprintf(path, sizeof(path),
						"/sys/devices/platform/msi-ec/%s/realtime_temperature",
						fe->msi_sysfs_dir);
				val = sysfs_read_int(path);
				fe->temp_mc = val > 0 ? val * 1000 : 0;
				continue;
			}

			if (fe->ec_reg_rpm) {
				/* Try 16-bit tachometer first */
				int rpm = ec_read_rpm(fe->ec_reg_rpm_h, fe->ec_reg_rpm_l);
				if (rpm >= 0) {
					fe->rpm = rpm;
				} else {
					int val = ec_read_reg(fe->ec_reg_rpm);
					fe->rpm = val > 0 ? val * 100 : 0;
				}
				if (fe->ec_reg_temp) {
					int val = ec_read_reg(fe->ec_reg_temp);
					fe->temp_mc = val > 0 ? val * 1000 : 0;
				}
				continue;
			}

			snprintf(path, sizeof(path), "%s/fan%d_input",
					fe->hwmon_path, fe->fan_index);
			fe->rpm = sysfs_read_int(path);
			if (fe->rpm < 0)
				fe->rpm = 0;

			if (fe->has_pwm) {
				snprintf(path, sizeof(path), "%s/pwm%d",
						fe->hwmon_path, fe->pwm_index);
				fe->pwm = sysfs_read_int(path);
				if (fe->pwm < 0)
					fe->pwm = 0;

				snprintf(path, sizeof(path), "%s/pwm%d_enable",
						fe->hwmon_path, fe->pwm_index);
				fe->pwm_enable = sysfs_read_int(path);
				if (fe->pwm_enable < 0)
					fe->pwm_enable = 2;
			}

			snprintf(path, sizeof(path), "%s/temp1_input",
					fe->hwmon_path);
			fe->temp_mc = sysfs_read_int(path);
		}
	}
}

void
fan_write_pwm(FanEntry *f, int pwm)
{
	char path[256];

	if (!f || !f->has_pwm)
		return;

	if (pwm < 0) pwm = 0;
	if (pwm > 255) pwm = 255;

	snprintf(path, sizeof(path), "%s/pwm%d", f->hwmon_path, f->pwm_index);
	if (sysfs_write_int(path, pwm) == 0)
		f->pwm = pwm;
}

void
fan_set_manual(FanEntry *f)
{
	char path[256];

	if (!f || !f->has_pwm)
		return;

	snprintf(path, sizeof(path), "%s/pwm%d_enable",
			f->hwmon_path, f->pwm_index);
	if (sysfs_write_int(path, 1) == 0)
		f->pwm_enable = 1;
}

void
fan_set_auto(FanEntry *f)
{
	char path[256];

	if (!f || !f->has_pwm)
		return;

	snprintf(path, sizeof(path), "%s/pwm%d_enable",
			f->hwmon_path, f->pwm_index);
	if (sysfs_write_int(path, 2) == 0)
		f->pwm_enable = 2;
}

/* ── GPU fan boost for game mode ──────────────────────────────────── */

#define FAN_BOOST_MAX_SAVED 8

static struct {
	char hwmon_path[128];
	int pwm_index;
	int saved_pwm_enable;
	int saved_pwm;
} fan_boost_saved[FAN_BOOST_MAX_SAVED];
static int fan_boost_saved_count;

static int
is_gpu_hwmon(const char *name)
{
	if (!name || !*name)
		return 0;
	return strstr(name, "amdgpu") || strstr(name, "nvidia") ||
	       strstr(name, "nouveau") || strstr(name, "i915") ||
	       strstr(name, "xe");
}

void
fan_boost_activate(void)
{
	DIR *dir;
	struct dirent *ent;
	char path[256], namebuf[64];

	if (fan_boost_active)
		return;

	fan_boost_saved_count = 0;

	dir = opendir("/sys/class/hwmon");
	if (!dir) {
		wlr_log(WLR_INFO, "Fan boost: cannot open /sys/class/hwmon");
		return;
	}

	while ((ent = readdir(dir)) != NULL &&
	       fan_boost_saved_count < FAN_BOOST_MAX_SAVED) {
		if (ent->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), "/sys/class/hwmon/%s/name",
				ent->d_name);
		if (sysfs_read_str(path, namebuf, sizeof(namebuf)) != 0)
			continue;

		if (!is_gpu_hwmon(namebuf))
			continue;

		/* Found a GPU hwmon — boost all its PWM fans */
		for (int fi = 1; fi <= 6 && fan_boost_saved_count < FAN_BOOST_MAX_SAVED; fi++) {
			int cur_pwm, cur_enable;

			snprintf(path, sizeof(path),
					"/sys/class/hwmon/%s/pwm%d", ent->d_name, fi);
			cur_pwm = sysfs_read_int(path);
			if (cur_pwm < 0)
				continue;

			snprintf(path, sizeof(path),
					"/sys/class/hwmon/%s/pwm%d_enable", ent->d_name, fi);
			cur_enable = sysfs_read_int(path);
			if (cur_enable < 0)
				cur_enable = 2;

			/* Save current state */
			snprintf(fan_boost_saved[fan_boost_saved_count].hwmon_path,
					sizeof(fan_boost_saved[0].hwmon_path),
					"/sys/class/hwmon/%s", ent->d_name);
			fan_boost_saved[fan_boost_saved_count].pwm_index = fi;
			fan_boost_saved[fan_boost_saved_count].saved_pwm_enable = cur_enable;
			fan_boost_saved[fan_boost_saved_count].saved_pwm = cur_pwm;
			fan_boost_saved_count++;

			/* Set manual mode and full speed */
			snprintf(path, sizeof(path),
					"/sys/class/hwmon/%s/pwm%d_enable", ent->d_name, fi);
			sysfs_write_int(path, 1);

			snprintf(path, sizeof(path),
					"/sys/class/hwmon/%s/pwm%d", ent->d_name, fi);
			sysfs_write_int(path, 255);
		}
	}
	closedir(dir);

	/* MSI EC cooler boost */
	if (is_msi_laptop()) {
		msi_sysfs_write_str("/sys/devices/platform/msi-ec/cooler_boost", "on");
	}

	fan_boost_active = 1;
	wlr_log(WLR_INFO, "Fan boost activated (%d GPU fan(s) set to max)",
			fan_boost_saved_count);
}

void
fan_boost_deactivate(void)
{
	char path[256];

	if (!fan_boost_active)
		return;

	for (int i = 0; i < fan_boost_saved_count; i++) {
		/* Restore pwm_enable first (may switch back to auto) */
		snprintf(path, sizeof(path), "%s/pwm%d_enable",
				fan_boost_saved[i].hwmon_path,
				fan_boost_saved[i].pwm_index);
		sysfs_write_int(path, fan_boost_saved[i].saved_pwm_enable);

		/* If the saved mode was manual, restore the saved PWM value too */
		if (fan_boost_saved[i].saved_pwm_enable == 1) {
			snprintf(path, sizeof(path), "%s/pwm%d",
					fan_boost_saved[i].hwmon_path,
					fan_boost_saved[i].pwm_index);
			sysfs_write_int(path, fan_boost_saved[i].saved_pwm);
		}
	}

	/* MSI EC cooler boost off */
	if (is_msi_laptop()) {
		msi_sysfs_write_str("/sys/devices/platform/msi-ec/cooler_boost", "off");
	}

	fan_boost_active = 0;
	fan_boost_saved_count = 0;
	wlr_log(WLR_INFO, "Fan boost deactivated - GPU fans restored to auto");
}

/* Get flat fan entry by index across all devices */
static FanEntry *
fan_entry_by_flat_idx(FanPopup *p, int idx)
{
	int n = 0;

	if (!p || idx < 0)
		return NULL;

	for (int d = 0; d < p->device_count; d++) {
		for (int f = 0; f < p->devices[d].fan_count; f++) {
			if (n == idx)
				return &p->devices[d].fans[f];
			n++;
		}
	}
	return NULL;
}

void
renderfanpopup(Monitor *m)
{
	FanPopup *p;
	int padding, line_spacing, row_height;
	int y, popup_w;
	int slider_w = 120;
	int slider_h = 10;
	int label_col_w = 0;
	int rpm_col_w = 0;
	int col_gap;
	char line[128];
	struct wlr_scene_node *node, *tmp;
	uint64_t now = monotonic_msec();
	int flat_idx;

	if (!m || !m->statusbar.fan_popup.tree)
		return;

	p = &m->statusbar.fan_popup;
	padding = statusbar_module_padding;
	line_spacing = 4;
	col_gap = 12;
	row_height = statusfont.height > 0 ? statusfont.height : 16;

	/* Clear previous content but keep bg */
	wl_list_for_each_safe(node, tmp, &p->tree->children, link) {
		if (p->bg && node == &p->bg->node)
			continue;
		wlr_scene_node_destroy(node);
	}

	if (!statusfont.font || p->total_fans <= 0) {
		p->width = p->height = 0;
		if (p->tree)
			wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->visible = 0;
		return;
	}

	/* Refresh fan data */
	if (p->last_fetch_ms == 0 || now - p->last_fetch_ms >= 2000) {
		fan_read_all(p);
		p->last_fetch_ms = now;
	}

	/* Measure columns */
	for (int d = 0; d < p->device_count; d++) {
		FanDevice *dev = &p->devices[d];
		int header_w;

		/* Header: "CPU (coretemp) -- 62C" */
		if (dev->fans[0].temp_mc > 0) {
			snprintf(line, sizeof(line), "%s (%s) -- %d\302\260C",
					fan_dev_type_label(dev->type), dev->name,
					dev->fans[0].temp_mc / 1000);
		} else {
			snprintf(line, sizeof(line), "%s (%s)",
					fan_dev_type_label(dev->type), dev->name);
		}
		header_w = status_text_width(line);
		/* header_w is checked against total later */

		for (int f = 0; f < dev->fan_count; f++) {
			FanEntry *fe = &dev->fans[f];
			int lw, rw;

			lw = status_text_width(fe->label);
			if (lw > label_col_w)
				label_col_w = lw;

			snprintf(line, sizeof(line), "%d RPM", fe->rpm);
			rw = status_text_width(line);
			if (rw > rpm_col_w)
				rpm_col_w = rw;
		}

		(void)header_w;
	}

	popup_w = 2 * padding + label_col_w + col_gap + rpm_col_w + col_gap + slider_w;
	/* Ensure header fits */
	for (int d = 0; d < p->device_count; d++) {
		FanDevice *dev = &p->devices[d];
		int header_w;

		if (dev->fans[0].temp_mc > 0) {
			snprintf(line, sizeof(line), "%s (%s) -- %d\302\260C",
					fan_dev_type_label(dev->type), dev->name,
					dev->fans[0].temp_mc / 1000);
		} else {
			snprintf(line, sizeof(line), "%s (%s)",
					fan_dev_type_label(dev->type), dev->name);
		}
		header_w = status_text_width(line) + 2 * padding;
		if (header_w > popup_w)
			popup_w = header_w;
	}
	if (popup_w < 200)
		popup_w = 200;

	/* Ensure msi-ec control labels fit */
	if (p->msi_ec) {
		static const char *fm_labels[] = { "Fan: auto", "Fan: silent", "Fan: advanced" };
		static const char *sm_labels[] = { "Perf: eco", "Perf: comfort", "Perf: sport", "Perf: turbo" };
		int w;

		for (int i = 0; i < 3; i++) {
			w = status_text_width(fm_labels[i]) + 2 * padding;
			if (w > popup_w) popup_w = w;
		}
		for (int i = 0; i < 4; i++) {
			w = status_text_width(sm_labels[i]) + 2 * padding;
			if (w > popup_w) popup_w = w;
		}
		w = status_text_width("Cooler Boost: ON") + 2 * padding;
		if (w > popup_w) popup_w = w;
	}

	/* Calculate height */
	y = padding;
	for (int d = 0; d < p->device_count; d++) {
		FanDevice *dev = &p->devices[d];

		y += row_height + line_spacing; /* header */
		for (int f = 0; f < dev->fan_count; f++) {
			y += row_height + line_spacing; /* fan row */
		}
		y += line_spacing; /* group spacing */
	}
	if (p->msi_ec) {
		/* separator + 3 control rows */
		y += 4 + line_spacing;
		y += 3 * (row_height + line_spacing);
	}
	y += padding - line_spacing; /* remove last spacing, add bottom padding */

	p->width = popup_w;
	p->height = y;

	/* Background */
	if (!p->bg && !(p->bg = wlr_scene_tree_create(p->tree)))
		return;
	wlr_scene_node_set_enabled(&p->bg->node, 1);
	wlr_scene_node_set_position(&p->bg->node, 0, 0);
	wl_list_for_each_safe(node, tmp, &p->bg->children, link)
		wlr_scene_node_destroy(node);
	drawrect(p->bg, 0, 0, p->width, p->height, statusbar_popup_bg);

	/* Render content */
	y = padding;
	flat_idx = 0;
	for (int d = 0; d < p->device_count; d++) {
		FanDevice *dev = &p->devices[d];
		struct wlr_scene_tree *row;
		StatusModule mod = {0};

		/* Device header */
		if (dev->fans[0].temp_mc > 0) {
			snprintf(line, sizeof(line), "%s (%s) -- %d\302\260C",
					fan_dev_type_label(dev->type), dev->name,
					dev->fans[0].temp_mc / 1000);
		} else {
			snprintf(line, sizeof(line), "%s (%s)",
					fan_dev_type_label(dev->type), dev->name);
		}
		row = wlr_scene_tree_create(p->tree);
		if (row) {
			wlr_scene_node_set_position(&row->node, padding, y);
			mod.tree = row;
			tray_render_label(&mod, line, 0, row_height, statusbar_fg);
		}
		y += row_height + line_spacing;

		/* Fan rows */
		for (int f = 0; f < dev->fan_count; f++) {
			FanEntry *fe = &dev->fans[f];
			int sx, sy;

			/* Label */
			row = wlr_scene_tree_create(p->tree);
			if (row) {
				wlr_scene_node_set_position(&row->node, padding + 8, y);
				mod.tree = row;
				tray_render_label(&mod, fe->label, 0, row_height, statusbar_fg);
			}

			/* RPM */
			snprintf(line, sizeof(line), "%d RPM", fe->rpm);
			row = wlr_scene_tree_create(p->tree);
			if (row) {
				wlr_scene_node_set_position(&row->node,
						padding + 8 + label_col_w + col_gap, y);
				mod.tree = row;
				tray_render_label(&mod, line, 0, row_height, statusbar_fg);
			}

			/* Slider */
			sx = padding + 8 + label_col_w + col_gap + rpm_col_w + col_gap;
			sy = y + (row_height - slider_h) / 2;

			fe->slider_x = sx;
			fe->slider_y = sy;
			fe->slider_w = slider_w;
			fe->slider_h = slider_h;
			fe->row_y = y;
			fe->row_h = row_height;

			/* Slider background (dark) */
			{
				static const float slider_bg[] = {0.2f, 0.2f, 0.2f, 1.0f};
				drawrect(p->tree, sx, sy, slider_w, slider_h, slider_bg);
			}

			if (fe->has_pwm) {
				if (fe->pwm_enable == 1) {
					/* Manual mode - show fill */
					int fill_w = (fe->pwm * slider_w) / 255;
					if (fill_w < 0) fill_w = 0;
					if (fill_w > slider_w) fill_w = slider_w;

					/* Color gradient: blue (low) to red (high) */
					float frac = (float)fe->pwm / 255.0f;
					float fill_col[4];
					fill_col[0] = 0.2f + 0.6f * frac;     /* R */
					fill_col[1] = 0.4f * (1.0f - frac);    /* G */
					fill_col[2] = 0.8f * (1.0f - frac);    /* B */
					fill_col[3] = 1.0f;

					if (fill_w > 0)
						drawrect(p->tree, sx, sy, fill_w, slider_h, fill_col);
				} else {
					/* Auto mode - show "AUTO" label on slider */
					row = wlr_scene_tree_create(p->tree);
					if (row) {
						int auto_w = status_text_width("AUTO");
						int auto_x = sx + (slider_w - auto_w) / 2;
						int auto_y = sy - 1;
						static const float auto_col[] = {0.5f, 0.7f, 0.5f, 1.0f};
						wlr_scene_node_set_position(&row->node, auto_x, auto_y);
						mod.tree = row;
						tray_render_label(&mod, "AUTO", 0, slider_h + 2, auto_col);
					}
				}
			}

			flat_idx++;
			y += row_height + line_spacing;
		}
		y += line_spacing; /* group spacing */
	}

	/* MSI EC controls */
	if (p->msi_ec) {
		static const char *fan_mode_names[] = { "auto", "silent", "advanced" };
		static const char *shift_mode_names[] = { "eco", "comfort", "sport", "turbo" };
		static const float ctrl_col[] = {0.6f, 0.8f, 1.0f, 1.0f};
		static const float boost_on_col[] = {1.0f, 0.4f, 0.3f, 1.0f};
		static const float sep_col[] = {0.3f, 0.3f, 0.3f, 1.0f};
		struct wlr_scene_tree *row;
		StatusModule mod = {0};

		/* Separator line */
		drawrect(p->tree, padding, y, popup_w - 2 * padding, 1, sep_col);
		y += 4 + line_spacing;

		/* Fan mode */
		p->fanmode_y = y;
		p->fanmode_h = row_height;
		snprintf(line, sizeof(line), "Fan: %s",
				fan_mode_names[p->fan_mode % 3]);
		row = wlr_scene_tree_create(p->tree);
		if (row) {
			wlr_scene_node_set_position(&row->node, padding, y);
			mod.tree = row;
			tray_render_label(&mod, line, 0, row_height, ctrl_col);
		}
		y += row_height + line_spacing;

		/* Shift mode */
		p->shiftmode_y = y;
		p->shiftmode_h = row_height;
		snprintf(line, sizeof(line), "Perf: %s",
				shift_mode_names[p->shift_mode % 4]);
		row = wlr_scene_tree_create(p->tree);
		if (row) {
			wlr_scene_node_set_position(&row->node, padding, y);
			mod.tree = row;
			tray_render_label(&mod, line, 0, row_height, ctrl_col);
		}
		y += row_height + line_spacing;

		/* Cooler boost */
		p->boost_y = y;
		p->boost_h = row_height;
		snprintf(line, sizeof(line), "Cooler Boost: %s",
				p->cooler_boost ? "ON" : "OFF");
		row = wlr_scene_tree_create(p->tree);
		if (row) {
			wlr_scene_node_set_position(&row->node, padding, y);
			mod.tree = row;
			tray_render_label(&mod, line, 0, row_height,
					p->cooler_boost ? boost_on_col : ctrl_col);
		}
		y += row_height + line_spacing;
	}

	p->last_render_ms = now;
}

int
fan_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	FanPopup *p;
	int rel_x, rel_y;
	int popup_x;

	if (!m || !m->statusbar.fan_popup.visible)
		return 0;

	p = &m->statusbar.fan_popup;
	if (!p->tree || p->width <= 0 || p->height <= 0)
		return 0;

	popup_x = m->statusbar.fan.x;
	if (p->width > 0 && m->statusbar.area.width > 0) {
		int max_x = m->statusbar.area.width - p->width;
		if (max_x < 0) max_x = 0;
		if (popup_x > max_x) popup_x = max_x;
		if (popup_x < 0) popup_x = 0;
	}

	rel_x = lx - popup_x;
	rel_y = ly - m->statusbar.area.height;
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;

	/* Find which fan row was clicked */
	{
		int flat = 0;

		for (int d = 0; d < p->device_count; d++) {
			for (int f = 0; f < p->devices[d].fan_count; f++) {
				FanEntry *fe = &p->devices[d].fans[f];

				if (!fe->has_pwm) {
					flat++;
					continue;
				}

				/* Right-click on row: toggle auto/manual */
				if (button == BTN_RIGHT &&
						rel_y >= fe->row_y && rel_y < fe->row_y + fe->row_h) {
					if (fe->pwm_enable == 1)
						fan_set_auto(fe);
					else
						fan_set_manual(fe);
					renderfanpopup(m);
					return 1;
				}

				/* Left-click on slider: set PWM */
				if (button == BTN_LEFT &&
						rel_x >= fe->slider_x &&
						rel_x < fe->slider_x + fe->slider_w &&
						rel_y >= fe->slider_y &&
						rel_y < fe->slider_y + fe->slider_h) {
					int new_pwm;

					if (fe->pwm_enable != 1)
						fan_set_manual(fe);

					new_pwm = ((rel_x - fe->slider_x) * 255) / fe->slider_w;
					if (new_pwm < 0) new_pwm = 0;
					if (new_pwm > 255) new_pwm = 255;
					fan_write_pwm(fe, new_pwm);

					/* Start dragging */
					p->dragging = 1;
					p->drag_fan_idx = flat;

					renderfanpopup(m);
					return 1;
				}

				flat++;
			}
		}
	}

	/* MSI EC controls */
	if (p->msi_ec && button == BTN_LEFT) {
		static const char *fan_modes[] = { "auto", "silent", "advanced" };
		static const char *shift_modes[] = { "eco", "comfort", "sport", "turbo" };

		if (rel_y >= p->fanmode_y && rel_y < p->fanmode_y + p->fanmode_h) {
			int next = (p->fan_mode + 1) % 3;
			if (msi_sysfs_write_str("/sys/devices/platform/msi-ec/fan_mode",
						fan_modes[next]) == 0) {
				p->fan_mode = next;
				renderfanpopup(m);
			}
			return 1;
		}
		if (rel_y >= p->shiftmode_y && rel_y < p->shiftmode_y + p->shiftmode_h) {
			int next = (p->shift_mode + 1) % 4;
			if (msi_sysfs_write_str("/sys/devices/platform/msi-ec/shift_mode",
						shift_modes[next]) == 0) {
				p->shift_mode = next;
				renderfanpopup(m);
			}
			return 1;
		}
		if (rel_y >= p->boost_y && rel_y < p->boost_y + p->boost_h) {
			const char *val = p->cooler_boost ? "off" : "on";
			if (msi_sysfs_write_str("/sys/devices/platform/msi-ec/cooler_boost",
						val) == 0) {
				p->cooler_boost = !p->cooler_boost;
				renderfanpopup(m);
			}
			return 1;
		}
	}

	return 1; /* click was inside popup, consume it */
}

void
fan_popup_handle_drag(Monitor *m, double cx, double cy)
{
	FanPopup *p;
	FanEntry *fe;
	int popup_x, rel_x;
	int new_pwm;

	if (!m)
		return;

	p = &m->statusbar.fan_popup;
	if (!p->dragging || !p->visible)
		return;

	fe = fan_entry_by_flat_idx(p, p->drag_fan_idx);
	if (!fe || !fe->has_pwm) {
		p->dragging = 0;
		return;
	}

	popup_x = m->statusbar.fan.x;
	if (p->width > 0 && m->statusbar.area.width > 0) {
		int max_x = m->statusbar.area.width - p->width;
		if (max_x < 0) max_x = 0;
		if (popup_x > max_x) popup_x = max_x;
		if (popup_x < 0) popup_x = 0;
	}

	rel_x = (int)floor(cx) - m->statusbar.area.x - popup_x;
	new_pwm = ((rel_x - fe->slider_x) * 255) / fe->slider_w;
	if (new_pwm < 0) new_pwm = 0;
	if (new_pwm > 255) new_pwm = 255;

	fan_write_pwm(fe, new_pwm);
	renderfanpopup(m);
}

void
updatefanhover(Monitor *m, double cx, double cy)
{
	int lx, ly;
	int inside = 0;
	int popup_hover = 0;
	int was_visible;
	FanPopup *p;
	int popup_x;
	uint64_t now = monotonic_msec();

	if (!m || !m->showbar || !m->statusbar.fan.tree || !m->statusbar.fan_popup.tree) {
		if (m && m->statusbar.fan_popup.tree) {
			wlr_scene_node_set_enabled(&m->statusbar.fan_popup.tree->node, 0);
			m->statusbar.fan_popup.visible = 0;
		}
		return;
	}

	p = &m->statusbar.fan_popup;
	lx = (int)floor(cx) - m->statusbar.area.x;
	ly = (int)floor(cy) - m->statusbar.area.y;

	popup_x = m->statusbar.fan.x;
	if (p->width > 0 && m->statusbar.area.width > 0) {
		int max_x = m->statusbar.area.width - p->width;
		if (max_x < 0) max_x = 0;
		if (popup_x > max_x) popup_x = max_x;
		if (popup_x < 0) popup_x = 0;
	}

	/* Check if hovering popup area */
	if (p->visible && p->width > 0 && p->height > 0 &&
			lx >= popup_x &&
			lx < popup_x + p->width &&
			ly >= m->statusbar.area.height &&
			ly < m->statusbar.area.height + p->height) {
		popup_hover = 1;
	}

	/* Check if hovering fan module */
	if (lx >= m->statusbar.fan.x &&
			lx < m->statusbar.fan.x + m->statusbar.fan.width &&
			ly >= 0 && ly < m->statusbar.area.height &&
			m->statusbar.fan.width > 0) {
		inside = 1;
	} else if (popup_hover) {
		inside = 1;
	}

	/* Keep visible while dragging */
	if (p->dragging)
		inside = 1;

	was_visible = p->visible;

	if (inside) {
		if (p->hover_start_ms == 0)
			p->hover_start_ms = now;

		/* 300ms delay before showing */
		if (!was_visible && (now - p->hover_start_ms) < 300) {
			schedule_popup_delay(300 - (now - p->hover_start_ms) + 1);
			return;
		}

		if (!was_visible) {
			/* Initial scan on first show */
			if (p->device_count == 0)
				fan_scan_hwmon(p);
		}
		p->visible = 1;
		wlr_scene_node_set_enabled(&p->tree->node, 1);
		wlr_scene_node_set_position(&p->tree->node,
				popup_x, m->statusbar.area.height);
		if (!was_visible || (p->last_render_ms == 0 ||
					now - p->last_render_ms >= 2000)) {
			renderfanpopup(m);
		}
	} else if (p->visible || p->hover_start_ms != 0) {
		p->visible = 0;
		p->dragging = 0;
		wlr_scene_node_set_enabled(&p->tree->node, 0);
		p->last_render_ms = 0;
		p->hover_start_ms = 0;
	}
}

void
renderfan(StatusModule *module, int bar_height, const char *text)
{
	if (!module || !module->tree)
		return;

	render_icon_label(module, bar_height, text,
			ensure_fan_icon_buffer, &fan_icon_buf, &fan_icon_w, &fan_icon_h,
			0, statusbar_icon_text_gap, statusbar_fg);
}
