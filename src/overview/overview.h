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

/* Worker thread; blurred wallpaper copy. */
struct nx_buffer *overview_blur(GdkPixbuf *wallpaper);
/* Takes ownership; NULL clears. */
void overview_set_backdrop(struct nx_buffer *blur);

#endif
