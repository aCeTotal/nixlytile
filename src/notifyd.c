/*
 * notifyd.c — in-compositor org.freedesktop.Notifications daemon.
 *
 * Desktop notifications (notify-send, bluetooth connects, wireplumber,
 * browsers) normally land in an external daemon (dunst/mako) that draws
 * its own stock-looking box with no animation.  Here the compositor owns
 * the D-Bus name instead and renders every notification as a popup-card
 * (same chrome as the statusbar popups) that slides in from the right
 * edge, holds, and slides back out — the same lane and feel as osd.c
 * toasts and the adopted client notifications in notify.c.
 *
 * Bus plumbing follows tray.c (sd-bus on the wl_event_loop).  The name
 * is requested with REPLACE_EXISTING + QUEUE, so a running dunst is
 * replaced when it allows it, or taken over the moment it exits.
 */
#include "nixlytile.h"
#include "client.h"
#include "popup_card.h"

#define ND_MARGIN        16
#define ND_GAP           10
#define ND_HOLD_MS       5000
#define ND_MAX_HOLD_MS   30000
#define ND_WRAP_PX       340
#define ND_MAX_LINES     4

/* Same critically-damped feel as osd.c / notify.c. */
static const SpringParams SPRING_ND = { 1.0, 1.0, 800.0 };

typedef struct NdToast {
	struct wl_list link;
	uint32_t id;
	Monitor *m;
	struct wlr_scene_tree *tree;
	int w, h;
	int slot_y;
	int target_x, off_x;
	double x_f, x_vel;
	int hiding;
	uint32_t close_reason;   /* 1 expired, 3 CloseNotification */
	struct wl_event_source *timer;
} NdToast;

static struct wl_list nd_toasts = { &nd_toasts, &nd_toasts };
static sd_bus *notifyd_bus;
static sd_bus_slot *notifyd_vtable_slot;
static struct wl_event_source *notifyd_event;
static uint32_t notifyd_next_id = 1;

static void
nd_schedule(Monitor *m)
{
	if (m && m->wlr_output)
		wlr_output_schedule_frame(m->wlr_output);
}

static void
nd_toast_destroy(NdToast *t)
{
	if (notifyd_bus)
		sd_bus_emit_signal(notifyd_bus, "/org/freedesktop/Notifications",
				"org.freedesktop.Notifications",
				"NotificationClosed", "uu", t->id,
				t->close_reason);
	if (t->timer)
		wl_event_source_remove(t->timer);
	if (t->tree)
		wlr_scene_node_destroy(&t->tree->node);
	wl_list_remove(&t->link);
	free(t);
}

static int
nd_hide_timeout(void *data)
{
	NdToast *t = data;

	t->hiding = 1;
	t->target_x = t->off_x;
	nd_schedule(t->m);
	return 0;
}

/* Ledig y i margen — samme stable-nedover som notify.c, men over denne
 * banens egne kort. */
static int
nd_free_slot_y(Monitor *m, int h)
{
	int y = m->w.y + ND_MARGIN;
	int again = 1;

	while (again) {
		NdToast *o;
		again = 0;
		wl_list_for_each(o, &nd_toasts, link) {
			if (o->m != m || o->hiding)
				continue;
			if (y < o->slot_y + o->h + ND_GAP
					&& o->slot_y < y + h + ND_GAP) {
				y = o->slot_y + o->h + ND_GAP;
				again = 1;
				break;
			}
		}
	}
	return y;
}

/* Klipp mot egen skjermkant under sliden (kopiert fra osd.c: barna er
 * uskalerte scene-buffere, 1:1 source-box-crop holder). */
static void
nd_clip_to_mon(NdToast *t, int x)
{
	struct wlr_scene_node *node;
	int lim = t->m->m.x + t->m->m.width - x;

	wl_list_for_each(node, &t->tree->children, link) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		int bw = sb->buffer ? sb->buffer->width : 0;
		int bh = sb->buffer ? sb->buffer->height : 0;
		int vis = lim - node->x;

		if (bw <= 0)
			continue;
		if (vis >= bw) {
			wlr_scene_node_set_enabled(node, 1);
			wlr_scene_buffer_set_source_box(sb, NULL);
			wlr_scene_buffer_set_dest_size(sb, bw, bh);
		} else if (vis <= 0) {
			wlr_scene_node_set_enabled(node, 0);
		} else {
			struct wlr_fbox src = { 0, 0, vis, bh };
			wlr_scene_node_set_enabled(node, 1);
			wlr_scene_buffer_set_source_box(sb, &src);
			wlr_scene_buffer_set_dest_size(sb, vis, bh);
		}
	}
}

/* Fjern enkel Pango-markup (<b>, <i>, <a href=…>) og de fem
 * XML-entitetene spec-en tillater i body. */
static void
nd_strip_markup(char *dst, size_t dstlen, const char *src)
{
	size_t o = 0;

	for (size_t i = 0; src[i] && o + 1 < dstlen; i++) {
		if (src[i] == '<') {
			while (src[i] && src[i] != '>')
				i++;
			if (!src[i])
				break;
			continue;
		}
		if (src[i] == '&') {
			static const struct { const char *ent; char ch; } tab[] = {
				{ "&amp;", '&' }, { "&lt;", '<' },
				{ "&gt;", '>' }, { "&quot;", '"' },
				{ "&apos;", '\'' },
			};
			size_t j;
			for (j = 0; j < LENGTH(tab); j++) {
				size_t n = strlen(tab[j].ent);
				if (!strncmp(src + i, tab[j].ent, n)) {
					dst[o++] = tab[j].ch;
					i += n - 1;
					break;
				}
			}
			if (j < LENGTH(tab))
				continue;
		}
		dst[o++] = src[i];
	}
	dst[o] = '\0';
}

/* Grådig ordbryting på pikselbredde; \n bryter alltid. */
static void
nd_body_rows(Card *card, const char *body)
{
	char clean[512];
	char line[160];
	const char *p;
	int lines = 0;

	nd_strip_markup(clean, sizeof(clean), body);
	p = clean;
	while (*p && lines < ND_MAX_LINES) {
		size_t n = 0, brk = 0;

		while (p[n] && p[n] != '\n') {
			if (n + 1 >= sizeof(line))
				break;
			line[n] = p[n];
			line[n + 1] = '\0';
			if (card_text_width(line) > ND_WRAP_PX && brk > 0) {
				n = brk;
				break;
			}
			n++;
			if (p[n] == ' ')
				brk = n;
		}
		memcpy(line, p, n);
		line[n] = '\0';
		if (line[0])
			card_text(card, line, NULL, NULL);
		lines++;
		p += n;
		while (*p == ' ' || *p == '\n')
			p++;
	}
}

/* (Re)bygg kortinnholdet i t->tree og oppdater t->w/h. */
static int
nd_build_card(NdToast *t, const char *app, const char *summary,
		const char *body)
{
	Card *card = card_begin();
	CardResult res;
	struct wlr_scene_node *node, *tmp;
	struct wlr_scene_buffer *sb;
	char sub[48];
	char title[96];

	if (!card)
		return 0;

	/* App-navn som small-caps undertittel, som "AUDIO INPUT" i
	 * volum-popupen. */
	snprintf(sub, sizeof(sub), "%s", app && app[0] ? app : "Notification");
	for (size_t i = 0; sub[i]; i++)
		if (sub[i] >= 'a' && sub[i] <= 'z')
			sub[i] -= 'a' - 'A';
	nd_strip_markup(title, sizeof(title), summary && summary[0] ?
			summary : "Notification");

	card_header(card, NULL, title, sub, NULL);
	if (body && body[0]) {
		card_gap(card, 2);
		nd_body_rows(card, body);
	}
	if (card_finish(card, &res))
		return 0;

	wl_list_for_each_safe(node, tmp, &t->tree->children, link)
		wlr_scene_node_destroy(node);
	sb = wlr_scene_buffer_create(t->tree, NULL);
	if (sb) {
		wlr_scene_buffer_set_buffer(sb, res.buf);
		wlr_scene_node_set_position(&sb->node, 0, 0);
	}
	wlr_buffer_drop(res.buf);
	t->w = res.w;
	t->h = res.h;
	return sb != NULL;
}

/* Game mode: foretrekk en skjerm uten fullskjermsklient (samme DND-logikk
 * som notify.c); finnes ingen, vises ikke varselet. */
static Monitor *
nd_pick_mon(void)
{
	Monitor *it;

	if (!game_mode_active)
		return selmon;
	wl_list_for_each(it, &mons, link)
		if (it->wlr_output && it->wlr_output->enabled
				&& !fullscreen_visible_on(it))
			return it;
	return NULL;
}

static int
notifyd_method_notify(sd_bus_message *msg, void *userdata,
		sd_bus_error *ret_error)
{
	const char *app = NULL, *icon = NULL, *summary = NULL, *body = NULL;
	uint32_t rid = 0;
	int32_t expire = -1;
	uint64_t hold;
	Monitor *m;
	NdToast *t = NULL, *it;
	int r;

	r = sd_bus_message_read(msg, "susss", &app, &rid, &icon, &summary,
			&body);
	if (r < 0)
		return r;
	sd_bus_message_skip(msg, "as");
	sd_bus_message_skip(msg, "a{sv}");
	sd_bus_message_read(msg, "i", &expire);

	if (rid)
		wl_list_for_each(it, &nd_toasts, link)
			if (it->id == rid) {
				t = it;
				break;
			}

	m = t ? t->m : nd_pick_mon();
	if (!m || !m->wlr_output) {
		/* DND (game mode) eller ingen skjerm: svar med id uten kort. */
		return sd_bus_reply_method_return(msg, "u", notifyd_next_id++);
	}

	if (!t) {
		t = calloc(1, sizeof(*t));
		if (!t)
			return -ENOMEM;
		t->id = notifyd_next_id++;
		t->m = m;
		t->close_reason = 1;
		t->tree = wlr_scene_tree_create(layers[LyrOverlay]);
		if (!t->tree) {
			free(t);
			return -ENOMEM;
		}
		if (!nd_build_card(t, app, summary, body)) {
			wlr_scene_node_destroy(&t->tree->node);
			free(t);
			return sd_bus_reply_method_return(msg, "u",
					notifyd_next_id - 1);
		}
		t->slot_y = nd_free_slot_y(m, t->h);
		t->off_x = m->m.x + m->m.width + ND_GAP;
		t->x_f = (double)t->off_x;
		t->x_vel = 0.0;
		wlr_scene_node_set_position(&t->tree->node, t->off_x, t->slot_y);
		t->timer = wl_event_loop_add_timer(event_loop, nd_hide_timeout,
				t);
		wl_list_insert(&nd_toasts, &t->link);
	} else if (!nd_build_card(t, app, summary, body)) {
		return sd_bus_reply_method_return(msg, "u", t->id);
	}

	/* Re-anker (bredden kan ha endret seg) og gli inn igjen om kortet var
	 * på vei ut. */
	t->target_x = m->m.x + m->m.width - t->w - ND_MARGIN;
	t->hiding = 0;
	hold = expire > 0 ? MIN((uint64_t)expire, (uint64_t)ND_MAX_HOLD_MS)
		: ND_HOLD_MS;
	if (t->timer)
		wl_event_source_timer_update(t->timer, (int)hold);
	wlr_scene_node_raise_to_top(&t->tree->node);
	nd_clip_to_mon(t, (int)t->x_f);
	nd_schedule(m);

	return sd_bus_reply_method_return(msg, "u", t->id);
}

static int
notifyd_method_close(sd_bus_message *msg, void *userdata,
		sd_bus_error *ret_error)
{
	uint32_t id = 0;
	NdToast *t;
	int r;

	r = sd_bus_message_read(msg, "u", &id);
	if (r < 0)
		return r;
	wl_list_for_each(t, &nd_toasts, link) {
		if (t->id != id)
			continue;
		t->close_reason = 3;
		t->hiding = 1;
		t->target_x = t->off_x;
		nd_schedule(t->m);
		break;
	}
	return sd_bus_reply_method_return(msg, "");
}

static int
notifyd_method_caps(sd_bus_message *msg, void *userdata,
		sd_bus_error *ret_error)
{
	return sd_bus_reply_method_return(msg, "as", 2, "body",
			"body-markup");
}

static int
notifyd_method_info(sd_bus_message *msg, void *userdata,
		sd_bus_error *ret_error)
{
	return sd_bus_reply_method_return(msg, "ssss", "nixlytile",
			"nixlytile", VERSION, "1.2");
}

static int
notifyd_bus_event(int fd, uint32_t mask, void *data)
{
	sd_bus *bus = data;
	int r;
	int events;
	uint32_t newmask;

	(void)fd;
	if (!bus)
		return 0;

	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		wlr_log(WLR_ERROR,
				"notifyd: session bus hangup — disabling notifications");
		if (notifyd_event) {
			wl_event_source_remove(notifyd_event);
			notifyd_event = NULL;
		}
		return 0;
	}

	while ((r = sd_bus_process(bus, NULL)) > 0)
		;

	events = sd_bus_get_events(bus);
	newmask = 0;
	if (events & SD_BUS_EVENT_READABLE)
		newmask |= WL_EVENT_READABLE;
	if (events & SD_BUS_EVENT_WRITABLE)
		newmask |= WL_EVENT_WRITABLE;
	if (notifyd_event)
		wl_event_source_fd_update(notifyd_event,
				newmask ? newmask : WL_EVENT_READABLE);
	return 0;
}

void
notifyd_init(void)
{
	static const sd_bus_vtable notifyd_vtable[] = {
		SD_BUS_VTABLE_START(0),
		SD_BUS_METHOD("Notify", "susssasa{sv}i", "u",
				notifyd_method_notify, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD("CloseNotification", "u", "",
				notifyd_method_close, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD("GetCapabilities", "", "as",
				notifyd_method_caps, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_METHOD("GetServerInformation", "", "ssss",
				notifyd_method_info, SD_BUS_VTABLE_UNPRIVILEGED),
		SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
		SD_BUS_SIGNAL("ActionInvoked", "us", 0),
		SD_BUS_VTABLE_END
	};
	uint64_t name_flags = SD_BUS_NAME_ALLOW_REPLACEMENT
		| SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_QUEUE;
	int r;
	int fd;
	int events;
	uint32_t mask;

	if (notifyd_bus)
		return;

	r = sd_bus_open_user(&notifyd_bus);
	if (r < 0) {
		wlr_log(WLR_ERROR, "notifyd: failed to connect to session bus: %s",
				strerror(-r));
		notifyd_bus = NULL;
		return;
	}

	r = sd_bus_add_object_vtable(notifyd_bus, &notifyd_vtable_slot,
			"/org/freedesktop/Notifications",
			"org.freedesktop.Notifications", notifyd_vtable, NULL);
	if (r < 0)
		goto fail;
	/* QUEUE: står en dunst/mako og eier navnet uten å tillate
	 * replacement, overtar vi automatisk i det den avslutter. */
	r = sd_bus_request_name(notifyd_bus, "org.freedesktop.Notifications",
			name_flags);
	if (r < 0) {
		wlr_log(WLR_ERROR, "notifyd: failed to request name: %s",
				strerror(-r));
		goto fail;
	}

	fd = sd_bus_get_fd(notifyd_bus);
	events = sd_bus_get_events(notifyd_bus);
	mask = 0;
	if (events & SD_BUS_EVENT_READABLE)
		mask |= WL_EVENT_READABLE;
	if (events & SD_BUS_EVENT_WRITABLE)
		mask |= WL_EVENT_WRITABLE;
	if (mask == 0)
		mask = WL_EVENT_READABLE;
	notifyd_event = wl_event_loop_add_fd(event_loop, fd, mask,
			notifyd_bus_event, notifyd_bus);
	return;
fail:
	if (notifyd_vtable_slot)
		sd_bus_slot_unref(notifyd_vtable_slot);
	notifyd_vtable_slot = NULL;
	sd_bus_unref(notifyd_bus);
	notifyd_bus = NULL;
}

void
notifyd_tick(Monitor *m, double dt, int *still)
{
	NdToast *t, *tmp;

	*still = 0;
	wl_list_for_each_safe(t, tmp, &nd_toasts, link) {
		if (t->m != m)
			continue;
		if (spring_tick(&t->x_f, &t->x_vel, (double)t->target_x,
				SPRING_ND, dt)) {
			wlr_scene_node_set_position(&t->tree->node,
					(int)t->x_f, t->slot_y);
			nd_clip_to_mon(t, (int)t->x_f);
			*still = 1;
			continue;
		}
		wlr_scene_node_set_position(&t->tree->node,
				t->target_x, t->slot_y);
		nd_clip_to_mon(t, t->target_x);
		if (t->hiding)
			nd_toast_destroy(t);
	}
}

/* Monitor is being destroyed — drop its toasts before m is freed. */
void
notifyd_purge_mon(Monitor *m)
{
	NdToast *t, *tmp;

	wl_list_for_each_safe(t, tmp, &nd_toasts, link)
		if (t->m == m)
			nd_toast_destroy(t);
}
