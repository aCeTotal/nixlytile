#include "map.h"

#include <math.h>
#include <string.h>

#include "link.h"

#define MIN(a, b)    ((a) < (b) ? (a) : (b))
#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#define QUALITY_STEP (VRS_QUALITY_MAX / (LADDER_STEPS - 1))
#define ROUND_NUM    3u        /* step up to 1.5 footprints */
#define ROUND_DEN    2u

/* Texel grid, one axis. */
struct axis {
	uint32_t texel;             /* render px */
	uint32_t size;              /* render px */
	float scale;                /* display px per render px */
	float focus;                /* display px */
};

/* One render size's zone geometry. */
struct rings {
	struct axis x, y;
	VkExtent2D grid;
	int periphery;              /* ladder step */
	float edge[LADDER_STEPS];   /* step's outer radius, display px */
	uint8_t code[LADDER_STEPS]; /* render code per step */
};

/* Texel columns of one ring. */
struct span {
	uint32_t first;
	uint32_t count;
};

static int
fits(VkExtent2D size, VkExtent2D want)
{
	return size.width <= want.width && size.height <= want.height;
}

/* Larger area, then wider. */
static int
beats(VkExtent2D a, VkExtent2D b)
{
	uint32_t area = a.width * a.height, best = b.width * b.height;

	return area > best || (area == best && a.width > b.width);
}

static uint8_t
best_code(const VkExtent2D *sizes, uint32_t count, VkExtent2D want)
{
	VkExtent2D best = { 1, 1 };
	uint32_t i;

	for (i = 0; i < count; i++)
		best = fits(sizes[i], want) && beats(sizes[i], best) ? sizes[i] : best;
	return (uint8_t)((uint32_t)__builtin_ctz(best.width) << CODE_WIDTH_SHIFT
		| (uint32_t)__builtin_ctz(best.height));
}

/* Largest supported size per want. */
struct rate_fit
map_fit(const VkExtent2D *sizes, uint32_t count)
{
	struct rate_fit fit;
	uint32_t i, w, h;

	for (i = 0; i < RATE_AXIS_STEPS * RATE_AXIS_STEPS; i++) {
		w = i / RATE_AXIS_STEPS;
		h = i % RATE_AXIS_STEPS;
		fit.code[w][h] = best_code(sizes, count, (VkExtent2D){ 1u << w, 1u << h });
	}
	return fit;
}

/* Quality 100 native, 0 coarsest. */
static uint32_t
map_periphery(uint32_t quality)
{
	return (VRS_QUALITY_MAX - quality + QUALITY_STEP / 2) / QUALITY_STEP;
}

int
zones_equal(const struct zones *a, const struct zones *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

/* Largest render step, footprint rounded. */
static uint32_t
axis_step(uint32_t footprint, uint32_t display, uint32_t render)
{
	uint32_t k = footprint;

	while (k && ((uint64_t)display << k) * ROUND_DEN
			> ((uint64_t)render << footprint) * ROUND_NUM)
		k--;
	return k;
}

static uint8_t
step_code(const struct map *m, VkExtent2D size, uint32_t step)
{
	VkExtent2D d = m->zones.display;

	return m->fit.code[axis_step((step + 1) / 2, d.width, size.width)]
		[axis_step(step / 2, d.height, size.height)];
}

static float
focus_px(uint32_t focus, uint32_t shift, uint32_t display)
{
	if (focus == VRS_FOCUS_NONE)
		return (float)display / 2.0f;
	return (float)(focus >> shift & VRS_FOCUS_MASK) / (float)VRS_UNIT * (float)display;
}

static float
per_height(uint32_t mille, uint32_t height)
{
	return (float)mille / (float)VRS_SIZE_UNIT * (float)height;
}

/* Inner steps still at 1x1. */
static int
native_steps(const struct rings *g)
{
	int n = 0;

	while (n < g->periphery && !g->code[n + 1])
		n++;
	return n;
}

/* Upscaler taps stay native. */
static float
fovea_pad(const struct rings *g, float ring, float diagonal)
{
	return fmaxf(diagonal - ring * (float)native_steps(g), 0.0f);
}

/* Sub-rings at least one texel. */
static struct rings
rings_of(const struct map *m, VkExtent2D size)
{
	const struct zones *z = &m->zones;
	VkExtent2D d = z->display;
	struct rings g = {
		.x = { m->texel.width, size.width, (float)d.width / (float)size.width,
			focus_px(z->focus, VRS_FOCUS_SHIFT, d.width) },
		.y = { m->texel.height, size.height, (float)d.height / (float)size.height,
			focus_px(z->focus, 0, d.height) },
		.grid = texel_grid(m->texel, size),
		.periphery = (int)map_periphery(z->quality),
	};
	float diagonal = hypotf((float)g.x.texel * g.x.scale, (float)g.y.texel * g.y.scale);
	float ring = fmaxf(per_height(z->blend, d.height) / (float)MAX(g.periphery - 1, 1), diagonal);
	float fovea = per_height(z->fovea, d.height);
	int i;

	for (i = 0; i <= g.periphery; i++)
		g.code[i] = step_code(m, size, (uint32_t)i);
	if (size.width != d.width || size.height != d.height)
		fovea += fovea_pad(&g, ring, diagonal);
	for (i = 0; i < g.periphery; i++)
		g.edge[i] = fovea + ring * (float)i;
	return g;
}

/* Display distance to nearest edge. */
static float
gap(const struct axis *a, uint32_t index)
{
	float lo = (float)(index * a->texel) * a->scale;
	float hi = (float)MIN((index + 1) * a->texel, a->size) * a->scale;

	if (a->focus < lo)
		return lo - a->focus;
	return a->focus > hi ? a->focus - hi : 0.0f;
}

/* Columns within reach of focus. */
static struct span
span_of(const struct axis *a, float reach, uint32_t columns)
{
	float width = (float)a->texel * a->scale, end = (float)(columns - 1);
	float first = fminf(fmaxf(ceilf((a->focus - reach) / width) - 1.0f, 0.0f), end);
	float last = fminf(floorf((a->focus + reach) / width), end);

	return (struct span){ (uint32_t)first, (uint32_t)(last - first) + 1 };
}

/* Farthest row covers every column. */
int
map_native(const struct map *m, VkExtent2D size)
{
	struct rings g = rings_of(m, size);
	int n = native_steps(&g);
	float dy, edge;

	if (n == g.periphery)
		return 1;
	dy = fmaxf(gap(&g.y, 0), gap(&g.y, g.grid.height - 1));
	edge = g.edge[n];
	return dy <= edge
		&& span_of(&g.x, sqrtf(edge * edge - dy * dy), g.grid.width).count == g.grid.width;
}

/* Periphery, then nested rings inward. */
static void
fill_row(const struct rings *g, uint32_t row, uint8_t *codes)
{
	float dy = gap(&g->y, row);
	struct span s;
	int step;

	memset(codes, g->code[g->periphery], g->grid.width);
	for (step = g->periphery - 1; step >= 0 && dy <= g->edge[step]; step--) {
		s = span_of(&g->x, sqrtf(g->edge[step] * g->edge[step] - dy * dy), g->grid.width);
		memset(codes + s.first, g->code[step], s.count);
	}
}

/* Row-major codes, one per texel. */
void
map_fill(const struct map *m, VkExtent2D size, uint8_t *codes)
{
	struct rings g = rings_of(m, size);
	uint32_t row;

	for (row = 0; row < g.grid.height; row++)
		fill_row(&g, row, codes + (size_t)row * g.grid.width);
}
