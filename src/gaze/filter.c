#include <math.h>

#include "gaze_model.h"

/* Calm fixations, fast saccades. */
#define MIN_CUTOFF 0.5
#define SPEED_GAIN 1.5
#define SPEED_CUTOFF 1.0
#define TAU 6.283185307179586

static double
alpha(double cutoff, double dt)
{
	double tau = 1.0 / (TAU * cutoff);

	return 1.0 / (1.0 + tau / dt);
}

float
smooth(Smooth *s, float x, double t)
{
	double dt = t - s->t, cutoff;

	if (s->t <= 0 || dt <= 0 || dt > 1.0) {
		*s = (Smooth){ x, 0.0f, t };
		return x;
	}
	s->dx += (float)(alpha(SPEED_CUTOFF, dt) * ((x - s->x) / dt - s->dx));
	cutoff = MIN_CUTOFF + SPEED_GAIN * fabs(s->dx);
	s->x += (float)(alpha(cutoff, dt) * (x - s->x));
	s->t = t;
	return s->x;
}
