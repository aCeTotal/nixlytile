#include "passes.h"

#define COVER_MIN    0.9f     /* DRS scale still foveated */
#define KEEP         VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR
#define REPLACE      VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR
#define MIN_OP       VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MIN_KHR
#define OURS_KEEP    RATE_WORD(1, 1, KEEP, KEEP)
#define OURS_REPLACE RATE_WORD(1, 1, KEEP, REPLACE)
#define OURS_CAPPED  RATE_WORD(2, 2, KEEP, MIN_OP)

/* Inline fmaxf; NaN takes lo. */
static float
at_least(float v, float lo)
{
	return v > lo ? v : lo;
}

/* Inline fminf; NaN takes hi. */
static float
at_most(float v, float hi)
{
	return v < hi ? v : hi;
}

/* Clipped viewport covers COVER_MIN squared. */
static int
covers(const VkViewport *v, VkExtent2D area)
{
	float end = v->y + v->height;
	float left = at_least(v->x, 0.0f);
	float top = at_least(at_most(end, v->y), 0.0f);
	float right = at_most(v->x + v->width, (float)area.width);
	float bottom = at_most(at_least(end, v->y), (float)area.height);

	return right > left && (right - left) * (bottom - top)
		>= COVER_MIN * COVER_MIN * (float)area.width * (float)area.height;
}

/* Our map, not the app's. */
static int
zoned(const struct recording *r)
{
	return r->area.width != UNBOUNDED && !(r->guess && r->own);
}

/* Unknown: compare real, coverage off. */
static int
coarse(const struct recording *r)
{
	int scene = r->mode == MODE_DEPTH
		|| (r->mode == MODE_DYNAMIC && r->test == 1 && r->compares);

	return scene && zoned(r) && !r->multi && r->coverage != 1
		&& covers(&r->viewport, r->area);
}

/* App's rate, attachment 1x1. */
static uint32_t
alone(uint32_t held)
{
	uint32_t op = rate_field(held, FIELD_OP1);

	if (op == REPLACE || op == MIN_OP)
		return OURS_KEEP;
	return RATE_WORD(rate_field(held, FIELD_WIDTH), rate_field(held, FIELD_HEIGHT),
		rate_field(held, FIELD_OP0), KEEP);
}

/* Discards: ours capped, else app's. */
static uint32_t
capped(uint32_t held)
{
	if (!held || rate_field(held, FIELD_OP1) == REPLACE)
		return OURS_CAPPED;
	return alone(held);
}

/* Zoned: ours; elsewhere the app's. */
uint32_t
drawrule_rate(const struct recording *r)
{
	int replace = coarse(r);

	if (replace && r->discards)
		return capped(r->held);
	if (!r->held)
		return replace ? OURS_REPLACE : OURS_KEEP;
	return replace || !zoned(r) ? r->held : alone(r->held);
}
