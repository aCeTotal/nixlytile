#include <math.h>

#include "gaze_model.h"

enum { NOSE_TIP = 1, RIGHT_EYE_OUTER = 33, RIGHT_CHEEK = 234, LEFT_EYE_OUTER = 263, LEFT_CHEEK = 454 };

static Point
sub(Point a, Point b)
{
	return (Point){ a.x - b.x, a.y - b.y };
}

static float
dot(Point a, Point b)
{
	return a.x * b.x + a.y * b.y;
}

/* Unit eye-to-eye axis. */
static Point
head_axis(const Face *face)
{
	Point x = sub(face->pts[LEFT_EYE_OUTER], face->pts[RIGHT_EYE_OUTER]);
	float n = hypotf(x.x, x.y);

	return (Point){ x.x / n, x.y / n };
}

/* Iris offset in eye widths. */
static float
iris_offset(const Eyes *eyes, Point axis)
{
	float sum = 0;

	for (int e = 0; e < EYES; e++)
		sum += dot(sub(eyes->iris[e], eyes->mid[e]), axis) / (eyes->width[e] * EYES);
	return sum;
}

/* Nose between the cheeks, ±0.5. */
static float
yaw(const Point *p, Point axis)
{
	Point centre = { (p[RIGHT_CHEEK].x + p[LEFT_CHEEK].x) / 2, (p[RIGHT_CHEEK].y + p[LEFT_CHEEK].y) / 2 };

	return dot(sub(p[NOSE_TIP], centre), axis) / fabsf(dot(sub(p[LEFT_CHEEK], p[RIGHT_CHEEK]), axis));
}

/* Mesh may lag two frames. */
Features
features_of(const Face *face, const Eyes *eyes, const Frame *f)
{
	const Point *p = face->pts;
	Point axis = head_axis(face);

	return (Features){
		.v = {
			[FEAT_U] = iris_offset(eyes, axis),
			[FEAT_YAW] = yaw(p, axis),
			[FEAT_X] = (p[RIGHT_EYE_OUTER].x + p[LEFT_EYE_OUTER].x) / (float)f->w - 1,
		},
		.open = (eyes->openness[0] + eyes->openness[1]) / 2,
		.t = f->t,
	};
}
