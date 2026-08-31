/*
 * power_ui.c — power statusbar module (icon right of the clock) and its
 * hover popup: Logout / Lock / Reboot / Shutdown as compact icon rows.
 * Logout quits the compositor (ends the session), Lock spawns
 * nixly-lockscreen, Reboot/Shutdown go through systemctl.
 */
#include "nixlytile.h"
#include "popup_card.h"

#define POWER_HIT_BASE 500  /* +0 logout +1 lock +2 reboot +3 shutdown */

char power_icon_path[PATH_MAX] = "images/svg/power.svg";

static struct wlr_buffer *power_icon_buf;
static int power_icon_w, power_icon_h;
static int power_icon_loaded_h;
static char power_icon_loaded_path[PATH_MAX];

void
drop_power_icon_buffer(void)
{
	if (power_icon_buf) {
		wlr_buffer_drop(power_icon_buf);
		power_icon_buf = NULL;
	}
	power_icon_loaded_h = 0;
	power_icon_w = power_icon_h = 0;
	power_icon_loaded_path[0] = '\0';
}

int
ensure_power_icon_buffer(int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GError *gerr = NULL;
	struct wlr_buffer *buf;
	int w = 0, h = 0;
	char resolved[PATH_MAX];
	const char *path = power_icon_path;

	if (target_h <= 0)
		return -1;
	if (resolve_asset_path(power_icon_path, resolved,
				sizeof(resolved)) == 0 && resolved[0])
		path = resolved;
	if (power_icon_buf && power_icon_loaded_h == target_h &&
			strncmp(power_icon_loaded_path, path,
				sizeof(power_icon_loaded_path)) == 0)
		return 0;
	if (tray_load_svg_pixbuf(path, target_h, &pixbuf) != 0) {
		pixbuf = gdk_pixbuf_new_from_file(path, &gerr);
		if (!pixbuf) {
			if (gerr)
				g_error_free(gerr);
			return -1;
		}
	}
	buf = statusbar_buffer_from_pixbuf(pixbuf, target_h, &w, &h);
	if (!buf)
		return -1;
	drop_power_icon_buffer();
	power_icon_buf = buf;
	power_icon_w = w;
	power_icon_h = h;
	power_icon_loaded_h = target_h;
	snprintf(power_icon_loaded_path, sizeof(power_icon_loaded_path),
			"%s", path);
	return 0;
}

void
renderpower(StatusModule *module, int bar_height, const char *text)
{
	(void)text;
	render_tray_icon_module(module, bar_height,
			ensure_power_icon_buffer, &power_icon_buf,
			&power_icon_w, &power_icon_h);
}

/* ── popup ───────────────────────────────────────────────────────── */

static const struct {
	const char *icon;
	const char *label;
} power_rows[4] = {
	{ "images/svg/power_logout.svg",   "Logout" },
	{ "images/svg/power_lock.svg",     "Lock" },
	{ "images/svg/power_reboot.svg",   "Reboot" },
	{ "images/svg/power_shutdown.svg", "Shutdown" },
};

void
render_power_popup(Monitor *m)
{
	InfoPopup *p = &m->statusbar.power_popup;
	Card *card;
	CardResult res;

	if (!p->tree)
		return;
	card = card_begin();
	if (!card)
		return;
	card_min_w(card, 1);   /* hug the rows — no header, no filler */
	for (int i = 0; i < 4; i++)
		card_icon_text(card, power_rows[i].icon, power_rows[i].label,
				i == 3 ? card_col_red : NULL,
				POWER_HIT_BASE + i,
				p->btn_hover == POWER_HIT_BASE + i);

	if (card_finish(card, &res) != 0)
		return;
	memcpy(p->hits, res.hits, sizeof(p->hits));
	p->nhits = res.nhits;
	popup_view_apply(&p->view, p->tree, &res);
	p->width = p->view.w;
	p->height = p->view.h;
}

int
power_popup_handle_click(Monitor *m, int lx, int ly, uint32_t button)
{
	InfoPopup *p = &m->statusbar.power_popup;
	int rel_x, rel_y;

	if (!p->visible || !p->tree || button != BTN_LEFT)
		return 0;

	rel_x = lx - p->tree->node.x;
	rel_y = ly - statusbar_popup_y(m);
	if (rel_x < 0 || rel_y < 0 || rel_x >= p->width || rel_y >= p->height)
		return 0;

	for (int i = 0; i < p->nhits; i++) {
		CardHit *hit = &p->hits[i];

		if (hit->w <= 0 ||
				rel_x < hit->x || rel_x >= hit->x + hit->w ||
				rel_y < hit->y || rel_y >= hit->y + hit->h)
			continue;
		switch (hit->id - POWER_HIT_BASE) {
		case 0:
			quit(&(Arg){0});
			return 1;
		case 1: {
			const char *const argv[] =
				{ "nixly-lockscreen", NULL };

			spawn_cmd_async(argv);
			info_popups_hide(m);
			return 1;
		}
		case 2: {
			const char *const argv[] =
				{ "systemctl", "reboot", NULL };

			spawn_cmd_async(argv);
			return 1;
		}
		case 3: {
			const char *const argv[] =
				{ "systemctl", "poweroff", NULL };

			spawn_cmd_async(argv);
			return 1;
		}
		}
	}
	/* swallow clicks on the card body */
	return 1;
}
