/* MSI EC fan tables via msi-ec. */
#include "nixlytile.h"

#define EC_MAX_TEMP 100
/* Six temperatures, then seven speeds. */
#define EC_POINTS   (2 * FAN_CURVE_PTS + 1)
#define EC_SPEED(i) (FAN_CURVE_PTS + (i))

static const char *const curve_files[] = {
	"/sys/devices/platform/msi-ec/cpu/fan_curve",
	"/sys/devices/platform/msi-ec/gpu/fan_curve",
};

static int
table_read(int is_gpu, unsigned table[EC_POINTS])
{
	FILE *f = fopen(curve_files[is_gpu], "re");
	int i = 0;

	if (!f)
		return -1;
	while (i < EC_POINTS && fscanf(f, "%u", &table[i]) == 1)
		i++;
	fclose(f);
	return i == EC_POINTS ? 0 : -1;
}

static int
table_write(int is_gpu, const unsigned table[EC_POINTS])
{
	char buf[128];
	int len = 0, fd, ok;

	for (int i = 0; i < EC_POINTS; i++)
		len += snprintf(buf + len, sizeof(buf) - len, "%u%c", table[i], i == EC_POINTS - 1 ? '\n' : ' ');
	fd = open(curve_files[is_gpu], O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ok = write(fd, buf, (size_t)len) == len;
	close(fd);
	return ok ? 0 : -1;
}

int
fan_ec_writable(int is_gpu)
{
	return access(curve_files[is_gpu], W_OK) == 0;
}

int
fan_ec_read_curve(int is_gpu, FanCurve *out)
{
	unsigned table[EC_POINTS];

	if (table_read(is_gpu, table) != 0)
		return -1;
	for (int i = 0; i < FAN_CURVE_PTS; i++) {
		out->temp[i] = (uint8_t)(table[i] > EC_MAX_TEMP ? EC_MAX_TEMP : table[i]);
		out->pct[i] = (uint8_t)(table[EC_SPEED(i + 1)] > 100 ? 100 : table[EC_SPEED(i + 1)]);
	}
	out->base = (uint8_t)(table[EC_SPEED(0)] > 100 ? 100 : table[EC_SPEED(0)]);
	return 0;
}

int
fan_ec_write_curve(int is_gpu, const FanCurve *c)
{
	unsigned table[EC_POINTS];

	table[EC_SPEED(0)] = c->base;
	for (int i = 0; i < FAN_CURVE_PTS; i++) {
		table[i] = c->temp[i] > EC_MAX_TEMP ? EC_MAX_TEMP : c->temp[i];
		table[EC_SPEED(i + 1)] = c->pct[i];
	}
	return table_write(is_gpu, table);
}

/* Manual speed is a flat table. */
int
fan_ec_write_flat(int is_gpu, int pct)
{
	unsigned table[EC_POINTS];

	if (table_read(is_gpu, table) != 0)
		return -1;
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	for (int i = 0; i <= FAN_CURVE_PTS; i++)
		table[EC_SPEED(i)] = (unsigned)pct;
	return table_write(is_gpu, table);
}
