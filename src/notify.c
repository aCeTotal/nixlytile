#include "nixlytile.h"
#include "client.h"

/* Varselbane.
 *
 * Små flytende vinduer uten fokus-ønske — Steam sine vennevarsler, blueman
 * sine tilkoblingsbokser — er ikke dialoger og skal ikke behandles som det.
 * Uten denne banen treffer de sentrerings-regelen i mapnotify/configurex11
 * og lander midt på skjermen, over det du holder på med.
 *
 * Her adopteres de i stedet: parkeres i høyre marg, glir inn fra utsiden,
 * står NOTIF_HOLD_MS, og glir ut igjen. Unntaket er prompts som venter på
 * inntasting (passordbokser): de får tastaturet og blir stående til klienten
 * selv river dem ned — se notif_wants_input(). Vinduet lukkes ALDRI av oss — det
 * gjemmes bare når det har glidd ut. Steam og blueman gjenbruker og river
 * ned sine egne varselvinduer, og en close fra compositoren midt i den
 * livssyklusen kan få dem til å slutte å vise varsler i det hele tatt.
 *
 * Slide-en kjøres på scene-noden direkte, ikke via resize(): resize() kaller
 * applybounds() som klemmer klienten inn i flate-arealet, og da kommer den
 * aldri utenfor skjermkanten. */

/* Hvor langt inn fra skjermkanten varselet står, og luft mellom dem. */
#define NOTIF_MARGIN 16
#define NOTIF_GAP 10
/* Synlig tid før den glir ut igjen. */
#define NOTIF_HOLD_MS 4000
/* Størrelsestak for å bli regnet som varsel, som andel av skjermen. Over
 * dette er det et ekte vindu selv om det er flytende og fokusløst. */
#define NOTIF_MAX_W_NUM 2
#define NOTIF_MAX_W_DEN 5
#define NOTIF_MAX_H_NUM 1
#define NOTIF_MAX_H_DEN 4

/* Samme følelse som vindusfjæra ellers i compositoren (kritisk dempet, ingen
 * overshoot). SPRING_WINDOW er static i anim.c, så verdiene gjentas her. */
static const SpringParams SPRING_NOTIF = { 1.0, 1.0, 800.0 };

struct wl_list notifs;

/* request_frame() er static inline i output.c. Her holder det å be om en
 * frame direkte — fjæra tikkes videre av monitor_anim_tick. */
static void
notif_schedule(Monitor *m)
{
	if (m && m->wlr_output)
		wlr_output_schedule_frame(m->wlr_output);
}

/* Klipp varselet mot sin egen skjermkant. Noden glir i globale
 * layout-koordinater, så delen som stikker utenfor høyre kant ville ellers
 * blitt tegnet på venstre side av naboskjermen. Surface-treet klippes i
 * surface-lokale koordinater; kantrektene kappes i samme bredde. */
static void
notif_clip_to_mon(Notif *n, int x)
{
	Client *c = n->c;
	struct wlr_box clip;
	int vis = n->m->m.x + n->m->m.width - x;
	int w = c->geom.width;
	int rw;

	if (!c->scene_surface)
		return;
	if (vis < 0)
		vis = 0;
	if (vis > w)
		vis = w;

	client_get_clip(c, &clip);
	if (clip.width > vis - (int)c->bw)
		clip.width = MAX(vis - (int)c->bw, 0);
	/* En TOM klippeboks får wlroots til å fjerne klippet og vise hele
	 * surfacet — helt utenfor kanten betyr det at varselet dukker opp
	 * i full bredde på naboskjermen. Bruk en boks langt utenfor
	 * surfacet i stedet (samme triks som client_clip_to_usable). */
	if (clip.width <= 0 || clip.height <= 0)
		clip = (struct wlr_box){ 1 << 20, 0, 1, 1 };
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);

	/* Unmanaged (override-redirect) klienter har ingen border-rekter. */
	if (!c->border[0])
		return;

	if (vis >= w) {
		client_set_border_size(c, w, c->geom.height);
		return;
	}
	/* border[0]/[1] = topp/bunn (0,·), [2] = venstre, [3] = høyre ved
	 * x = w - bw. Kapp alt ved vis. */
	wlr_scene_rect_set_size(c->border[0], vis, c->bw);
	wlr_scene_rect_set_size(c->border[1], vis, c->bw);
	wlr_scene_rect_set_size(c->border[2], MIN((int)c->bw, vis),
			MAX(c->geom.height - 2 * (int)c->bw, 0));
	rw = vis - (w - (int)c->bw);
	if (rw < 0)
		rw = 0;
	wlr_scene_rect_set_size(c->border[3], rw,
			MAX(c->geom.height - 2 * (int)c->bw, 0));
}

/* Kalles fra commit-handleren i client.c: den frisker opp surface-klippet
 * på hver commit, og ville ellers gjenopprettet full bredde midt i sliden. */
void
notify_refresh_clip(Client *c)
{
	Notif *n;

	wl_list_for_each(n, &notifs, link) {
		if (n->c != c)
			continue;
		notif_clip_to_mon(n, (int)n->x_f);
		return;
	}
}

static int
notif_hide_timeout(void *data)
{
	Notif *n = data;

	/* Sikkerhetsnett for prompts notif_wants_input() ikke kjente igjen:
	 * har boksen tastaturet når timeren går, står brukeren og skriver i
	 * den. Da blir den stående — ingen ny timer. */
	if (n->c && client_surface(n->c) == seat->keyboard_state.focused_surface) {
		n->sticky = 1;
		return 0;
	}

	n->hiding = 1;
	n->target_x = n->off_x;
	notif_schedule(n->m);
	return 0;
}

/* Ledig y i margen: stable nedover fra toppen av flate-arealet (m->w.y
 * ligger allerede under statuslinja) og hopp over slots som er tatt. */
static int
notif_free_slot_y(Monitor *m, int h)
{
	int y = m->w.y + NOTIF_MARGIN;
	int again = 1;

	while (again) {
		Notif *o;
		again = 0;
		wl_list_for_each(o, &notifs, link) {
			if (o->m != m || o->hiding)
				continue;
			if (y < o->slot_y + o->h + NOTIF_GAP
					&& o->slot_y < y + h + NOTIF_GAP) {
				y = o->slot_y + o->h + NOTIF_GAP;
				again = 1;
				break;
			}
		}
	}
	return y;
}

/* Er dette et varsel? Kun X11: "uten fokus-ønske" finnes som begrep i
 * ICCCM/override-redirect, men har ingen motpart i xdg-shell — en Wayland-
 * toplevel kan ikke si at den ikke vil ha fokus, så vi har ingenting å
 * klassifisere på der og lar dem være. */
/* Skjermen varselet hører til. Override-redirect-klienter går aldri gjennom
 * applyrules/setmon, så c->mon er NULL for dem — utled fra der vinduet ble
 * mappet, ellers aktiv skjerm. */
static Monitor *
notif_monitor_for(Client *c)
{
	Monitor *m;

	if (c->mon)
		return c->mon;
	/* Mappet på (0,0) = uposisjonert (samme heuristikk som mapnotify) —
	 * Steam legger varslene sine der, og xytomon ville da alltid valgt
	 * skjermen øverst til venstre i stedet for den aktive. */
	if (c->geom.x == 0 && c->geom.y == 0)
		return selmon;
	m = xytomon(c->geom.x + c->geom.width / 2,
			c->geom.y + c->geom.height / 2);
	return m ? m : selmon;
}

#ifdef XWAYLAND
/* Nedtrekksmeny/kontekstmeny fra en X11-klient. */
static int
notif_is_menu(Client *c)
{
	static const enum wlr_xwayland_net_wm_window_type type[] = {
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_MENU,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_POPUP_MENU,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_COMBO,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR,
		WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DND,
	};
	size_t i;

	for (i = 0; i < LENGTH(type); i++)
		if (wlr_xwayland_surface_has_window_type(c->surface.xwayland,
					type[i]))
			return 1;

	/* Uten vindustype (eldre toolkits): menyen åpnes inntil pekeren.
	 * Litt slark rundt boksen, siden en meny kan åpne seg opp/til
	 * venstre når det ikke er plass under. */
	if (client_is_unmanaged(c) && cursor) {
		struct wlr_box b = c->geom;
		int slack = 8;
		if (cursor->x >= b.x - slack && cursor->x <= b.x + b.width + slack
				&& cursor->y >= b.y - slack
				&& cursor->y <= b.y + b.height + slack)
			return 1;
	}
	return 0;
}
#endif

static int
notify_looks_like_notification(Client *c, Monitor **out)
{
	Monitor *m;
	int max_w, max_h;

	if (!c)
		return 0;
	if (c->isfullscreen || c->is_game_splash || looks_like_game(c))
		return 0;

	m = notif_monitor_for(c);
	if (!m || m->m.width <= 0 || m->m.height <= 0)
		return 0;
	*out = m;
	max_w = m->m.width * NOTIF_MAX_W_NUM / NOTIF_MAX_W_DEN;
	max_h = m->m.height * NOTIF_MAX_H_NUM / NOTIF_MAX_H_DEN;
	if (c->geom.width <= 0 || c->geom.height <= 0)
		return 0;
	if (c->geom.width > max_w || c->geom.height > max_h)
		return 0;

#ifdef XWAYLAND
	if (client_is_x11(c)) {
		/* Menyer er override-redirect og ber ikke om fokus — samme
		 * signatur som et varsel — men de hører hjemme der klienten
		 * la dem, under musepekeren. Vindustypen sier det når den er
		 * satt; ellers: en boks som åpner seg under pekeren er en
		 * meny, et varsel dukker opp der brukeren ikke er. */
		if (notif_is_menu(c))
			return 0;

		/* Override-redirect som ikke ber om fokus: Steams vennevarsler.
		 * client_wants_focus() er allerede compositorens definisjon av
		 * "denne vil ha input" for akkurat den klassen. */
		if (client_is_unmanaged(c))
			return !client_wants_focus(c);

		/* Managed X11: vindustypen er utvetydig når den er satt, og
		 * fanger de som ikke er override-redirect. */
		if (!c->isfloating)
			return 0;
		if (wlr_xwayland_surface_has_window_type(c->surface.xwayland,
					WLR_XWAYLAND_NET_WM_WINDOW_TYPE_NOTIFICATION)
				|| wlr_xwayland_surface_has_window_type(c->surface.xwayland,
					WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLTIP))
			return 1;
		/* Ellers: liten, flytende og uten input-modell = varsel.
		 * ICCCM WM_HINTS input=False + ingen WM_TAKE_FOCUS er så nær
		 * "vil ikke ha fokus" som X11 kommer for managed vinduer. */
		return wlr_xwayland_surface_icccm_input_model(c->surface.xwayland)
				== WLR_ICCCM_INPUT_MODEL_NONE;
	}
#endif

	/* Wayland xdg-toplevel. Her finnes ingen "vil ikke ha fokus"-hint i
	 * det hele tatt — xdg-shell har ikke noe motstykke til ICCCM-input
	 * eller override-redirect. Nærmeste holdbare proxy er: liten, flytende,
	 * og UTEN parent. En ekte dialog setter parent på toplevel'en sin
	 * (set_parent) nettopp for å si "jeg hører til det vinduet"; et varsel
	 * står alene. Uten parent-kravet ville en liten frittstående dialog
	 * blitt skjøvet ut i margen og forsvunnet etter 4 s. */
	if (!c->isfloating)
		return 0;
	if (!c->surface.xdg || !c->surface.xdg->toplevel)
		return 0;
	return c->surface.xdg->toplevel->parent == NULL;
}

/* Passordbokser — polkit-agenten, pinentry, ssh-askpass, nøkkelring — er
 * små, flytende og uten parent, altså nøyaktig samme signatur som et varsel.
 * De skal fortsatt gli inn i margen, men de venter på inntasting: de får
 * tastaturet, og de glir aldri ut av seg selv. */
static int
notif_wants_input(Client *c)
{
	static const char *const pat[] = {
		"polkit", "pinentry", "askpass", "keyring", "gcr",
		"authenticat", "autentiser", "password", "passord",
	};
	const char *field[2];
	size_t i, j;

	/* xdg-dialog-v1: klienten sier selv at dette er en dialog, ikke et
	 * varsel. GTK4/libadwaita setter den på alle dialogene sine. */
	if (!client_is_x11(c) && c->surface.xdg && c->surface.xdg->toplevel
			&& wlr_xdg_dialog_v1_try_from_wlr_xdg_toplevel(
				c->surface.xdg->toplevel))
		return 1;

	field[0] = client_get_appid(c);
	field[1] = client_get_title(c);
	for (i = 0; i < LENGTH(field); i++) {
		if (!field[i])
			continue;
		for (j = 0; j < LENGTH(pat); j++)
			if (strcasestr(field[i], pat[j]))
				return 1;
	}
	return 0;
}

/* Er dette varselet en prompt som venter på tastetrykk? Kalles fra
 * mapnotify rett etter adopsjonen for å gi den fokus. */
int
notify_wants_keyboard(Client *c)
{
	Notif *n;

	if (!c || !c->is_notif)
		return 0;
	wl_list_for_each(n, &notifs, link)
		if (n->c == c)
			return n->sticky;
	return 0;
}

int
notify_try_adopt(Client *c)
{
	Monitor *m = NULL;
	Notif *n;
	int park = 0;

	if (!notify_looks_like_notification(c, &m) || !m)
		return 0;

	/* Game mode: a popup on the game monitor forces composition (and an
	 * animation) for ~4.5 s. Prefer a monitor without a fullscreen
	 * client; if there is none, park a non-sticky popup offscreen until
	 * its timeout (DND). Sticky prompts (polkit etc.) must stay visible. */
	if (game_mode_active) {
		Monitor *alt = NULL, *it;
		wl_list_for_each(it, &mons, link) {
			if (it->wlr_output && it->wlr_output->enabled &&
					!fullscreen_visible_on(it)) {
				alt = it;
				break;
			}
		}
		if (alt)
			m = alt;
		else if (!notif_wants_input(c))
			park = 1;
	}

	n = calloc(1, sizeof(*n));
	if (!n)
		return 0;

	n->c = c;
	n->m = m;
	n->w = c->geom.width;
	n->h = c->geom.height;
	n->slot_y = notif_free_slot_y(n->m, n->h);
	n->target_x = n->m->m.x + n->m->m.width - n->w - NOTIF_MARGIN;
	if (park)
		n->target_x = n->m->m.x + n->m->m.width + NOTIF_GAP;
	/* Start helt utenfor kanten, ikke bare delvis: en varselboks som
	 * dukker opp halvveis inne har allerede "poppet" før den glir. */
	n->off_x = n->m->m.x + n->m->m.width + NOTIF_GAP;
	n->x_f = (double)n->off_x;
	n->x_vel = 0.0;
	n->hiding = 0;
	n->sticky = notif_wants_input(c);

	c->is_notif = 1;
	c->geom.x = n->target_x;
	c->geom.y = n->slot_y;
	wlr_scene_node_set_position(&c->scene->node, n->off_x, n->slot_y);
	notif_clip_to_mon(n, n->off_x);

	if (!n->sticky) {
		n->timer = wl_event_loop_add_timer(event_loop,
				notif_hide_timeout, n);
		if (n->timer)
			wl_event_source_timer_update(n->timer, NOTIF_HOLD_MS);
	}

	wl_list_insert(&notifs, &n->link);
	notif_schedule(n->m);
	return 1;
}

/* Sett noden tilbake til utgangspunktet utenfor kanten.
 *
 * Managed klienter må gjennom resize() etter adopsjonen for å få rammer og
 * flate på plass i slotten, og resize() setter scene-noden til c->geom. Det
 * ville plassert varselet ferdig innslidd. Kalles derfor rett etter resize,
 * så fjæra har noe å gli fra. */
void
notify_start_offscreen(Client *c)
{
	Notif *n;

	if (!c || !c->is_notif)
		return;
	wl_list_for_each(n, &notifs, link) {
		if (n->c != c)
			continue;
		wlr_scene_node_set_position(&c->scene->node, n->off_x, n->slot_y);
		notif_clip_to_mon(n, n->off_x);
		return;
	}
}

void
notify_release(Client *c)
{
	Notif *n, *tmp;

	if (!c || !c->is_notif)
		return;
	wl_list_for_each_safe(n, tmp, &notifs, link) {
		if (n->c != c)
			continue;
		if (n->timer)
			wl_event_source_remove(n->timer);
		wl_list_remove(&n->link);
		free(n);
	}
	c->is_notif = 0;
}

void
notify_tick(Monitor *m, double dt, int *still)
{
	Notif *n, *tmp;

	*still = 0;
	wl_list_for_each_safe(n, tmp, &notifs, link) {
		if (n->m != m)
			continue;
		if (!n->c || !client_surface(n->c)
				|| !client_surface(n->c)->mapped) {
			/* Klienten forsvant under animasjonen. */
			if (n->timer)
				wl_event_source_remove(n->timer);
			wl_list_remove(&n->link);
			free(n);
			continue;
		}

		if (spring_tick(&n->x_f, &n->x_vel, (double)n->target_x,
				SPRING_NOTIF, dt)) {
			wlr_scene_node_set_position(&n->c->scene->node,
					(int)n->x_f, n->slot_y);
			notif_clip_to_mon(n, (int)n->x_f);
			*still = 1;
			continue;
		}

		/* Fjæra er i mål. */
		wlr_scene_node_set_position(&n->c->scene->node,
				n->target_x, n->slot_y);
		notif_clip_to_mon(n, n->target_x);
		if (!n->hiding)
			continue;

		/* Ute av syne: gjem noden og slipp slotten. Vinduet lever
		 * videre — klienten eier det og river det ned selv. */
		wlr_scene_node_set_enabled(&n->c->scene->node, 0);
		n->c->is_notif = 0;
		if (n->timer)
			wl_event_source_remove(n->timer);
		wl_list_remove(&n->link);
		free(n);
	}
}
