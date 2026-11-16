#ifndef GAZE_MODEL_H
#define GAZE_MODEL_H

#include "track.h"

enum { FEAT_U, FEAT_YAW, FEAT_X, FEATURES };

/* Quadratic gaze angle, linear head. */
enum { TERM_BIAS, TERM_GX, TERM_GXX, TERM_YAW, TERM_X, TERMS };

typedef struct {
	float v[FEATURES];
	float open;
	double t;               /* capture time, seconds */
} Features;

/* Horizontal only: webcams miss vertical. */
typedef struct {
	float mean[FEATURES];
	float beta[TERMS];
	float error;            /* 90th percentile, output widths */
} GazeModel;

/* Features while viewing target. */
typedef struct {
	Features f;
	float target;           /* fraction of output width */
	int point;
} Sample;

/* One Euro filter, one axis. */
typedef struct {
	float x, dx;
	double t;
} Smooth;

Features features_of(const Face *face, const Eyes *eyes, const Frame *f);
float smooth(Smooth *s, float x, double t);
float model_predict(const GazeModel *m, const Features *f);
int model_fit(GazeModel *m, const Sample *s, int n);

#endif
