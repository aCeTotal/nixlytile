#ifndef NIXLYTILE_REMOTE_H
#define NIXLYTILE_REMOTE_H

#include <wayland-server-core.h>
#include <wlr/backend/headless.h>

/* Parked geometry for idle virtual outputs, placed far off the physical layout */
#define REMOTE_PARK_W    1280
#define REMOTE_PARK_H    720
#define REMOTE_PARK_HZ   60
#define REMOTE_PARK_X    20000
#define REMOTE_PARK_STEP 4096

#define REMOTE_MAX_OUTPUTS 4

/* Config: number of parked virtual outputs, pointer speed in px/s */
extern int remote_outputs;
extern int remote_mouse_speed;

typedef struct {
	int width, height, fps;
	int hdr;
	double scale;
	int output;   /* 1-based virtual output */
	int outputs;  /* total participating outputs */
} RemoteParams;

/* Create the headless backend and its parked outputs.  Call after
 * wlr_backend_autocreate, before the backend is started. */
void remote_backend_init(struct wl_display *display, int n_outputs);

/* Returns 0 on success, or a static error string. */
const char *remote_start(const RemoteParams *p);
const char *remote_stop(int output);

int remote_is_active(void);

/* remote_pad.c */
void remote_pad_init(void);
void remote_pad_cleanup(void);

/* remote_mouse.c — axis values are normalized to [-1, 1] */
void remote_mouse_axis(int code, double value);
void remote_mouse_click(int right, int pressed);
void remote_mouse_reset(void);

/* Virtual output idling off-layout: never focusable, never auto-arranged */
#define REMOTE_PARKED(m) ((m)->is_virtual && !remote_is_active())

#endif
