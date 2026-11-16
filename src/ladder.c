#include "nixlytile.h"
#include "diag.h"

#define LADDER_MIN_FPS      20.0f
#define LADDER_VRR_HEADROOM 5.0f
#define LADDER_VRR_STEP     0.95f
#define LADDER_SLACK        0.97f
#define LADDER_FILL         0.9f
#define EVAL_NS             250000000ULL
#define CALM_NS             2000000000ULL
#define PROBATION_NS        10000000000ULL
#define BACKOFF_MIN_NS      15000000000ULL
#define BACKOFF_MAX_NS      240000000000ULL
#define HOLD_MARGIN         0.6f
#define CPU_SHARE           0.8f
#define NS_PER_S            1e9f

static float
refresh_hz(Monitor *m)
{
	return (float)m->nx_output->refresh / 1000.0f;
}

/* Mode refresh over present timing. */
uint64_t
rung_vblank_ns(Monitor *m)
{
	float hz = refresh_hz(m);

	return hz > 0.0f ? (uint64_t)(NS_PER_S / hz) : m->present_interval_ns;
}

/* N vblanks; VRR: 5% steps. */
uint64_t
rung_interval(Monitor *m, int rung)
{
	float fps;

	if (!m->game_vrr_active)
		return (uint64_t)(rung + 1) * rung_vblank_ns(m);
	fps = (refresh_hz(m) - LADDER_VRR_HEADROOM) * powf(LADDER_VRR_STEP, (float)rung);
	return (uint64_t)(NS_PER_S / fps);
}

float
rung_fps(Monitor *m, int rung)
{
	return NS_PER_S / (float)rung_interval(m, rung);
}

/* Slack admits 59.94 Hz rungs. */
static int
rung_max(Monitor *m)
{
	uint64_t slowest = (uint64_t)(NS_PER_S / (LADDER_MIN_FPS * LADDER_SLACK));
	int rung = 0;

	while (rung_interval(m, rung + 1) <= slowest)
		rung++;
	return rung;
}

/* Fastest rung fitting frame time. */
static int
rung_for(Monitor *m, uint64_t frame_ns)
{
	int rung, top = rung_max(m);

	for (rung = 0; rung < top; rung++)
		if ((float)rung_interval(m, rung) >= (float)frame_ns * LADDER_SLACK)
			break;
	return rung;
}

static uint64_t
work_p90(const DynRender *d)
{
	uint64_t sorted[DR_WORK_SLOTS], v;
	int i, j;

	for (i = 0; i < d->work_n; i++) {
		v = d->work[i];
		for (j = i; j > 0 && sorted[j - 1] > v; j--)
			sorted[j] = sorted[j - 1];
		sorted[j] = v;
	}
	return d->work_n ? sorted[d->work_n * 9 / 10] : 0;
}

static void
set_rung(Monitor *m, int rung, uint64_t now)
{
	DynRender *d = &m->dr;

	d->rung = rung;
	d->calm_ns = now;
	d->fresh = 1;
	memset(d->miss_ns, 0, sizeof(d->miss_ns));
	diag_logf("DYNR", "%s: %.1f fps", m->nx_output->name, rung_fps(m, rung));
}

static void
hold_raises(DynRender *d, uint64_t now)
{
	d->hold_ns = now + d->backoff_ns;
	d->backoff_ns = MIN(2 * d->backoff_ns, BACKOFF_MAX_NS);
}

/* Held: only clear headroom raises. */
static void
raise_rung(Monitor *m, uint64_t now)
{
	DynRender *d = &m->dr;
	float margin = now < d->hold_ns ? HOLD_MARGIN : 1.0f;
	float work;
	int rung = 0;

	if (d->rung == 0 || d->work_n < DR_WORK_SLOTS / 2)
		return;
	work = (float)work_p90(d);
	while (rung < d->rung && work > CPU_SHARE * margin * (float)rung_interval(m, rung))
		rung++;
	if (rung == d->rung)
		return;
	d->raise_ns = now;
	set_rung(m, rung, now);
}

/* Start at game's current rung. */
void
ladder_start(Monitor *m, uint64_t now)
{
	DynRender *d = &m->dr;
	float fps = m->estimated_game_fps;

	d->work_n = d->work_idx = 0;
	d->released_ns = d->raise_ns = d->hold_ns = 0;
	d->backoff_ns = BACKOFF_MIN_NS;
	d->eval_ns = now;
	set_rung(m, fps > 0.0f ? rung_for(m, (uint64_t)(NS_PER_S / fps)) : 0, now);
}

/* Raises wait for calm. */
void
ladder_tick(Monitor *m, uint64_t now)
{
	DynRender *d = &m->dr;

	if (now - d->eval_ns < EVAL_NS)
		return;
	d->eval_ns = now;
	if (d->raise_ns && now - d->raise_ns >= PROBATION_NS) {
		d->raise_ns = 0;
		d->backoff_ns = BACKOFF_MIN_NS;
	}
	if (now - d->calm_ns < CALM_NS)
		return;
	raise_rung(m, now);
}

/* Jump to rung work fits. */
void
ladder_missed(Monitor *m, uint64_t now)
{
	DynRender *d = &m->dr;
	int target = rung_for(m, (uint64_t)((float)work_p90(d) / LADDER_FILL));

	if (d->raise_ns && now - d->raise_ns < PROBATION_NS)
		hold_raises(d, now);
	d->raise_ns = 0;
	target = MIN(MAX(target, d->rung + 1), rung_max(m));
	if (target != d->rung)
		set_rung(m, target, now);
}
