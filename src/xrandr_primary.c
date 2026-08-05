#include "nixlytile.h"
#ifdef XWAYLAND
#include <xcb/randr.h>
#endif

/*
 * Keep Xwayland's RandR primary output in sync with the monitor an X11
 * game occupies.  Wine/Proton treat the RandR primary as the "main"
 * display; with no primary set they fall back to whichever output
 * enumerates first, so fullscreen resolution lists in games come from
 * the wrong screen on multi-monitor setups.
 */
void
xwayland_set_primary(Monitor *m)
{
#ifdef XWAYLAND
	xcb_connection_t *xc;
	xcb_screen_t *screen;
	xcb_randr_get_screen_resources_current_reply_t *res;
	xcb_randr_output_t *outputs;
	const char *name;
	int nout, i;

	if (!xwayland || !m || !m->wlr_output)
		return;
	name = m->wlr_output->name;

	xc = wlr_xwayland_get_xwm_connection(xwayland);
	if (!xc)
		return;
	screen = xcb_setup_roots_iterator(xcb_get_setup(xc)).data;
	if (!screen)
		return;

	res = xcb_randr_get_screen_resources_current_reply(xc,
		xcb_randr_get_screen_resources_current(xc, screen->root), NULL);
	if (!res)
		return;

	outputs = xcb_randr_get_screen_resources_current_outputs(res);
	nout = xcb_randr_get_screen_resources_current_outputs_length(res);
	for (i = 0; i < nout; i++) {
		xcb_randr_get_output_info_reply_t *info =
			xcb_randr_get_output_info_reply(xc,
				xcb_randr_get_output_info(xc, outputs[i],
					res->config_timestamp), NULL);
		if (!info)
			continue;
		int len = xcb_randr_get_output_info_name_length(info);
		uint8_t *oname = xcb_randr_get_output_info_name(info);
		if (len == (int)strlen(name) && !memcmp(oname, name, len)) {
			xcb_randr_set_output_primary(xc, screen->root, outputs[i]);
			xcb_flush(xc);
			wlr_log(WLR_INFO, "Xwayland: RandR primary -> %s", name);
			free(info);
			break;
		}
		free(info);
	}
	free(res);
#endif
}
