/* gshortcuts.c — org.freedesktop.impl.portal.GlobalShortcuts backend.
 *
 * Lets portal-aware apps (Discord, OBS, ...) register global keyboard
 * shortcuts that fire regardless of focus — including inside fullscreen
 * games holding a keyboard-shortcuts-inhibitor.  xdg-desktop-portal
 * (the frontend) talks to us over the session bus; we own
 * org.freedesktop.impl.portal.desktop.nixlytile and match key events in
 * keypress() before the lock/inhibitor checks.  A matched press emits
 * Activated, its release Deactivated (press-and-hold semantics — what
 * Discord push-to-talk needs), and the key is consumed so the focused
 * client never sees it.
 *
 * Triggers come from the app's preferred_trigger, overridable in
 * ~/.local/nixlyos/shortcuts.conf (hot-reloaded via inotify):
 *
 *   # app_id:shortcut_id=TRIGGER   ("*" matches any app_id)
 *   discord:Push to Talk (hold)=F13
 *   *:mute=CTRL+SHIFT+m
 *
 * Trigger syntax per the XDG shortcuts spec: MOD+...+key where MOD is
 * CTRL, ALT, SHIFT or LOGO (SUPER/META/WIN accepted) and key is an XKB
 * keysym name, case-insensitive.
 *
 * Install: nixlytile.portal + nixlytile-portals.conf (repo root) must
 * land in $(DATADIR)/xdg-desktop-portal/{portals/,} — see Makefile —
 * and XDG_CURRENT_DESKTOP must contain "nixlytile" (set at startup).
 */

#include "nixlytile.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define GS_BUS_NAME  "org.freedesktop.impl.portal.desktop.nixlytile"
#define GS_OBJ_PATH  "/org/freedesktop/portal/desktop"
#define GS_IFACE     "org.freedesktop.impl.portal.GlobalShortcuts"
#define GS_SESS_IFACE "org.freedesktop.impl.portal.Session"
#define GSCONF_NAME  "shortcuts.conf"

typedef struct {
	struct wl_list link;   /* GSSession.shortcuts */
	char *id;
	char *description;
	char *trigger;         /* resolved trigger string, NULL = unbound */
	uint32_t mods;
	xkb_keysym_t keysym;   /* lowered; NoSymbol = unbound */
	int held;
	uint32_t held_keycode;
} GSShortcut;

typedef struct {
	struct wl_list link;   /* gs_sessions */
	char *path;            /* session_handle object path */
	char *app_id;
	char *peer;            /* unique bus name of the portal frontend */
	struct wl_list shortcuts;
	sd_bus_slot *slot;
} GSSession;

static sd_bus *gs_bus;
static sd_bus_slot *gs_vtable_slot;
static sd_bus_slot *gs_name_slot;
static struct wl_event_source *gs_bus_source;
static struct wl_list gs_sessions;

static char gsconf_dir[PATH_MAX];
static char gsconf_path[PATH_MAX];
static int gsconf_fd = -1;
static struct wl_event_source *gsconf_source;

/* Interactive capture: a BindShortcuts with unbound shortcuts parks its
 * reply here and the next key combo the user presses becomes the
 * trigger (the compositor IS the portal's "bind dialog" — an OSD toast
 * prompts per shortcut, Esc skips one, 30 s timeout finishes). */
static sd_bus_message *gs_capture_reply;
static GSSession *gs_capture_sess;
static GSShortcut *gs_capture_sc;
static struct wl_event_source *gs_capture_timer;

/* ── Trigger parsing ─────────────────────────────────────────────── */

static int
gs_parse_trigger(const char *trigger, uint32_t *out_mods, xkb_keysym_t *out_sym)
{
	char buf[128];
	char *tok, *save = NULL;
	uint32_t mods = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;

	if (!trigger || !*trigger)
		return 0;
	snprintf(buf, sizeof(buf), "%s", trigger);

	for (tok = strtok_r(buf, "+", &save); tok;
			tok = strtok_r(NULL, "+", &save)) {
		if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control"))
			mods |= WLR_MODIFIER_CTRL;
		else if (!strcasecmp(tok, "alt"))
			mods |= WLR_MODIFIER_ALT;
		else if (!strcasecmp(tok, "shift"))
			mods |= WLR_MODIFIER_SHIFT;
		else if (!strcasecmp(tok, "logo") || !strcasecmp(tok, "super")
				|| !strcasecmp(tok, "meta") || !strcasecmp(tok, "win"))
			mods |= WLR_MODIFIER_LOGO;
		else
			sym = xkb_keysym_from_name(tok,
				XKB_KEYSYM_CASE_INSENSITIVE);
	}

	if (sym == XKB_KEY_NoSymbol)
		return 0;
	*out_mods = mods;
	*out_sym = xkb_keysym_to_lower(sym);
	return 1;
}

/* Look up an override in shortcuts.conf.  Returns a strdup'd trigger or
 * NULL.  Read on every resolve — the file is tiny and this only runs on
 * Bind/List/reload, never in the key path. */
static char *
gs_conf_lookup(const char *app_id, const char *id)
{
	char line[512];
	char *found = NULL;
	FILE *fp = fopen(gsconf_path, "r");

	if (!fp)
		return NULL;
	while (fgets(line, sizeof(line), fp)) {
		char *nl = strchr(line, '\n');
		char *colon, *eq;
		if (nl)
			*nl = '\0';
		if (line[0] == '#' || line[0] == '\0')
			continue;
		if (!(colon = strchr(line, ':')))
			continue;
		if (!(eq = strchr(colon + 1, '=')))
			continue;
		*colon = *eq = '\0';
		if (strcmp(colon + 1, id) != 0)
			continue;
		if (strcmp(line, "*") != 0 &&
				strcmp(line, app_id ? app_id : "") != 0)
			continue;
		free(found);
		found = strdup(eq + 1);
	}
	fclose(fp);
	return found;
}

/* conf override wins over the app's preferred_trigger. */
static void
gs_resolve(GSSession *sess, GSShortcut *sc, const char *preferred)
{
	char *conf = gs_conf_lookup(sess->app_id, sc->id);
	const char *pick = conf ? conf : preferred;

	free(sc->trigger);
	sc->trigger = NULL;
	sc->mods = 0;
	sc->keysym = XKB_KEY_NoSymbol;

	if (pick && gs_parse_trigger(pick, &sc->mods, &sc->keysym))
		sc->trigger = strdup(pick);
	free(conf);
}

/* ── Signals ─────────────────────────────────────────────────────── */

static void
gs_emit_active(GSSession *sess, GSShortcut *sc, int activated)
{
	sd_bus_message *sig = NULL;

	if (sd_bus_message_new_signal(gs_bus, &sig, GS_OBJ_PATH, GS_IFACE,
			activated ? "Activated" : "Deactivated") < 0)
		return;
	sd_bus_message_append(sig, "os", sess->path, sc->id);
	sd_bus_message_append(sig, "t", monotonic_msec());
	sd_bus_message_append(sig, "a{sv}", 0);
	sd_bus_send(gs_bus, sig, NULL);
	sd_bus_message_unref(sig);
}

/* Append the a(sa{sv}) shortcut list (id + description +
 * trigger_description) shared by BindShortcuts/ListShortcuts results
 * and the ShortcutsChanged signal. */
static void
gs_append_shortcuts(sd_bus_message *msg, GSSession *sess)
{
	GSShortcut *sc;

	sd_bus_message_open_container(msg, 'a', "(sa{sv})");
	wl_list_for_each(sc, &sess->shortcuts, link) {
		sd_bus_message_open_container(msg, 'r', "sa{sv}");
		sd_bus_message_append(msg, "s", sc->id);
		sd_bus_message_open_container(msg, 'a', "{sv}");
		sd_bus_message_append(msg, "{sv}", "description",
			"s", sc->description ? sc->description : "");
		sd_bus_message_append(msg, "{sv}", "trigger_description",
			"s", sc->trigger ? sc->trigger : "");
		sd_bus_message_close_container(msg);
		sd_bus_message_close_container(msg);
	}
	sd_bus_message_close_container(msg);
}

static void
gs_emit_changed(GSSession *sess)
{
	sd_bus_message *sig = NULL;

	if (sd_bus_message_new_signal(gs_bus, &sig, GS_OBJ_PATH, GS_IFACE,
			"ShortcutsChanged") < 0)
		return;
	sd_bus_message_append(sig, "o", sess->path);
	gs_append_shortcuts(sig, sess);
	sd_bus_send(gs_bus, sig, NULL);
	sd_bus_message_unref(sig);
}

/* ── Session lifecycle ───────────────────────────────────────────── */

static void
gs_shortcut_free(GSShortcut *sc)
{
	wl_list_remove(&sc->link);
	free(sc->id);
	free(sc->description);
	free(sc->trigger);
	free(sc);
}

static void gs_capture_cancel(GSSession *sess);

static void
gs_session_free(GSSession *sess)
{
	GSShortcut *sc, *tmp;

	gs_capture_cancel(sess);
	/* Never leave a held shortcut dangling (Discord stuck unmuted). */
	wl_list_for_each(sc, &sess->shortcuts, link)
		if (sc->held)
			gs_emit_active(sess, sc, 0);
	wl_list_for_each_safe(sc, tmp, &sess->shortcuts, link)
		gs_shortcut_free(sc);
	wl_list_remove(&sess->link);
	if (sess->slot)
		sd_bus_slot_unref(sess->slot);
	free(sess->path);
	free(sess->app_id);
	free(sess->peer);
	free(sess);
}

static GSSession *
gs_session_find(const char *path)
{
	GSSession *sess;
	wl_list_for_each(sess, &gs_sessions, link)
		if (strcmp(sess->path, path) == 0)
			return sess;
	return NULL;
}

static int
gs_session_close(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	GSSession *sess = userdata;

	(void)err;
	gs_session_free(sess);
	return sd_bus_reply_method_return(m, "");
}

static int
gs_prop_version(sd_bus *bus, const char *path, const char *iface,
	const char *prop, sd_bus_message *reply, void *userdata,
	sd_bus_error *err);

static const sd_bus_vtable gs_session_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", gs_session_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("Closed", "", 0),
	SD_BUS_PROPERTY("version", "u", gs_prop_version, 0,
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

/* ── Portal methods ──────────────────────────────────────────────── */

static int
gs_create_session(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *handle, *session_handle, *app_id;
	const char *sender;
	GSSession *sess;
	int r;

	(void)userdata; (void)err;
	r = sd_bus_message_read(m, "oos", &handle, &session_handle, &app_id);
	if (r < 0)
		return r;
	sd_bus_message_skip(m, "a{sv}");

	/* A stale session at the same path (frontend restart) is replaced. */
	if ((sess = gs_session_find(session_handle)))
		gs_session_free(sess);

	sess = calloc(1, sizeof(*sess));
	if (!sess)
		return sd_bus_reply_method_return(m, "ua{sv}", 2, 0);
	sess->path = strdup(session_handle);
	sess->app_id = strdup(app_id);
	sender = sd_bus_message_get_sender(m);
	sess->peer = strdup(sender ? sender : "");
	wl_list_init(&sess->shortcuts);
	wl_list_init(&sess->link);

	r = sd_bus_add_object_vtable(gs_bus, &sess->slot, sess->path,
		GS_SESS_IFACE, gs_session_vtable, sess);
	if (r < 0) {
		wl_list_remove(&sess->link);
		free(sess->path); free(sess->app_id); free(sess->peer);
		free(sess);
		return sd_bus_reply_method_return(m, "ua{sv}", 2, 0);
	}
	wl_list_insert(&gs_sessions, &sess->link);

	wlr_log(WLR_INFO, "gshortcuts: session %s for app '%s'",
		session_handle, app_id);
	return sd_bus_reply_method_return(m, "ua{sv}", 0, 0);
}

static int
gs_reply_shortcuts(sd_bus_message *m, GSSession *sess)
{
	sd_bus_message *reply = NULL;
	int r;

	r = sd_bus_message_new_method_return(m, &reply);
	if (r < 0)
		return r;
	sd_bus_message_append(reply, "u", 0);
	sd_bus_message_open_container(reply, 'a', "{sv}");
	sd_bus_message_open_container(reply, 'e', "sv");
	sd_bus_message_append(reply, "s", "shortcuts");
	sd_bus_message_open_container(reply, 'v', "a(sa{sv})");
	gs_append_shortcuts(reply, sess);
	sd_bus_message_close_container(reply);
	sd_bus_message_close_container(reply);
	sd_bus_message_close_container(reply);
	r = sd_bus_send(gs_bus, reply, NULL);
	sd_bus_message_unref(reply);
	return r;
}

/* ── Interactive capture ─────────────────────────────────────────── */

/* Persist a captured trigger: rewrite shortcuts.conf with the matching
 * line replaced (or appended).  Apps like Discord re-send their own
 * (possibly empty) preferred_trigger on every restart — the conf line
 * makes the user's captured combo win from then on. */
static void
gs_conf_store(const char *app_id, const char *id, const char *trigger)
{
	char tmp_path[PATH_MAX + 8];
	char line[512];
	FILE *in, *out;

	{
		char parent[PATH_MAX];
		char *slash;
		snprintf(parent, sizeof(parent), "%s", gsconf_dir);
		if ((slash = strrchr(parent, '/'))) {
			*slash = '\0';
			mkdir(parent, 0755);
		}
	}
	mkdir(gsconf_dir, 0755);
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", gsconf_path);
	out = fopen(tmp_path, "w");
	if (!out)
		return;
	in = fopen(gsconf_path, "r");
	if (in) {
		while (fgets(line, sizeof(line), in)) {
			char probe[512];
			char *colon, *eq;
			snprintf(probe, sizeof(probe), "%s", line);
			if ((colon = strchr(probe, ':')) &&
					(eq = strchr(colon + 1, '='))) {
				*colon = *eq = '\0';
				if (strcmp(probe, app_id) == 0 &&
						strncmp(colon + 1, id,
							(size_t)(eq - colon - 1)) == 0 &&
						strlen(id) == (size_t)(eq - colon - 1))
					continue;
			}
			fputs(line, out);
		}
		fclose(in);
	}
	fprintf(out, "%s:%s=%s\n", app_id, id, trigger);
	fclose(out);
	rename(tmp_path, gsconf_path);
}

static void
gs_trigger_string(uint32_t mods, xkb_keysym_t sym, char *buf, size_t len)
{
	char name[64];

	buf[0] = '\0';
	if (mods & WLR_MODIFIER_CTRL)
		strncat(buf, "CTRL+", len - strlen(buf) - 1);
	if (mods & WLR_MODIFIER_ALT)
		strncat(buf, "ALT+", len - strlen(buf) - 1);
	if (mods & WLR_MODIFIER_SHIFT)
		strncat(buf, "SHIFT+", len - strlen(buf) - 1);
	if (mods & WLR_MODIFIER_LOGO)
		strncat(buf, "LOGO+", len - strlen(buf) - 1);
	if (xkb_keysym_get_name(sym, name, sizeof(name)) <= 0)
		snprintf(name, sizeof(name), "0x%x", sym);
	strncat(buf, name, len - strlen(buf) - 1);
}

static void
gs_capture_prompt(void)
{
	char msg[256];

	if (!gs_capture_sc || !selmon)
		return;
	snprintf(msg, sizeof(msg), "Trykk hurtigtast for \xe2\x80\x9c%s\xe2\x80\x9d (Esc hopper over)",
		gs_capture_sc->description && gs_capture_sc->description[0]
			? gs_capture_sc->description : gs_capture_sc->id);
	osd_show(selmon, msg);
}

static void
gs_capture_finish(void)
{
	if (gs_capture_timer) {
		wl_event_source_remove(gs_capture_timer);
		gs_capture_timer = NULL;
	}
	if (gs_capture_reply) {
		if (gs_capture_sess)
			gs_reply_shortcuts(gs_capture_reply, gs_capture_sess);
		else
			sd_bus_reply_method_return(gs_capture_reply,
				"ua{sv}", 2, 0);
		sd_bus_message_unref(gs_capture_reply);
		gs_capture_reply = NULL;
	}
	gs_capture_sess = NULL;
	gs_capture_sc = NULL;
}

/* Advance to the next unbound shortcut after `from` (NULL = list head);
 * finish the deferred reply when none are left. */
static void
gs_capture_next(GSShortcut *from)
{
	struct wl_list *start = from ? &from->link
		: &gs_capture_sess->shortcuts;
	struct wl_list *it;

	for (it = start->next; it != &gs_capture_sess->shortcuts;
			it = it->next) {
		GSShortcut *sc = wl_container_of(it, sc, link);
		if (sc->keysym == XKB_KEY_NoSymbol) {
			gs_capture_sc = sc;
			gs_capture_prompt();
			if (gs_capture_timer)
				wl_event_source_timer_update(gs_capture_timer,
					30000);
			return;
		}
	}
	if (selmon)
		osd_show(selmon, "Hurtigtaster lagret");
	gs_capture_finish();
}

static int
gs_capture_timeout(void *data)
{
	(void)data;
	gs_capture_finish();
	return 0;
}

/* Called when a session dies while its BindShortcuts reply is parked. */
static void
gs_capture_cancel(GSSession *sess)
{
	if (gs_capture_sess != sess)
		return;
	gs_capture_sess = NULL; /* reply falls back to response=2 */
	gs_capture_finish();
}

static int
gs_bind_shortcuts(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *handle, *session_handle;
	GSSession *sess;
	GSShortcut *sc, *tmp;
	int r;

	(void)userdata; (void)err;
	r = sd_bus_message_read(m, "oo", &handle, &session_handle);
	if (r < 0)
		return r;
	sess = gs_session_find(session_handle);
	if (!sess) {
		sd_bus_message_skip(m, "a(sa{sv})sa{sv}");
		return sd_bus_reply_method_return(m, "ua{sv}", 2, 0);
	}

	/* A re-Bind while this session's previous Bind is still capturing
	 * would free the shortcut the capture points at — finish (and
	 * answer) the old call before replacing the list. */
	if (gs_capture_sess == sess)
		gs_capture_finish();

	wl_list_for_each(sc, &sess->shortcuts, link)
		if (sc->held)
			gs_emit_active(sess, sc, 0);
	wl_list_for_each_safe(sc, tmp, &sess->shortcuts, link)
		gs_shortcut_free(sc);

	r = sd_bus_message_enter_container(m, 'a', "(sa{sv})");
	if (r < 0)
		return sd_bus_reply_method_return(m, "ua{sv}", 2, 0);
	while (sd_bus_message_enter_container(m, 'r', "sa{sv}") > 0) {
		const char *id;
		char *desc = NULL, *pref = NULL;

		if (sd_bus_message_read(m, "s", &id) < 0)
			break;
		sd_bus_message_enter_container(m, 'a', "{sv}");
		while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
			const char *key, *val;
			sd_bus_message_read(m, "s", &key);
			if (sd_bus_message_enter_container(m, 'v', "s") > 0) {
				sd_bus_message_read(m, "s", &val);
				sd_bus_message_exit_container(m);
				if (strcmp(key, "description") == 0) {
					free(desc);
					desc = strdup(val);
				} else if (strcmp(key, "preferred_trigger") == 0) {
					free(pref);
					pref = strdup(val);
				}
			} else {
				sd_bus_message_skip(m, "v");
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m); /* a{sv} */
		sd_bus_message_exit_container(m); /* struct */

		sc = calloc(1, sizeof(*sc));
		if (sc) {
			sc->id = strdup(id);
			sc->description = desc;
			wl_list_insert(sess->shortcuts.prev, &sc->link);
			gs_resolve(sess, sc, pref);
			wlr_log(WLR_INFO, "gshortcuts: bind '%s' (%s) -> %s",
				sc->id, sess->app_id,
				sc->trigger ? sc->trigger : "(unbound)");
		} else {
			free(desc);
		}
		free(pref);
	}
	sd_bus_message_exit_container(m); /* a(sa{sv}) */

	/* Shortcuts the app couldn't name a trigger for (Discord sends
	 * none): capture interactively.  Park the reply — the frontend
	 * (and the app's "bind hotkey" spinner) waits until the user has
	 * pressed a combo per shortcut or the capture times out.  Only
	 * one capture at a time; a second concurrent Bind gets its
	 * unbound shortcuts left unbound (conf can still fill them). */
	if (!gs_capture_reply) {
		GSShortcut *unbound = NULL;
		wl_list_for_each(sc, &sess->shortcuts, link) {
			if (sc->keysym == XKB_KEY_NoSymbol) {
				unbound = sc;
				break;
			}
		}
		if (unbound) {
			gs_capture_reply = sd_bus_message_ref(m);
			gs_capture_sess = sess;
			gs_capture_timer = wl_event_loop_add_timer(event_loop,
				gs_capture_timeout, NULL);
			gs_capture_sc = unbound;
			gs_capture_prompt();
			if (gs_capture_timer)
				wl_event_source_timer_update(gs_capture_timer,
					30000);
			return 1; /* reply deferred */
		}
	}

	return gs_reply_shortcuts(m, sess);
}

static int
gs_list_shortcuts(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *handle, *session_handle;
	GSSession *sess;
	int r;

	(void)userdata; (void)err;
	r = sd_bus_message_read(m, "oo", &handle, &session_handle);
	if (r < 0)
		return r;
	sess = gs_session_find(session_handle);
	if (!sess)
		return sd_bus_reply_method_return(m, "ua{sv}", 2, 0);
	return gs_reply_shortcuts(m, sess);
}

static int
gs_prop_version(sd_bus *bus, const char *path, const char *iface,
	const char *prop, sd_bus_message *reply, void *userdata,
	sd_bus_error *err)
{
	(void)bus; (void)path; (void)iface; (void)prop;
	(void)userdata; (void)err;
	return sd_bus_message_append(reply, "u", 1u);
}

static const sd_bus_vtable gs_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}",
		gs_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("BindShortcuts", "ooa(sa{sv})sa{sv}", "ua{sv}",
		gs_bind_shortcuts, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ListShortcuts", "oo", "ua{sv}",
		gs_list_shortcuts, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("Activated", "osta{sv}", 0),
	SD_BUS_SIGNAL("Deactivated", "osta{sv}", 0),
	SD_BUS_SIGNAL("ShortcutsChanged", "oa(sa{sv})", 0),
	SD_BUS_PROPERTY("version", "u", gs_prop_version, 0,
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END
};

/* Frontend crash: its sessions would otherwise keep consuming keys
 * forever.  Drop every session owned by a vanished peer. */
static int
gs_name_owner_changed(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *name, *old_owner, *new_owner;
	GSSession *sess, *tmp;

	(void)userdata; (void)err;
	if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
		return 0;
	if (*new_owner != '\0' || *old_owner == '\0')
		return 0;
	wl_list_for_each_safe(sess, tmp, &gs_sessions, link)
		if (sess->peer && strcmp(sess->peer, name) == 0)
			gs_session_free(sess);
	return 0;
}

/* ── Key matching (called from keypress()) ───────────────────────── */

int
gshortcuts_handle_key(uint32_t mods, uint32_t keycode,
	const xkb_keysym_t *syms, int nsyms,
	const xkb_keysym_t *level0_syms, int nlevel0, int pressed)
{
	GSSession *sess;
	GSShortcut *sc;
	int consumed = 0;
	int i;

	if (!gs_bus)
		return 0;

	/* Capture mode: the next combo becomes the current shortcut's
	 * trigger.  All key events are swallowed so the recorder combo
	 * never leaks into the focused client. */
	if (gs_capture_reply && gs_capture_sess) {
		xkb_keysym_t sym;
		char trig[128];

		if (!pressed)
			return 1;
		sym = nlevel0 > 0 ? level0_syms[0]
			: (nsyms > 0 ? syms[0] : XKB_KEY_NoSymbol);
		if (sym == XKB_KEY_NoSymbol)
			return 1;
		/* Bare modifiers arm the combo, they don't end it. */
		if ((sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) ||
				(sym >= XKB_KEY_ISO_Lock &&
				 sym <= XKB_KEY_ISO_Level5_Lock))
			return 1;
		if (sym == XKB_KEY_Escape) {
			gs_capture_next(gs_capture_sc);
			return 1;
		}
		gs_capture_sc->mods = CLEANMASK(mods);
		gs_capture_sc->keysym = xkb_keysym_to_lower(sym);
		gs_trigger_string(gs_capture_sc->mods, sym, trig, sizeof(trig));
		free(gs_capture_sc->trigger);
		gs_capture_sc->trigger = strdup(trig);
		gs_conf_store(gs_capture_sess->app_id, gs_capture_sc->id, trig);
		wlr_log(WLR_INFO, "gshortcuts: captured '%s' (%s) -> %s",
			gs_capture_sc->id, gs_capture_sess->app_id, trig);
		gs_capture_next(gs_capture_sc);
		return 1;
	}

	if (!pressed) {
		wl_list_for_each(sess, &gs_sessions, link) {
			wl_list_for_each(sc, &sess->shortcuts, link) {
				if (sc->held && sc->held_keycode == keycode) {
					sc->held = 0;
					gs_emit_active(sess, sc, 0);
					consumed = 1;
				}
			}
		}
		return consumed;
	}

	wl_list_for_each(sess, &gs_sessions, link) {
		wl_list_for_each(sc, &sess->shortcuts, link) {
			if (sc->keysym == XKB_KEY_NoSymbol || sc->held)
				continue;
			if (CLEANMASK(mods) != CLEANMASK(sc->mods))
				continue;
			for (i = 0; i < nsyms + nlevel0; i++) {
				xkb_keysym_t sym = i < nsyms ? syms[i]
					: level0_syms[i - nsyms];
				if (xkb_keysym_to_lower(sym) == sc->keysym) {
					sc->held = 1;
					sc->held_keycode = keycode;
					gs_emit_active(sess, sc, 1);
					consumed = 1;
					break;
				}
			}
		}
	}
	return consumed;
}

/* ── shortcuts.conf hot-reload ───────────────────────────────────── */

static void
gs_conf_apply(void)
{
	GSSession *sess;
	GSShortcut *sc;

	wl_list_for_each(sess, &gs_sessions, link) {
		int changed = 0;
		wl_list_for_each(sc, &sess->shortcuts, link) {
			char *old = sc->trigger ? strdup(sc->trigger) : NULL;
			if (sc->held) {
				sc->held = 0;
				gs_emit_active(sess, sc, 0);
			}
			/* preferred_trigger is not kept — a conf reload can
			 * only rebind via the conf or unbind; the app rebinds
			 * with its preference on next BindShortcuts. */
			gs_resolve(sess, sc, old);
			if ((old == NULL) != (sc->trigger == NULL) ||
					(old && sc->trigger &&
					 strcmp(old, sc->trigger) != 0))
				changed = 1;
			free(old);
		}
		if (changed)
			gs_emit_changed(sess);
	}
}

static int
gsconf_inotify_event(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t len;
	int relevant = 0;

	(void)mask; (void)data;
	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		char *p = buf;
		while (p < buf + len) {
			struct inotify_event *ev = (struct inotify_event *)p;
			if (ev->len && strcmp(ev->name, GSCONF_NAME) == 0)
				relevant = 1;
			p += sizeof(*ev) + ev->len;
		}
	}
	if (relevant)
		gs_conf_apply();
	return 0;
}

/* ── Bus plumbing ────────────────────────────────────────────────── */

static int
gs_bus_event(int fd, uint32_t mask, void *data)
{
	sd_bus *bus = data;
	int r;

	(void)fd;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		wlr_log(WLR_ERROR, "gshortcuts: session bus hangup — disabling");
		if (gs_bus_source) {
			wl_event_source_remove(gs_bus_source);
			gs_bus_source = NULL;
		}
		return 0;
	}
	while ((r = sd_bus_process(bus, NULL)) > 0)
		;
	return 0;
}

void
gshortcuts_init(void)
{
	const char *home = getenv("HOME");
	int r, fd, wd;

	if (gs_bus)
		return;
	wl_list_init(&gs_sessions);

	if (!home) {
		struct passwd *pw = getpwuid(getuid());
		home = pw ? pw->pw_dir : "/";
	}
	snprintf(gsconf_dir, sizeof(gsconf_dir), "%s/.local/nixlyos", home);
	snprintf(gsconf_path, sizeof(gsconf_path),
		"%s/.local/nixlyos/" GSCONF_NAME, home);

	r = sd_bus_open_user(&gs_bus);
	if (r < 0) {
		wlr_log(WLR_ERROR, "gshortcuts: no session bus: %s",
			strerror(-r));
		gs_bus = NULL;
		return;
	}
	r = sd_bus_add_object_vtable(gs_bus, &gs_vtable_slot, GS_OBJ_PATH,
		GS_IFACE, gs_vtable, NULL);
	if (r < 0)
		goto fail;
	r = sd_bus_request_name(gs_bus, GS_BUS_NAME,
		SD_BUS_NAME_ALLOW_REPLACEMENT | SD_BUS_NAME_REPLACE_EXISTING);
	if (r < 0) {
		wlr_log(WLR_ERROR, "gshortcuts: cannot own %s: %s",
			GS_BUS_NAME, strerror(-r));
		goto fail;
	}
	sd_bus_add_match(gs_bus, &gs_name_slot,
		"type='signal',sender='org.freedesktop.DBus',"
		"path='/org/freedesktop/DBus',"
		"interface='org.freedesktop.DBus',member='NameOwnerChanged'",
		gs_name_owner_changed, NULL);

	fd = sd_bus_get_fd(gs_bus);
	if (fd < 0)
		goto fail;
	gs_bus_source = wl_event_loop_add_fd(event_loop, fd,
		WL_EVENT_READABLE, gs_bus_event, gs_bus);

	/* Same watch pattern as gaming_conf: dir-level inotify so the file
	 * can appear/rewrite atomically. */
	gsconf_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (gsconf_fd >= 0) {
		wd = inotify_add_watch(gsconf_fd, gsconf_dir,
			IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
		if (wd >= 0)
			gsconf_source = wl_event_loop_add_fd(event_loop,
				gsconf_fd, WL_EVENT_READABLE,
				gsconf_inotify_event, NULL);
	}

	wlr_log(WLR_INFO, "gshortcuts: GlobalShortcuts portal backend up (%s)",
		GS_BUS_NAME);
	return;
fail:
	if (gs_vtable_slot) {
		sd_bus_slot_unref(gs_vtable_slot);
		gs_vtable_slot = NULL;
	}
	sd_bus_unref(gs_bus);
	gs_bus = NULL;
}

void
gshortcuts_cleanup(void)
{
	GSSession *sess, *tmp;

	if (!gs_bus)
		return;
	wl_list_for_each_safe(sess, tmp, &gs_sessions, link)
		gs_session_free(sess);
	if (gsconf_source) {
		wl_event_source_remove(gsconf_source);
		gsconf_source = NULL;
	}
	if (gsconf_fd >= 0) {
		close(gsconf_fd);
		gsconf_fd = -1;
	}
	if (gs_bus_source) {
		wl_event_source_remove(gs_bus_source);
		gs_bus_source = NULL;
	}
	if (gs_name_slot) {
		sd_bus_slot_unref(gs_name_slot);
		gs_name_slot = NULL;
	}
	if (gs_vtable_slot) {
		sd_bus_slot_unref(gs_vtable_slot);
		gs_vtable_slot = NULL;
	}
	sd_bus_release_name(gs_bus, GS_BUS_NAME);
	sd_bus_unref(gs_bus);
	gs_bus = NULL;
}
