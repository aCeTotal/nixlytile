#ifndef GAZE_TRACK_H
#define GAZE_TRACK_H

#include <stddef.h>
#include <stdint.h>

#include "tflite/tflite.h"

#define FACE_KEYPOINTS 6
#define MESH_POINTS 468
#define IRIS_POINTS 5
#define EYES 2

typedef struct {
	float x, y;
} Point;

/* Rotated rectangle in frame pixels. */
typedef struct {
	float cx, cy, w, h, angle;
} Rect;

/* RGB patch of the frame. */
typedef struct {
	uint8_t *rgb;
	int w, h;
	float ox, oy, scale;
} Image;

/* Square model input tensor. */
typedef struct {
	float *dst;
	int size;
	float lo, hi;
	int flip;
} Crop;

/* Captured MJPEG or YUYV frame. */
typedef struct {
	const uint8_t *data;
	size_t len;
	int w, h;
	int jpeg;
	double t;               /* capture time, seconds */
} Frame;

/* Frame region, divided by shrink. */
typedef struct {
	int x, y, w, h, shrink;
} Region;

typedef struct {
	float score;
	float cx, cy, w, h;
	Point key[FACE_KEYPOINTS];
} Detection;

typedef struct {
	Net *net;
	Point anchor[896];
} Detector;

/* Mesh crop and landmarks. */
typedef struct {
	Rect rect;
	Point pts[MESH_POINTS];
	float presence;
} Face;

/* Eye corners, image left/right. */
typedef struct {
	Point left, right;
} Corners;

/* Iris and eye corner midpoint. */
typedef struct {
	Point iris[EYES], mid[EYES];
	float width[EYES], openness[EYES];
} Eyes;

void sample_rect(const Image *img, const Rect *r, const Crop *c);
Point rect_point(const Rect *r, const Crop *c, Point model);

int detector_open(Detector *d, Net *net);
int detector_run(Detector *d, const Image *frame, Detection *out);

void mesh_run(Net *net, const Image *img, Face *face);
Rect mesh_rect(const Face *face);
void mesh_corners(const Face *face, Corners *out);
Rect detection_rect(const Detection *d);

Rect eye_rect(const Corners *c);
Eyes iris_run(Net *net, const Image *img, const Corners *at);

#endif
