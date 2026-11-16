#include <math.h>

#include "track.h"

#define INPUT 64
#define ROI_SCALE 2.3f
#define OUTER_CORNER 0
#define LOWER_LID 4
#define INNER_CORNER 8
#define UPPER_LID 12

/* Model expects image-left eyes. */
static const int mirrored[EYES] = { 0, 1 };

typedef struct {
	Net *net;
	const Image *img;
	const Corners *at;
} Job;

Rect
eye_rect(const Corners *c)
{
	float side = fmaxf(fabsf(c->right.x - c->left.x), fabsf(c->right.y - c->left.y)) * ROI_SCALE;

	return (Rect){ (c->left.x + c->right.x) / 2, (c->left.y + c->right.y) / 2, side, side,
		atan2f(c->right.y - c->left.y, c->right.x - c->left.x) };
}

static float
gap(Point a, Point b)
{
	return hypotf(b.x - a.x, b.y - a.y);
}

static Point
landmark(const float *v, int i)
{
	return (Point){ v[3 * i], v[3 * i + 1] };
}

static void
run_eye(const Job *job, int e, Eyes *out)
{
	Rect r = eye_rect(&job->at[e]);
	Crop crop = { net_input(job->net, 0), INPUT, 0.0f, 1.0f, mirrored[e] };
	const float *contour, *iris;
	Point outer, inner;

	sample_rect(job->img, &r, &crop);
	net_run(job->net);
	contour = net_output(job->net, 0);
	iris = net_output(job->net, 1);
	outer = rect_point(&r, &crop, landmark(contour, OUTER_CORNER));
	inner = rect_point(&r, &crop, landmark(contour, INNER_CORNER));
	out->iris[e] = rect_point(&r, &crop, landmark(iris, 0));
	out->mid[e] = (Point){ (outer.x + inner.x) / 2, (outer.y + inner.y) / 2 };
	out->width[e] = gap(outer, inner);
	out->openness[e] = gap(landmark(contour, UPPER_LID), landmark(contour, LOWER_LID)) /
		gap(landmark(contour, OUTER_CORNER), landmark(contour, INNER_CORNER));
}

/* Eyes cropped at corners. */
Eyes
iris_run(Net *net, const Image *img, const Corners *at)
{
	Job job = { net, img, at };
	Eyes out;

	for (int e = 0; e < EYES; e++)
		run_eye(&job, e, &out);
	return out;
}
