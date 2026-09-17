/*
 * fancurve.c — fan temp→speed curve model and fans.conf persistence.
 *
 * A curve is FAN_CURVE_PTS (temp, pct) pairs with step semantics: at or
 * above temp[i] the fan runs pct[i]; below temp[0] it runs pct[0].
 * This mirrors the MSI EC fan-table layout, so the same table drives
 * hwmon pwm fans (software loop on fanwatch's thread), NVML fans and
 * the EC directly.
 *
 * Persistence follows charge_limit.c: one small conf under
 * ~/.local/nixlyos/.  fanconf_load() runs once on fanwatch's worker
 * thread before the first apply; fanconf_store() runs on the compositor
 * thread after a UI edit, so the table is guarded by a mutex.
 */
#include "nixlytile.h"
#include <pthread.h>
#include <pwd.h>
#include <sys/stat.h>

#define FANCONF_MAX 32

typedef struct {
	char key[96];
	int mode;
	int manual_pct;
	FanCurve curve;
} SavedFan;

static SavedFan fc_saved[FANCONF_MAX];
static int fc_nsaved;
static pthread_mutex_t fc_lock = PTHREAD_MUTEX_INITIALIZER;

double
fan_curve_eval(const FanCurve *c, int temp_c)
{
	int pct = c->base;

	for (int i = 0; i < FAN_CURVE_PTS; i++)
		if (temp_c >= c->temp[i])
			pct = c->pct[i];
	return (double)pct;
}

/* The quiet default every fan starts on: silent until the silicon is
 * actually warm, then the smallest step that still holds the temperature.
 * 20% is the floor because most fans stall below it, and full speed is
 * reserved for the last few degrees before throttling.
 * section: 0 CPU, 1 GPU, 2 other. */
void
fan_curve_default(FanCurve *c, int section)
{
	static const uint8_t t_cpu[FAN_CURVE_PTS] = { 50, 60, 70, 80, 88, 94 };
	static const uint8_t p_cpu[FAN_CURVE_PTS] = { 20, 28, 38, 52, 75, 100 };
	static const uint8_t t_gpu[FAN_CURVE_PTS] = { 45, 55, 65, 75, 82, 87 };
	static const uint8_t p_gpu[FAN_CURVE_PTS] = { 20, 28, 38, 52, 70, 100 };
	static const uint8_t t_case[FAN_CURVE_PTS] = { 50, 60, 70, 80, 88, 95 };
	static const uint8_t p_case[FAN_CURVE_PTS] = { 20, 25, 35, 50, 70, 100 };
	const uint8_t *t = section == 1 ? t_gpu : section == 2 ? t_case : t_cpu;
	const uint8_t *p = section == 1 ? p_gpu : section == 2 ? p_case : p_cpu;

	memcpy(c->temp, t, FAN_CURVE_PTS);
	memcpy(c->pct, p, FAN_CURVE_PTS);
	/* CPU and GPU fans may stop when cold; an unidentified fan could be
	 * an AIO pump, and a stopped pump kills the loop. */
	c->base = section == 2 ? p[0] : 0;
}

/* Step lookup with 5°C hysteresis and one step down per tick: a
 * temperature sitting on a threshold would otherwise make the fan hunt
 * between two speeds forever, which is the opposite of quiet.
 * *step is the caller's remembered index, -1 = below the table. */
double
fan_curve_eval_stable(const FanCurve *c, int temp_c, int *step)
{
	int want = -1, cur = *step;

	for (int i = 0; i < FAN_CURVE_PTS; i++)
		if (temp_c >= c->temp[i])
			want = i;

	if (cur < -1 || cur >= FAN_CURVE_PTS)
		cur = want;
	else if (want > cur)
		cur = want;              /* heat needs an answer now */
	else if (want < cur && temp_c < (int)c->temp[cur] - FAN_CURVE_HYST_C)
		cur--;                   /* cooling is allowed to take its time */

	*step = cur;
	return cur < 0 ? (double)c->base : (double)c->pct[cur];
}

/* Stable per-fan key: device name + label, spaces flattened. */
void
fan_conf_key(const FanDevice *dev, const FanEntry *fe, char *buf, size_t len)
{
	size_t n;

	snprintf(buf, len, "%s:%s", dev->name, fe->label);
	for (n = 0; buf[n]; n++)
		if (buf[n] == ' ' || buf[n] == '\t')
			buf[n] = '_';
}

static const char *
fc_path(char *buf, size_t len)
{
	const char *home = getenv("HOME");

	if (!home || !*home) {
		struct passwd *pw = getpwuid(getuid());

		home = pw ? pw->pw_dir : NULL;
	}
	if (!home)
		return NULL;
	snprintf(buf, len, "%s/.local/nixlyos/fans.conf", home);
	return buf;
}

/* fan <key> <mode> <manual_pct> <base_pct> t:p t:p t:p t:p t:p t:p */
void
fanconf_load(void)
{
	char path[PATH_MAX], line[256];
	FILE *fp;

	if (!fc_path(path, sizeof(path)))
		return;
	fp = fopen(path, "r");
	if (!fp)
		return;
	pthread_mutex_lock(&fc_lock);
	fc_nsaved = 0;
	while (fc_nsaved < FANCONF_MAX && fgets(line, sizeof(line), fp)) {
		SavedFan *s = &fc_saved[fc_nsaved];
		int t[FAN_CURVE_PTS], p[FAN_CURVE_PTS], base;

		if (sscanf(line,
				"fan %95s %d %d %d %d:%d %d:%d %d:%d %d:%d %d:%d %d:%d",
				s->key, &s->mode, &s->manual_pct, &base,
				&t[0], &p[0], &t[1], &p[1], &t[2], &p[2],
				&t[3], &p[3], &t[4], &p[4], &t[5], &p[5]) != 16)
			continue;
		if (base < 0 || base > 100)
			continue;
		s->curve.base = (uint8_t)base;
		if (s->mode < 0 || s->mode > FAN_MODE_CURVE)
			continue;
		if (s->manual_pct < 0)
			s->manual_pct = 0;
		if (s->manual_pct > 100)
			s->manual_pct = 100;
		for (int i = 0; i < FAN_CURVE_PTS; i++) {
			if (t[i] < 0 || t[i] > 110 || p[i] < 0 || p[i] > 100)
				goto skip;
			s->curve.temp[i] = (uint8_t)t[i];
			s->curve.pct[i] = (uint8_t)p[i];
		}
		fc_nsaved++;
skip:		;
	}
	pthread_mutex_unlock(&fc_lock);
	fclose(fp);
}

int
fanconf_lookup(const char *key, int *mode, int *manual_pct, FanCurve *c)
{
	int found = 0;

	pthread_mutex_lock(&fc_lock);
	for (int i = 0; i < fc_nsaved; i++) {
		if (strcmp(fc_saved[i].key, key) != 0)
			continue;
		*mode = fc_saved[i].mode;
		*manual_pct = fc_saved[i].manual_pct;
		*c = fc_saved[i].curve;
		found = 1;
		break;
	}
	pthread_mutex_unlock(&fc_lock);
	return found;
}

static void
fc_save_locked(void)
{
	char path[PATH_MAX], dir[PATH_MAX];
	char *slash;
	FILE *fp;

	if (!fc_path(path, sizeof(path)))
		return;
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
	fp = fopen(path, "w");
	if (!fp)
		return;
	for (int i = 0; i < fc_nsaved; i++) {
		SavedFan *s = &fc_saved[i];

		fprintf(fp, "fan %s %d %d %d", s->key, s->mode,
				s->manual_pct, s->curve.base);
		for (int j = 0; j < FAN_CURVE_PTS; j++)
			fprintf(fp, " %d:%d", s->curve.temp[j], s->curve.pct[j]);
		fputc('\n', fp);
	}
	fclose(fp);
}

void
fanconf_store(const char *key, int mode, int manual_pct, const FanCurve *c)
{
	SavedFan *s = NULL;

	pthread_mutex_lock(&fc_lock);
	for (int i = 0; i < fc_nsaved; i++)
		if (strcmp(fc_saved[i].key, key) == 0) {
			s = &fc_saved[i];
			break;
		}
	if (!s && fc_nsaved < FANCONF_MAX)
		s = &fc_saved[fc_nsaved++];
	if (s) {
		snprintf(s->key, sizeof(s->key), "%s", key);
		s->mode = mode;
		s->manual_pct = manual_pct;
		s->curve = *c;
		fc_save_locked();
	}
	pthread_mutex_unlock(&fc_lock);
}
