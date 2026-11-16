#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gaze_model.h"

#define ERROR_QUANTILE 0.9
#define MIN_POINTS 5

/* Physical gaze angle priors. */
#define EYE_GAIN 0.40      /* iris offset per radian */
#define YAW_GAIN 0.67
#define TRAVEL_X 0.70      /* radians per half frame */
#define ANGLE_SPREAD 0.3

/* Fixed head residual spread. */
static const double spread[FEATURES] = { [FEAT_YAW] = 0.2, [FEAT_X] = 0.3 };

/* Per-term ridge, loose on priors. */
static const double ridge[TERMS] = {
	[TERM_GX] = 1e-4, [TERM_GXX] = 0.02, [TERM_YAW] = 0.2, [TERM_X] = 0.2,
};

typedef struct {
	double term[TERMS];
	double time;
	float target;
	int point;
} Row;

/* Ridge normal equations. */
typedef struct {
	double a[TERMS][TERMS];
	double b[TERMS];
} Normal;

/* Rows, fit and misses. */
typedef struct {
	Row *rows;
	double *err;
	int n;
	Normal all;
} Fit;

static void
terms(const GazeModel *m, const Features *f, double *t)
{
	double d[FEATURES], gx;

	for (int i = 0; i < FEATURES; i++)
		d[i] = f->v[i] - m->mean[i];
	gx = (d[FEAT_U] / EYE_GAIN + d[FEAT_YAW] / YAW_GAIN + d[FEAT_X] * TRAVEL_X) / ANGLE_SPREAD;
	t[TERM_BIAS] = 1;
	t[TERM_GX] = gx;
	t[TERM_GXX] = gx * gx;
	t[TERM_YAW] = d[FEAT_YAW] / spread[FEAT_YAW];
	t[TERM_X] = d[FEAT_X] / spread[FEAT_X];
}

static void
accumulate(Normal *n, const Row *r, double sign)
{
	for (int i = 0; i < TERMS; i++) {
		for (int j = 0; j < TERMS; j++)
			n->a[i][j] += sign * r->term[i] * r->term[j];
		n->b[i] += sign * r->term[i] * r->target;
	}
}

/* Partial row dot product. */
static double
row_dot(double l[TERMS][TERMS], int i, int j)
{
	double s = 0;

	for (int k = 0; k < j; k++)
		s += l[i][k] * l[j][k];
	return s;
}

static int
cholesky(double a[TERMS][TERMS])
{
	for (int j = 0; j < TERMS; j++) {
		double d = a[j][j] - row_dot(a, j, j);

		if (d <= 0)
			return -1;
		a[j][j] = sqrt(d);
		for (int i = j + 1; i < TERMS; i++)
			a[i][j] = (a[i][j] - row_dot(a, i, j)) / a[j][j];
	}
	return 0;
}

static void
substitute(double l[TERMS][TERMS], const double *b, double *x)
{
	double y[TERMS];

	for (int i = 0; i < TERMS; i++) {
		y[i] = b[i];
		for (int k = 0; k < i; k++)
			y[i] -= l[i][k] * y[k];
		y[i] /= l[i][i];
	}
	for (int i = TERMS - 1; i >= 0; i--) {
		x[i] = y[i];
		for (int k = i + 1; k < TERMS; k++)
			x[i] -= l[k][i] * x[k];
		x[i] /= l[i][i];
	}
}

static int
solve(const Normal *n, double *beta)
{
	double l[TERMS][TERMS];

	memcpy(l, n->a, sizeof(l));
	for (int i = 0; i < TERMS; i++)
		l[i][i] += ridge[i] * n->a[TERM_BIAS][TERM_BIAS];
	if (cholesky(l) < 0)
		return -1;
	substitute(l, n->b, beta);
	return 0;
}

static double
predict(const double *beta, const double *t)
{
	double x = 0;

	for (int i = 0; i < TERMS; i++)
		x += beta[i] * t[i];
	return x;
}

/* Smoothed misses, output widths. */
static void
smoothed_misses(Fit *fit, const double *beta, int first)
{
	Smooth s = {0};
	const Row *r = &fit->rows[first];

	for (int k = first; k < fit->n && fit->rows[k].point == r->point; k++)
		fit->err[k] = fabs(smooth(&s, (float)predict(beta, fit->rows[k].term), fit->rows[k].time) -
			r->target);
}

/* Point p held out. */
static int
hold_out(Fit *fit, int p)
{
	Normal without = fit->all;
	double beta[TERMS];
	int first = 0, k;

	while (first < fit->n && fit->rows[first].point != p)
		first++;
	for (k = first; k < fit->n && fit->rows[k].point == p; k++)
		accumulate(&without, &fit->rows[k], -1);
	if (first == fit->n || solve(&without, beta) < 0)
		return 0;
	smoothed_misses(fit, beta, first);
	return 1;
}

static void
normalise(GazeModel *m, const Sample *s, int n)
{
	for (int i = 0; i < FEATURES; i++) {
		double sum = 0;

		for (int k = 0; k < n; k++)
			sum += s[k].f.v[i];
		m->mean[i] = (float)(sum / n);
	}
}

static int
cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

/* Missing hold-outs count fully. */
static void
rows_of(Fit *fit, const GazeModel *m, const Sample *s)
{
	for (int k = 0; k < fit->n; k++) {
		terms(m, &s[k].f, fit->rows[k].term);
		fit->rows[k].time = s[k].f.t;
		fit->rows[k].target = s[k].target;
		fit->rows[k].point = s[k].point;
		fit->err[k] = HUGE_VAL;
		accumulate(&fit->all, &fit->rows[k], 1);
	}
}

static int
conclude(GazeModel *m, Fit *fit)
{
	double beta[TERMS];
	int points = 0;

	for (int p = 0; p <= fit->rows[fit->n - 1].point; p++)
		points += hold_out(fit, p);
	if (points < MIN_POINTS || solve(&fit->all, beta) < 0)
		return -1;
	qsort(fit->err, (size_t)fit->n, sizeof(*fit->err), cmp_double);
	m->error = (float)fit->err[(int)(ERROR_QUANTILE * (fit->n - 1))];
	for (int i = 0; i < TERMS; i++)
		m->beta[i] = (float)beta[i];
	return 0;
}

/* Samples grouped by point. */
int
model_fit(GazeModel *m, const Sample *s, int n)
{
	Fit fit = { calloc((size_t)n, sizeof(Row)), calloc((size_t)n, sizeof(double)), n,
		{ { { 0 } }, { 0 } } };
	int ret = -1;

	if (n > 0 && fit.rows && fit.err) {
		normalise(m, s, n);
		rows_of(&fit, m, s);
		ret = conclude(m, &fit);
	}
	free(fit.rows);
	free(fit.err);
	return ret;
}

float
model_predict(const GazeModel *m, const Features *f)
{
	double t[TERMS], beta[TERMS];

	terms(m, f, t);
	for (int i = 0; i < TERMS; i++)
		beta[i] = m->beta[i];
	return (float)predict(beta, t);
}
