#ifndef GAZE_PROFILE_H
#define GAZE_PROFILE_H

#include "gaze_model.h"

typedef struct {
	char output[64];
	GazeModel model;
} Profile;

int profile_load(Profile *p);
int profile_save(const Profile *p, const Sample *s, int n);

#endif
