#include "control.h"
#include "pattern.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define RISE          0.6f
#define FALL          0.15f
#define RELAX         0.75f
#define RELAX_TARGET  0.8f
#define PIXEL_SHARE   0.8f
#define SHARE_WEIGHT  0.3f
#define SHARE_MIN     0.05f
#define MIN_COST_GAP  0.1f
#define HOLD_NS       500000000ULL
#define HOLD_MAX_NS   8000000000ULL
#define RELAPSE_NS    1000000000ULL
#define REFINE_NS     50000000ULL
#define COARSEN_STEP  2
#define URGENT        1.1f
#define STALE_NS      1000000000ULL

static int
fresh(const struct control *c, int level, uint64_t now)
{
	return c->seen[level] && now - c->seen[level] < STALE_NS;
}

/* Measured level far enough apart. */
static int
reference(const struct control *c, int level, uint64_t now)
{
	int i, best = -1;
	float gap;

	for (i = 0; i < VRS_LEVELS; i++) {
		gap = c->cost[i] - c->cost[level];
		if (!c->measured[i] || now - c->measured[i] >= STALE_NS
				|| (gap > -MIN_COST_GAP && gap < MIN_COST_GAP))
			continue;
		if (best < 0 || c->measured[i] > c->measured[best])
			best = i;
	}
	return best;
}

/* Shading share of GPU time. */
static void
learn_share(struct control *c, int level, uint64_t now)
{
	int other = reference(c, level, now);
	float gap, per_cost, full, share;

	if (other < 0)
		return;
	gap = c->cost[level] - c->cost[other];
	per_cost = (c->gpu[level] - c->gpu[other]) / gap;
	if (per_cost <= 0.0f)
		return;
	full = c->gpu[level] + per_cost * (1.0f - c->cost[level]);
	share = MIN(MAX(per_cost / full, SHARE_MIN), 1.0f);
	c->share = c->share > 0.0f ? c->share + SHARE_WEIGHT * (share - c->share)
		: share;
}

/* Rises fast, decays slowly. */
void
control_sample(struct control *c, const struct sample *s)
{
	float weight = s->gpu_ns > c->gpu[s->level] ? RISE : FALL;

	if (fresh(c, s->level, s->at))
		c->gpu[s->level] += weight * (s->gpu_ns - c->gpu[s->level]);
	else
		c->gpu[s->level] = s->gpu_ns;
	c->seen[s->level] = s->at;
	c->measured[s->level] = s->at;
	learn_share(c, s->level, s->at);
}

/* Load-invariant: scales current time. */
float
control_predict(const struct control *c, int level)
{
	float share = c->share > 0.0f ? c->share : PIXEL_SHARE;
	float full = c->gpu[c->level] / (1.0f - share + share * c->cost[c->level]);

	return full * (1.0f - share + share * c->cost[level]);
}

float
control_gpu(const struct control *c)
{
	return c->gpu[c->level];
}

static void
set_level(struct control *c, int level, uint64_t now)
{
	float guess = control_predict(c, level);

	c->level = level;
	if (fresh(c, level, now))
		return;
	c->gpu[level] = guess;
	c->seen[level] = now;
}

/* Lowest level predicted under target. */
static int
fitting(const struct control *c, float target)
{
	int level = 0;

	while (level < VRS_LEVELS - 1 && control_predict(c, level) > target)
		level++;
	return level;
}

static int
coarse_goal(const struct control *c, float budget)
{
	return MIN(MAX(fitting(c, VRS_FILL * budget), c->level + 1), VRS_LEVELS - 1);
}

/* Center sharpens up to fill. */
static int
fine_goal(const struct control *c, float budget)
{
	int level = fitting(c, RELAX_TARGET * budget);

	if (c->level <= PATTERN_LEVEL_SOFT)
		return MIN(level, c->level);
	return MIN(level, MAX(fitting(c, VRS_FILL * budget), PATTERN_LEVEL_SOFT));
}

/* Quickly undone refining holds longer. */
static void
hold_refining(struct control *c, uint64_t now)
{
	int relapse = c->moved_ns && now - c->moved_ns < RELAPSE_NS;

	if (relapse)
		c->hold_len = MIN(2 * c->hold_len, HOLD_MAX_NS);
	else if (now >= c->hold_ns)
		c->hold_len = MAX(c->hold_len / 2, HOLD_NS);
	c->hold_ns = now + c->hold_len;
	c->moved_ns = 0;
}

static void
choose_goal(struct control *c, float budget, uint64_t now)
{
	float gpu = c->gpu[c->level];
	float relax = c->level > PATTERN_LEVEL_SOFT ? VRS_FILL : RELAX;

	if (budget <= 0.0f) {
		c->goal = 0;
		return;
	}
	if (gpu > budget) {
		c->goal = MAX(c->goal, coarse_goal(c, budget));
		hold_refining(c, now);
		return;
	}
	if (c->goal > c->level)
		return;
	if (!c->level || gpu >= relax * budget || now < c->hold_ns) {
		c->goal = c->level;
		return;
	}
	c->goal = fine_goal(c, budget);
}

/* Gradual; jumps before missing frames. */
static int
approach(struct control *c, float budget, uint64_t now)
{
	int step = c->gpu[c->level] > URGENT * budget ? VRS_LEVELS : COARSEN_STEP;

	if (c->goal > c->level) {
		set_level(c, MIN(c->goal, c->level + step), now);
		return c->level;
	}
	if (c->goal == c->level || now - c->moved_ns < REFINE_NS)
		return c->level;
	set_level(c, c->level - 1, now);
	c->moved_ns = now;
	return c->level;
}

/* Rate-limited: no visible jumps. */
int
control_step(struct control *c, float budget, uint64_t now)
{
	choose_goal(c, budget, now);
	return approach(c, budget, now);
}
