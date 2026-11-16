#include <math.h>

#include "track.h"

#define INPUT 192
#define ROI_SCALE 1.5f

enum { RIGHT_EYE_OUTER = 33, RIGHT_EYE_INNER = 133, LEFT_EYE_INNER = 362, LEFT_EYE_OUTER = 263 };

void
mesh_run(Net *net, const Image *img, Face *face)
{
	Crop crop = { net_input(net, 0), INPUT, 0.0f, 1.0f, 0 };
	const float *lm;

	sample_rect(img, &face->rect, &crop);
	net_run(net);
	lm = net_output(net, 0);
	for (int i = 0; i < MESH_POINTS; i++)
		face->pts[i] = rect_point(&face->rect, &crop, (Point){ lm[3 * i], lm[3 * i + 1] });
	face->presence = 1.0f / (1.0f + expf(-net_output(net, 1)[0]));
}

/* Next crop from landmark bounds. */
Rect
mesh_rect(const Face *face)
{
	Point lo = face->pts[0], hi = face->pts[0];
	const Point *a = &face->pts[RIGHT_EYE_OUTER], *b = &face->pts[LEFT_EYE_OUTER];

	for (int i = 1; i < MESH_POINTS; i++) {
		lo = (Point){ fminf(lo.x, face->pts[i].x), fminf(lo.y, face->pts[i].y) };
		hi = (Point){ fmaxf(hi.x, face->pts[i].x), fmaxf(hi.y, face->pts[i].y) };
	}
	return (Rect){ (lo.x + hi.x) / 2, (lo.y + hi.y) / 2,
		fmaxf(hi.x - lo.x, hi.y - lo.y) * ROI_SCALE, fmaxf(hi.x - lo.x, hi.y - lo.y) * ROI_SCALE,
		atan2f(b->y - a->y, b->x - a->x) };
}

void
mesh_corners(const Face *face, Corners *out)
{
	out[0] = (Corners){ face->pts[RIGHT_EYE_OUTER], face->pts[RIGHT_EYE_INNER] };
	out[1] = (Corners){ face->pts[LEFT_EYE_INNER], face->pts[LEFT_EYE_OUTER] };
}
