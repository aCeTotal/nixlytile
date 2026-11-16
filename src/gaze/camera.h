#ifndef GAZE_CAMERA_H
#define GAZE_CAMERA_H

#include "track.h"

#define CAMERA_BUFFERS 4

typedef struct {
	int fd;
	int w, h, jpeg, fps;
	int priority;           /* priority to restore, -1 none */
	void *map[CAMERA_BUFFERS];
	size_t maplen[CAMERA_BUFFERS];
	int nbuf;
	int held;               /* dequeued buffer, -1 if none */
	char path[32];
} Camera;

int camera_open(Camera *cam);
void camera_close(Camera *cam);
typedef enum { CAMERA_GONE, CAMERA_WAIT, CAMERA_FRAME } CameraStatus;

CameraStatus camera_next(Camera *cam, Frame *f, int timeout_ms);

#endif
