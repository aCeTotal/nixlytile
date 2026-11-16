#include <math.h>

#include "track.h"

#define INPUT 128
#define ANCHORS 896
#define COORDS 16
#define MIN_SCORE 0.5f
#define MERGE_IOU 0.3f
#define SCORE_CLIP 100.0f
#define ROI_SCALE 1.5f

/* BlazeFace short-range SSD anchors. */
static void
anchors(Point *a)
{
	static const struct { int grid, per; } layers[] = { { 16, 2 }, { 8, 6 } };
	int n = 0;

	for (size_t l = 0; l < sizeof(layers) / sizeof(layers[0]); l++) {
		int g = layers[l].grid;

		for (int i = 0; i < g * g * layers[l].per; i++) {
			int cell = i / layers[l].per;

			a[n++] = (Point){ ((float)(cell % g) + 0.5f) / (float)g,
				((float)(cell / g) + 0.5f) / (float)g };
		}
	}
}

int
detector_open(Detector *d, Net *net)
{
	d->net = net;
	anchors(d->anchor);
	return net ? 0 : -1;
}

static float
score_of(float raw)
{
	raw = raw < -SCORE_CLIP ? -SCORE_CLIP : raw > SCORE_CLIP ? SCORE_CLIP : raw;
	return 1.0f / (1.0f + expf(-raw));
}

/* Anchor box, tensor-normalized. */
static Detection
decode(const Detector *d, int i)
{
	const float *raw = net_output(d->net, 0) + (size_t)i * COORDS;
	Point a = d->anchor[i];
	Detection det = { .score = score_of(net_output(d->net, 1)[i]), .cx = raw[0] / INPUT + a.x,
		.cy = raw[1] / INPUT + a.y, .w = raw[2] / INPUT, .h = raw[3] / INPUT };

	for (int k = 0; k < FACE_KEYPOINTS; k++)
		det.key[k] = (Point){ raw[4 + 2 * k] / INPUT + a.x, raw[5 + 2 * k] / INPUT + a.y };
	return det;
}

static float
iou(const Detection *a, const Detection *b)
{
	float w = fminf(a->cx + a->w / 2, b->cx + b->w / 2) - fmaxf(a->cx - a->w / 2, b->cx - b->w / 2);
	float h = fminf(a->cy + a->h / 2, b->cy + b->h / 2) - fmaxf(a->cy - a->h / 2, b->cy - b->h / 2);
	float inter = w > 0 && h > 0 ? w * h : 0.0f;

	return inter / (a->w * a->h + b->w * b->h - inter);
}

static void
accumulate(Detection *sum, const Detection *d)
{
	sum->score += d->score;
	sum->cx += d->cx * d->score;
	sum->cy += d->cy * d->score;
	sum->w += d->w * d->score;
	sum->h += d->h * d->score;
	for (int k = 0; k < FACE_KEYPOINTS; k++) {
		sum->key[k].x += d->key[k].x * d->score;
		sum->key[k].y += d->key[k].y * d->score;
	}
}

/* Score-weighted overlapping anchor mean. */
static Detection
merged(const Detector *d, int best)
{
	Detection top = decode(d, best), sum = {0};

	for (int i = 0; i < ANCHORS; i++) {
		Detection c = decode(d, i);

		if (c.score >= MIN_SCORE && iou(&top, &c) > MERGE_IOU)
			accumulate(&sum, &c);
	}
	return sum;
}

/* Letterboxed point to frame. */
static Point
unbox(const Image *frame, Point p)
{
	float fw = (float)frame->w * frame->scale, fh = (float)frame->h * frame->scale;
	float side = fmaxf(fw, fh);

	return (Point){ frame->ox + fw / 2 + (p.x - 0.5f) * side, frame->oy + fh / 2 + (p.y - 0.5f) * side };
}

static Detection
to_frame(const Image *frame, const Detection *sum)
{
	float side = fmaxf((float)frame->w, (float)frame->h) * frame->scale;
	Point c = unbox(frame, (Point){ sum->cx / sum->score, sum->cy / sum->score });
	Detection out = { .cx = c.x, .cy = c.y,
		.w = sum->w / sum->score * side, .h = sum->h / sum->score * side };

	for (int k = 0; k < FACE_KEYPOINTS; k++)
		out.key[k] = unbox(frame, (Point){ sum->key[k].x / sum->score,
			sum->key[k].y / sum->score });
	return out;
}

/* Best face, 0 when none. */
int
detector_run(Detector *d, const Image *frame, Detection *out)
{
	float fw = (float)frame->w * frame->scale, fh = (float)frame->h * frame->scale;
	Rect box = { frame->ox + fw / 2, frame->oy + fh / 2, fmaxf(fw, fh), fmaxf(fw, fh), 0.0f };
	Crop crop = { net_input(d->net, 0), INPUT, -1.0f, 1.0f, 0 };
	const float *scores;
	int best = 0;

	sample_rect(frame, &box, &crop);
	net_run(d->net);
	scores = net_output(d->net, 1);
	for (int i = 1; i < ANCHORS; i++)
		best = scores[i] > scores[best] ? i : best;
	if (score_of(scores[best]) < MIN_SCORE)
		return 0;
	*out = merged(d, best);
	*out = to_frame(frame, out);
	return 1;
}

/* Level square mesh crop. */
Rect
detection_rect(const Detection *d)
{
	float side = fmaxf(d->w, d->h) * ROI_SCALE;

	return (Rect){ d->cx, d->cy, side, side,
		atan2f(d->key[1].y - d->key[0].y, d->key[1].x - d->key[0].x) };
}
