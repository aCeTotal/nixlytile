#include "pattern.h"
#include "link.h"

#include <math.h>
#include <string.h>

#define OUT 99.0f

/* Ring radii in half-heights. */
static const float ladder[][PATTERN_RINGS - 1] = {
	{  OUT,   OUT,   OUT,   OUT },
	{ 1.85f,  OUT,   OUT,   OUT },
	{ 1.70f, 1.95f,  OUT,   OUT },
	{ 1.55f, 1.80f,  OUT,   OUT },
	{ 1.45f, 1.65f, 1.90f,  OUT },
	{ 1.35f, 1.55f, 1.75f,  OUT },
	{ 1.25f, 1.45f, 1.60f, 1.90f },
	{ 1.15f, 1.35f, 1.50f, 1.75f },
	{ 1.05f, 1.25f, 1.40f, 1.60f },
	{ 0.95f, 1.15f, 1.30f, 1.50f },
	{ 0.85f, 1.05f, 1.20f, 1.40f },
	{ 0.75f, 0.95f, 1.10f, 1.30f },
	{ 0.65f, 0.85f, 1.00f, 1.20f },
	{ 0.50f, 0.75f, 0.90f, 1.10f },
	{ 0.35f, 0.60f, 0.80f, 1.00f },
	{ 0.20f, 0.45f, 0.70f, 0.90f },
};

_Static_assert(sizeof(ladder) / sizeof(ladder[0]) == VRS_LEVELS,
	"one ladder row per level");

struct row {
	const struct pattern_grid *g;
	uint8_t *line;
	float y;
	float unit;
};

struct ring {
	float radius;
	uint8_t code;
};

static void
paint_span(const struct row *r, const struct pattern_focus *f, struct ring ring)
{
	float dy = r->y - f->y;
	float reach = ring.radius * f->scale * r->unit;
	float half, texel = (float)r->g->texel_w;
	int first, last;

	if (reach <= fabsf(dy))
		return;
	half = sqrtf(reach * reach - dy * dy);
	first = (int)ceilf((f->x - half) / texel - 0.5f);
	last = (int)floorf((f->x + half) / texel - 0.5f);
	first = first < 0 ? 0 : first;
	last = last >= (int)r->g->columns ? (int)r->g->columns - 1 : last;
	if (first <= last)
		memset(r->line + first, ring.code, (size_t)(last - first + 1));
}

/* Coarse first; finest wins. */
static void
paint_row(const struct row *r, const struct pattern *p)
{
	int k, f;

	memset(r->line, p->codes[PATTERN_RINGS - 1], r->g->columns);
	for (k = PATTERN_RINGS - 2; k >= 0; k--)
		for (f = 0; f < p->foci; f++)
			paint_span(r, &p->focus[f],
				(struct ring){ ladder[p->level][k], p->codes[k] });
}

void
pattern_paint(const struct pattern_grid *g, const struct pattern *p)
{
	struct row r = { .g = g, .unit = 0.5f * (float)g->height };
	uint32_t y;

	for (y = 0; y < g->rows; y++) {
		r.line = g->texels + (size_t)y * g->columns;
		r.y = ((float)y + 0.5f) * (float)g->texel_h;
		paint_row(&r, p);
	}
}

/* Shaded fraction; 1 is full. */
float
pattern_cost(const struct pattern_grid *g, const struct pattern *p)
{
	size_t i, n = (size_t)g->columns * g->rows;
	float sum = 0.0f;

	pattern_paint(g, p);
	for (i = 0; i < n; i++)
		sum += 1.0f / (float)((1u << (g->texels[i] >> 2)) << (g->texels[i] & 3u));
	return sum / (float)n;
}
