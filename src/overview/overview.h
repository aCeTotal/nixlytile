/* Super+O zoomed-out workspace view. */
#ifndef OVERVIEW_H
#define OVERVIEW_H

/* Include after nixlytile.h. */

void overview_toggle(const Arg *arg);
int overview_is_open(void);

/* Unmodified keys while open. */
int overview_handle_key(uint32_t mods, xkb_keysym_t sym);

/* Bound action, aimed at selection. */
int overview_bind(const Key *k);

void overview_tick(Monitor *m, double dt, int *still);
void overview_purge_mon(Monitor *m);
void overview_purge_client(Client *c);

void overview_pointer_motion(double x, double y);
/* 1 when consumed. */
int overview_pointer_button(uint32_t button, int pressed);
/* Top-left screen corner toggles. */
void overview_corner(Monitor *m, double x, double y);

/* Worker thread; blurred wallpaper copy. */
struct nx_buffer *overview_blur(GdkPixbuf *wallpaper);
/* Takes ownership; NULL clears. */
void overview_set_backdrop(struct nx_buffer *blur);

#endif
