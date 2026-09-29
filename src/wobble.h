#ifndef WOBBLE_H
#define WOBBLE_H

#include <wlr/render/swapchain.h>

#define WOBBLE_EDGES 24 /* spring control edges */

struct Wobble {
	struct wlr_scene_buffer *view;   /* deformed window, one buffer */
	struct wlr_scene_buffer *real;   /* client buffer, kept for input */
	struct wlr_swapchain *swapchain;
	struct wl_listener commit;
	double off[2][WOBBLE_EDGES + 1]; /* x and y edge offsets */
	double vel[2][WOBBLE_EDGES + 1];
	double cap;
	int grab;                        /* edge pinned under the cursor */
	int held;
	int concealed;                   /* real surface hidden */
	uint32_t seq;                    /* commit the view shows */
	int last_x, last_y;
	int border_enabled[4];
};

int wobble_render(Client *c, Wobble *w);

#endif
