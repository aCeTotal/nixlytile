#include <math.h>
#include <stdio.h>

#include "tracker.h"

#define SEARCH_SHRINK 4
#define MESH_INTERVAL_S 0.1
#define MIN_PRESENCE 0.5f
#define BLINK_RATIO 0.7f
#define BLINK_SETTLE_S 0.07
#define OPEN_RATE 0.05f
#define MIN_EYE_PX 8.0f
#define STILL_SHIFT 0.08f

static Net *
model(const char *name)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", GAZE_MODEL_DIR, name);
	return net_load(path);
}

int
tracker_open(Tracker *t, int w, int h)
{
	*t = (Tracker){0};
	t->mesh = model("face_landmark.tflite");
	t->iris = model("iris_landmark.tflite");
	if (detector_open(&t->detector, model("face_detection_short_range.tflite")) < 0 ||
	    !t->mesh || !t->iris || decoder_open(&t->dec, w, h) < 0) {
		tracker_close(t);
		return -1;
	}
	return 0;
}

void
tracker_close(Tracker *t)
{
	net_free(t->detector.net);
	net_free(t->mesh);
	net_free(t->iris);
	decoder_close(&t->dec);
	*t = (Tracker){0};
}

static int
search(Tracker *t, const Frame *f)
{
	Region all = { 0, 0, f->w, f->h, SEARCH_SHRINK };
	const Image *img = decode(&t->dec, f, &all);
	Detection d;

	if (!img || !detector_run(&t->detector, img, &d))
		return 0;
	t->face.rect = detection_rect(&d);
	return 1;
}

/* Full-resolution bounds of rects. */
static Region
cover(const Rect *r, int n)
{
	float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;

	for (int i = 0; i < n; i++) {
		float half = (fabsf(cosf(r[i].angle)) + fabsf(sinf(r[i].angle))) * r[i].w / 2;

		x0 = fminf(x0, r[i].cx - half);
		y0 = fminf(y0, r[i].cy - half);
		x1 = fmaxf(x1, r[i].cx + half);
		y1 = fmaxf(y1, r[i].cy + half);
	}
	return (Region){ (int)floorf(x0), (int)floorf(y0), (int)ceilf(x1 - x0) + 1,
		(int)ceilf(y1 - y0) + 1, 1 };
}

/* Moving heads mesh every frame. */
static void
moved(Tracker *t)
{
	Corners was = t->at[0];
	float width;

	mesh_corners(&t->face, t->at);
	width = hypotf(t->at[0].right.x - t->at[0].left.x, t->at[0].right.y - t->at[0].left.y);
	t->moving = hypotf(t->at[0].left.x - was.left.x, t->at[0].left.y - was.left.y) >
		STILL_SHIFT * width;
}

/* Mesh pass; NULL without face. */
static const Image *
meshed(Tracker *t, const Frame *f)
{
	Region roi = cover(&t->face.rect, 1);
	const Image *img = decode(&t->dec, f, &roi);

	if (!img)
		return NULL;
	mesh_run(t->mesh, img, &t->face);
	t->tracking = t->face.presence >= MIN_PRESENCE;
	if (!t->tracking)
		return NULL;
	moved(t);
	t->face.rect = mesh_rect(&t->face);
	t->mesh_t = f->t;
	return img;
}

static const Image *
eye_band(Tracker *t, const Frame *f)
{
	Rect r[EYES] = { eye_rect(&t->at[0]), eye_rect(&t->at[1]) };
	Region roi = cover(r, EYES);

	return decode(&t->dec, f, &roi);
}

static int
plausible(const Eyes *e)
{
	return e->width[0] >= MIN_EYE_PX && e->width[1] >= MIN_EYE_PX;
}

/* Blinks plus reopening settle. */
static int
blinking(Tracker *t, const Features *f)
{
	if (t->open <= 0)
		t->open = f->open;
	if (f->open < BLINK_RATIO * t->open) {
		t->settled = f->t + BLINK_SETTLE_S;
		return 1;
	}
	t->open += OPEN_RATE * (f->open - t->open);
	return f->t < t->settled;
}

Track
tracker_step(Tracker *t, const Frame *f, Features *out)
{
	const Image *img;
	Eyes eyes;

	if (!t->tracking && !search(t, f))
		return TRACK_LOST;
	img = !t->tracking || t->moving || f->t - t->mesh_t >= MESH_INTERVAL_S ?
		meshed(t, f) : eye_band(t, f);
	if (!img)
		return TRACK_LOST;
	eyes = iris_run(t->iris, img, t->at);
	t->tracking = plausible(&eyes);
	if (!t->tracking)
		return TRACK_LOST;
	*out = features_of(&t->face, &eyes, f);
	return blinking(t, out) ? TRACK_BLINK : TRACK_OK;
}
