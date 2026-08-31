/*
 * fan_ec.c — MSI EC fan tables, through the nixly-fand helper.
 *
 * Register layout (MControlCenter/isw, identical across the boards the
 * msi-ec kernel driver whitelists — msi-ec loading is our gate that the
 * layout applies):
 *   CPU: 7 speed bytes @0x72-0x78 (percent), 6 temp thresholds @0x6A-0x6F
 *   GPU: 7 speed bytes @0x8A-0x90,           6 temp thresholds @0x82-0x87
 * speed[0] applies below temp[0]; speed[i+1] between temp[i] and
 * temp[i+1].  Our FanCurve maps as speed[0] = pct[0] (floor) and
 * speed[i+1] = pct[i], so "at/above temp[i] → pct[i]" holds exactly.
 *
 * The EC does the temp lookup itself once the table is written (fan
 * mode "advanced"), so curves cost zero per-tick work here.  Runs on
 * fanwatch's worker thread only.
 */
#include "nixlytile.h"

#define EC_CPU_TEMP0  0x6A
#define EC_CPU_SPEED0 0x72
#define EC_GPU_TEMP0  0x82
#define EC_GPU_SPEED0 0x8A

int
fan_ec_read_curve(int is_gpu, FanCurve *out)
{
	int t0 = is_gpu ? EC_GPU_TEMP0 : EC_CPU_TEMP0;
	int s0 = is_gpu ? EC_GPU_SPEED0 : EC_CPU_SPEED0;

	for (int i = 0; i < FAN_CURVE_PTS; i++) {
		int t = fan_helper_ec_read(t0 + i);
		int p = fan_helper_ec_read(s0 + 1 + i);

		if (t < 0 || p < 0)
			return -1;
		out->temp[i] = (uint8_t)(t > 110 ? 110 : t);
		out->pct[i] = (uint8_t)(p > 100 ? 100 : p);
	}
	{
		int b = fan_helper_ec_read(s0);

		if (b < 0)
			return -1;
		out->base = (uint8_t)(b > 100 ? 100 : b);
	}
	return 0;
}

int
fan_ec_write_curve(int is_gpu, const FanCurve *c)
{
	int t0 = is_gpu ? EC_GPU_TEMP0 : EC_CPU_TEMP0;
	int s0 = is_gpu ? EC_GPU_SPEED0 : EC_CPU_SPEED0;

	if (fan_helper_ec_write(s0, c->base) != 0)
		return -1;
	for (int i = 0; i < FAN_CURVE_PTS; i++) {
		if (fan_helper_ec_write(t0 + i, c->temp[i]) != 0 ||
				fan_helper_ec_write(s0 + 1 + i, c->pct[i]) != 0)
			return -1;
	}
	return 0;
}

/* Manual speed on an EC fan = flat table (the EC always runs a table). */
int
fan_ec_write_flat(int is_gpu, int pct)
{
	int s0 = is_gpu ? EC_GPU_SPEED0 : EC_CPU_SPEED0;

	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	for (int i = 0; i <= FAN_CURVE_PTS; i++)
		if (fan_helper_ec_write(s0 + i, pct) != 0)
			return -1;
	return 0;
}
