#ifndef GAZE_TRACKER_H
#define GAZE_TRACKER_H

#include "decode.h"
#include "gaze_model.h"

typedef struct {
	Detector detector;
	Net *mesh, *iris;
	Decoder dec;
	Face face;
	Corners at[EYES];       /* eye crops, fixed between meshes */
	int tracking;
	double mesh_t;          /* last mesh capture time */
	int moving;
	float open;             /* typical eye openness */
	double settled;         /* blink over at this time */
} Tracker;

typedef enum { TRACK_LOST, TRACK_BLINK, TRACK_OK } Track;

int tracker_open(Tracker *t, int w, int h);
void tracker_close(Tracker *t);
Track tracker_step(Tracker *t, const Frame *f, Features *out);

#endif
