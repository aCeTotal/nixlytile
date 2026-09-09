/*
 * overview.h — zoomed-out view of every tile on every workspace.
 *
 * Super+O opens it: one row per non-empty workspace, top row is
 * workspace 1, and every tile on that workspace is shown at once, scaled
 * to fit.  Each tile carries a header with its title, app id, the shell's
 * working directory when it is a terminal, and the remote host when that
 * shell is sitting in ssh.  Arrows pick a tile across all rows, Enter
 * zooms into it, Escape returns to where you were.
 *
 * The thumbnails are wlr_scene_surface mirrors of the live client
 * surfaces (the same trick instruments.c uses for cockpit displays) — no
 * capture, no copy, and clients parked on an inactive workspace still
 * show their last frame.  The zoom is a single spring driving every
 * thumbnail from its real on-screen rectangle to its slot in the grid, so
 * opening and closing are the same animation run in opposite directions.
 */
#ifndef OVERVIEW_H
#define OVERVIEW_H

/* Include after nixlytile.h — Arg, Monitor and Client come from there. */

/* Bind action ("toggle-overview"). */
void overview_toggle(const Arg *arg);

/* True while the overview owns the screen and the keyboard. */
int overview_is_open(void);

/* Key handling while open.  Returns 1 when the key was consumed.  `mods`
 * is the live modifier mask: anything held with Super/Ctrl/Alt is left
 * for the normal bind table, so the chord that opened the overview still
 * closes it. */
int overview_handle_key(uint32_t mods, xkb_keysym_t sym);

/* Spring tick, called from monitor_anim_tick like notify_tick/osd_tick. */
void overview_tick(Monitor *m, double dt, int *still);

/* A monitor or client is going away. */
void overview_purge_mon(Monitor *m);
void overview_purge_client(Client *c);

#endif /* OVERVIEW_H */
