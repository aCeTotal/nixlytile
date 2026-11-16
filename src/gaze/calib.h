#ifndef GAZE_CALIB_H
#define GAZE_CALIB_H

#include "nixlytile.h"
#include "profile.h"

typedef struct {
	struct nx_scene_tree *tree;
	struct nx_scene_buffer *dot, *text;
	struct PixmanBuffer *canvas[2];
	int flip;
	float scale;
} CalibUi;

/* Target marker with shrinking ring. */
typedef struct {
	float x, y, ring;
} Marker;

void calib_start(Monitor *m);
void calib_stop(const char *msg);
int calib_active(void);
void calib_feed(void);

void calib_ui_show(CalibUi *ui, Monitor *m, const char *title);
void calib_ui_marker(CalibUi *ui, Monitor *m, const Marker *mk);
void calib_ui_hide(CalibUi *ui);

/* gaze.c adopts a fresh calibration. */
void gaze_calibrated(const Profile *p, const Sample *s, int n);

#endif
