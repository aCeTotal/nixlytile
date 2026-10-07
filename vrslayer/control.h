#ifndef NIXLY_VRS_CONTROL_H
#define NIXLY_VRS_CONTROL_H

#include <stdint.h>

#include "link.h"

/* One measured frame. */
struct sample {
	int level;
	float gpu_ns;
	uint64_t at;
};

/* GPU frame time per level. */
struct control {
	float cost[VRS_LEVELS];     /* shaded fraction */
	float gpu[VRS_LEVELS];      /* ns, spike-up average */
	uint64_t seen[VRS_LEVELS];
	uint64_t measured[VRS_LEVELS];
	float share;                /* shading share; 0 uses prior */
	int level;
	int goal;                   /* level being approached */
	uint64_t hold_ns;           /* no refining before */
	uint64_t hold_len;          /* grows when refining relapses */
	uint64_t moved_ns;          /* last refining step */
};

void control_sample(struct control *c, const struct sample *s);
int control_step(struct control *c, float budget_ns, uint64_t now);
float control_predict(const struct control *c, int level);
float control_gpu(const struct control *c);

#endif
