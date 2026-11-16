#ifndef GAZE_MAILBOX_H
#define GAZE_MAILBOX_H

#include "gaze_model.h"

#define GAZE_UNIT 65535.0f

/* Published estimate, output width fraction. */
typedef struct {
	float x;
	int valid;
} GazeSample;

void mailbox_demand(void);
void mailbox_calibrate(int on);
void mailbox_model(const GazeModel *m);
GazeSample mailbox_sample(void);
int mailbox_features(Features *f);
int mailbox_fd(void);
void mailbox_drain(void);

int mailbox_open(void);
int mailbox_stop(void);
void mailbox_close(void);
int mailbox_active(void);
int mailbox_wanted(void);
int mailbox_await(void);
void mailbox_pause(long ms);
unsigned mailbox_model_gen(void);
int mailbox_copy_model(GazeModel *m);
void mailbox_hand_over(const Features *f, int ok);
int mailbox_calibrating(void);
void mailbox_publish(float x);
void mailbox_clear(void);

#endif
