/*
 * fan_nvml.c — desktop NVIDIA GPU fans via NVML (dlopen'd, no build
 * dep).  The proprietary driver exposes no hwmon pwm; NVML is the only
 * fan interface.  Reads (speed %, temp) work unprivileged; the set
 * calls are root-only in the driver, so writes try NVML directly (in
 * case we ever run privileged) and fall back to the nixly-fand helper.
 *
 * Deliberately inert on laptops with an EC (msi-ec present): there the
 * EC owns the fans and NVML reports N/A anyway, and nvmlInit would keep
 * an Optimus dGPU awake for nothing.  Worker thread only.
 */
#include "nixlytile.h"
#include <dlfcn.h>

typedef int nvret;
typedef void *nvdev;

static void *nv_lib;
static int nv_ready = -1;   /* -1 untried, 0 unavailable, 1 up */
static unsigned nv_count;
static nvdev nv_dev[4];

static nvret (*p_init)(void);
static nvret (*p_dev_count)(unsigned *);
static nvret (*p_dev_by_index)(unsigned, nvdev *);
static nvret (*p_dev_name)(nvdev, char *, unsigned);
static nvret (*p_get_temp)(nvdev, int, unsigned *);
static nvret (*p_num_fans)(nvdev, unsigned *);
static nvret (*p_get_fan_v2)(nvdev, unsigned, unsigned *);
static nvret (*p_set_fan_v2)(nvdev, unsigned, unsigned);
static nvret (*p_set_default_fan)(nvdev, unsigned);

static int
nv_setup(void)
{
	if (nv_ready >= 0)
		return nv_ready;
	nv_ready = 0;
	if (access("/proc/driver/nvidia/version", F_OK) != 0)
		return 0;
	nv_lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
	if (!nv_lib)
		nv_lib = dlopen("/run/opengl-driver/lib/libnvidia-ml.so.1",
				RTLD_NOW);
	if (!nv_lib)
		return 0;
	/* object-pointer → function-pointer via memcpy (ISO C pedantry) */
#define NVSYM(fp, name) do { \
		void *sym_ = dlsym(nv_lib, name); \
		memcpy(&(fp), &sym_, sizeof(fp)); \
	} while (0)
	NVSYM(p_init, "nvmlInit_v2");
	NVSYM(p_dev_count, "nvmlDeviceGetCount_v2");
	NVSYM(p_dev_by_index, "nvmlDeviceGetHandleByIndex_v2");
	NVSYM(p_dev_name, "nvmlDeviceGetName");
	NVSYM(p_get_temp, "nvmlDeviceGetTemperature");
	NVSYM(p_num_fans, "nvmlDeviceGetNumFans");
	NVSYM(p_get_fan_v2, "nvmlDeviceGetFanSpeed_v2");
	NVSYM(p_set_fan_v2, "nvmlDeviceSetFanSpeed_v2");
	NVSYM(p_set_default_fan, "nvmlDeviceSetDefaultFanSpeed_v2");
#undef NVSYM
	if (!p_init || !p_dev_count || !p_dev_by_index || !p_get_temp ||
			!p_num_fans || !p_get_fan_v2)
		return 0;
	if (p_init() != 0)
		return 0;
	if (p_dev_count(&nv_count) != 0 || nv_count == 0)
		return 0;
	if (nv_count > 4)
		nv_count = 4;
	for (unsigned i = 0; i < nv_count; i++)
		if (p_dev_by_index(i, &nv_dev[i]) != 0)
			return 0;
	nv_ready = 1;
	return 1;
}

/* Append one FanDevice per NVIDIA GPU that reports fans.  Returns fans
 * added.  Skipped entirely on msi-ec laptops (see header comment). */
int
fan_nvml_scan(FanState *fs)
{
	int added = 0;

	if (fs->has_msi || !nv_setup())
		return 0;
	for (unsigned g = 0; g < nv_count &&
			fs->ndevices < FAN_MAX_DEVICES; g++) {
		FanDevice *dev = &fs->devices[fs->ndevices];
		char name[64] = "NVIDIA GPU";
		unsigned nfans = 0, pct;

		if (p_num_fans(nv_dev[g], &nfans) != 0 || nfans == 0)
			continue;
		if (nfans > FAN_MAX_PER_DEV)
			nfans = FAN_MAX_PER_DEV;
		if (p_dev_name)
			p_dev_name(nv_dev[g], name, sizeof(name));
		memset(dev, 0, sizeof(*dev));
		snprintf(dev->name, sizeof(dev->name), "%s", name);
		dev->type = FAN_DEV_GPU_NVIDIA;
		for (unsigned f = 0; f < nfans; f++) {
			FanEntry *fe = &dev->fans[dev->fan_count];

			const char *ln = name;

			memset(fe, 0, sizeof(*fe));
			/* card name on the row: "RTX 4090" not "GPU fan" */
			if (strncmp(ln, "NVIDIA ", 7) == 0)
				ln += 7;
			if (strncmp(ln, "GeForce ", 8) == 0)
				ln += 8;
			if (!ln[0])
				ln = "GPU";
			if (nfans > 1)
				snprintf(fe->label, sizeof(fe->label),
						"%s fan %u", ln, f + 1);
			else
				snprintf(fe->label, sizeof(fe->label),
						"%s", ln);
			fe->ctl = FAN_CTL_NVML;
			fe->nvml_gpu = (int)g;
			fe->nvml_fan = (int)f;
			fe->mode = FAN_MODE_AUTO;
			fan_curve_default(&fe->curve, 1);
			if (p_get_fan_v2(nv_dev[g], f, &pct) == 0)
				fe->rpm = (int)pct; /* percent */
			dev->fan_count++;
			fs->total_fans++;
			added++;
		}
		if (dev->fan_count > 0)
			fs->ndevices++;
	}
	return added;
}

void
fan_nvml_refresh(FanState *fs)
{
	if (nv_ready != 1)
		return;
	for (int d = 0; d < fs->ndevices; d++) {
		FanDevice *dev = &fs->devices[d];

		for (int f = 0; f < dev->fan_count; f++) {
			FanEntry *fe = &dev->fans[f];
			unsigned pct, temp;

			if (fe->ctl != FAN_CTL_NVML)
				continue;
			if (p_get_fan_v2(nv_dev[fe->nvml_gpu],
					(unsigned)fe->nvml_fan, &pct) == 0)
				fe->rpm = (int)pct;
			if (p_get_temp(nv_dev[fe->nvml_gpu], 0, &temp) == 0)
				fe->temp_mc = (int)temp * 1000;
		}
	}
}

int
fan_nvml_set(int gpu, int fan, int pct)
{
	if (nv_ready == 1) {
		nvret r;

		if (pct < 0)
			r = p_set_default_fan ?
				p_set_default_fan(nv_dev[gpu], (unsigned)fan) :
				-1;
		else
			r = p_set_fan_v2 ?
				p_set_fan_v2(nv_dev[gpu], (unsigned)fan,
						(unsigned)pct) : -1;
		if (r == 0)
			return 0;
	}
	return fan_helper_nv(gpu, fan, pct);
}
