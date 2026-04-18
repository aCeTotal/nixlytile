#ifndef MPV_LAUNCHER_H
#define MPV_LAUNCHER_H

/* External mpv subprocess launcher for HTPC video playback.
 *
 * Spawns `mpv` as a Wayland client with full OSC, high-perf decode,
 * IPC socket for gamepad control, and pidfd-based exit watching.
 * On exit, saves last observed time-pos via save_resume_position(). */

int  mpv_launcher_start(const char *url, double resume_pos, int media_id);
void mpv_launcher_stop(void);          /* graceful: send quit, wait for exit */
int  mpv_launcher_active(void);        /* 1 if mpv is currently running */

/* Send raw JSON command (one line, no newline needed — we add it).
 * Example: {"command":["cycle","pause"]} */
int  mpv_launcher_send_cmd(const char *json);

/* High-level helpers (wrap send_cmd) */
void mpv_launcher_toggle_pause(void);
void mpv_launcher_seek_relative(double seconds);
void mpv_launcher_cycle_audio(void);
void mpv_launcher_cycle_sub(void);
void mpv_launcher_volume_delta(double delta);

#endif
