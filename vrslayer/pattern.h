#ifndef NIXLY_VRS_PATTERN_H
#define NIXLY_VRS_PATTERN_H

#include <stdint.h>

/* 1x1, 2x1, 2x2, 4x2, 4x4 */
#define PATTERN_RINGS 5
#define PATTERN_FOCI  2
/* Coarser levels shrink sharp center. */
#define PATTERN_LEVEL_SOFT 7

struct pattern_grid {
	uint8_t *texels;
	uint32_t columns, rows;
	uint32_t texel_w, texel_h;
	uint32_t width, height;     /* render area, pixels */
};

struct pattern_focus {
	float x, y;                 /* pixels */
	float scale;                /* ring radius factor */
};

struct pattern {
	const uint8_t *codes;       /* rate texel per ring */
	int level;
	int foci;
	struct pattern_focus focus[PATTERN_FOCI];
};

void pattern_paint(const struct pattern_grid *g, const struct pattern *p);
float pattern_cost(const struct pattern_grid *g, const struct pattern *p);

#endif
