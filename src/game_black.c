/* Black-screen probe for nixly-game-wrap: on IPC request, sample the
 * running game's surface texture on a sparse grid and report whether it
 * is showing real content or a stuck black frame.  The wrapper polls
 * this after launch and escalates its remedy ladder (drop Vulkan
 * layers, ntsync off, esync/fsync off, wined3d) until the game renders.
 *
 * Cost control: nothing runs per-frame; each request reads back a grid
 * of tiny boxes (~16 × 1 KB), only while a wrapper is probing.
 */
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>

#include "nixlytile.h"
#include "client.h"
#include "game_black.h"

#define GB_GRID 4          /* GB_GRID × GB_GRID sample boxes */
#define GB_BOX  16         /* box edge in buffer pixels */
#define GB_LUMA 12         /* any R/G/B byte above this = content */

/* The client the wrapper is asking about: the engaged game-mode client,
 * else the fullscreen game, else any mapped client the game classifier
 * accepts (windowed game still loading). */
static Client *
game_black_client(void)
{
	Client *c;

	if (game_mode_client)
		return game_mode_client;
	c = get_fullscreen_client();
	if (c && !is_steam_client(c) && !is_steam_popup(c) && looks_like_game(c))
		return c;
	wl_list_for_each(c, &clients, link) {
		if (!client_surface(c))
			continue;
		if (is_steam_client(c) || is_steam_popup(c))
			continue;
		if (looks_like_game(c))
			return c;
	}
	return NULL;
}

static int
box_has_content(struct wlr_texture *tex, uint32_t fmt, int x0, int y0,
		int w, int h, unsigned char *px)
{
	int i, n;

	if (!wlr_texture_read_pixels(tex,
			&(struct wlr_texture_read_pixels_options){
				.data = px,
				.format = fmt,
				.stride = (uint32_t)w * 4,
				.src_box = { x0, y0, w, h },
			}))
		return 0;
	/* All four accepted formats keep A/X in byte 3; bytes 0-2 are the
	 * color channels regardless of RGB/BGR order. */
	n = w * h;
	for (i = 0; i < n; i++) {
		if (px[i * 4] > GB_LUMA || px[i * 4 + 1] > GB_LUMA ||
				px[i * 4 + 2] > GB_LUMA)
			return 1;
	}
	return 0;
}

int
game_black_state(const char **out_appid)
{
	Client *c = game_black_client();
	struct wlr_surface *surface;
	struct wlr_client_buffer *cb;
	struct wlr_texture *tex;
	unsigned char *px;
	uint32_t fmt;
	int gx, gy, content = 0;

	*out_appid = NULL;
	if (!c)
		return GB_NONE;
	*out_appid = client_get_appid(c);
	surface = client_surface(c);
	if (!surface || !surface->buffer || !surface->buffer->texture)
		return GB_NOBUFFER;
	cb = surface->buffer;
	tex = cb->texture;
	if ((int)tex->width < GB_BOX || (int)tex->height < GB_BOX)
		return GB_NOBUFFER;

	fmt = wlr_texture_preferred_read_format(tex);
	if (fmt != DRM_FORMAT_XRGB8888 && fmt != DRM_FORMAT_ARGB8888 &&
			fmt != DRM_FORMAT_XBGR8888 && fmt != DRM_FORMAT_ABGR8888)
		fmt = DRM_FORMAT_ARGB8888;
	px = malloc((size_t)GB_BOX * GB_BOX * 4);
	if (!px)
		return GB_NOBUFFER;

	for (gy = 0; gy < GB_GRID && !content; gy++) {
		for (gx = 0; gx < GB_GRID && !content; gx++) {
			int x0 = (int)((tex->width - GB_BOX) *
					(gx + 0.5) / GB_GRID);
			int y0 = (int)((tex->height - GB_BOX) *
					(gy + 0.5) / GB_GRID);
			content = box_has_content(tex, fmt, x0, y0,
					GB_BOX, GB_BOX, px);
		}
	}
	free(px);
	return content ? GB_CONTENT : GB_BLACK;
}
