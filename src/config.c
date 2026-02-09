#include "nixlytile.h"
#include "client.h"

void
apply_startup_defaults(void)
{
	static int applied = 0;
	const double light_default = 40.0;
	const double speaker_default = 70.0;
	const double mic_default = 80.0;

	if (applied)
		return;

	/* Backlight */
	if (set_backlight_percent(light_default) == 0) {
		light_last_percent = light_default;
		light_cached_percent = light_default;
	} else if (light_cached_percent < 0.0) {
		light_cached_percent = light_default;
	}

	/*
	 * Speaker/headset volume - read actual state from PipeWire first,
	 * then unmute and set to default. This ensures statusbar shows
	 * correct state from the very beginning.
	 */
	{
		int is_headset = 0;
		double actual_vol;

		/* Invalidate cache to force fresh read from PipeWire */
		volume_invalidate_cache(0); /* speaker */
		volume_invalidate_cache(1); /* headset */

		/* Read actual current state from PipeWire */
		actual_vol = pipewire_volume_percent(&is_headset);
		(void)actual_vol; /* We just want the mute state synced */

		/* Now unmute and set volume */
		set_pipewire_mute(0);

		/* Update all mute state variables to match */
		volume_muted = 0;
		volume_cached_speaker_muted = 0;
		volume_cached_headset_muted = 0;

		/* Invalidate cache again so next read gets fresh data */
		volume_invalidate_cache(0);
		volume_invalidate_cache(1);
	}

	if (set_pipewire_volume(speaker_default) == 0) {
		speaker_active = speaker_default;
		speaker_stored = speaker_default;
		volume_last_speaker_percent = speaker_default;
		volume_last_headset_percent = speaker_default;
	}

	/*
	 * Microphone volume - read actual state from PipeWire first,
	 * then unmute and set to default.
	 */
	{
		double actual_mic;

		/* Invalidate cache to force fresh read */
		mic_last_read_ms = 0;

		/* Read actual current state from PipeWire */
		actual_mic = pipewire_mic_volume_percent();
		(void)actual_mic;

		/* Now unmute and set volume */
		set_pipewire_mic_mute(0);

		/* Update all mic mute state variables to match */
		mic_muted = 0;
		mic_cached_muted = 0;

		/* Invalidate cache again */
		mic_last_read_ms = 0;
	}

	if (set_pipewire_mic_volume(mic_default) == 0) {
		microphone_active = mic_default;
		microphone_stored = mic_default;
		mic_last_percent = mic_default;
	}

	applied = 1;
}

char *config_strdup(const char *s)
{
	size_t len = strlen(s);
	char *dup = malloc(len + 1);
	if (dup) memcpy(dup, s, len + 1);
	return dup;
}

void config_trim(char *s)
{
	char *start = s, *end;
	while (*start && isspace((unsigned char)*start)) start++;
	if (start != s) memmove(s, start, strlen(start) + 1);
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)*(end - 1))) end--;
	*end = '\0';
}

int config_parse_color(const char *value, float color[4])
{
	unsigned int hex;
	if (value[0] == '#') value++;
	else if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) value += 2;
	if (sscanf(value, "%x", &hex) != 1) return 0;
	/* Handle both #RRGGBB and #RRGGBBAA formats */
	if (strlen(value) <= 6) hex = (hex << 8) | 0xFF;
	color[0] = ((hex >> 24) & 0xFF) / 255.0f;
	color[1] = ((hex >> 16) & 0xFF) / 255.0f;
	color[2] = ((hex >> 8) & 0xFF) / 255.0f;
	color[3] = (hex & 0xFF) / 255.0f;
	return 1;
}

void config_expand_path(const char *src, char *dst, size_t dstlen)
{
	const char *home = getenv("HOME");
	if (src[0] == '~' && src[1] == '/') {
		snprintf(dst, dstlen, "%s%s", home ? home : "", src + 1);
	} else if (strncmp(src, "$HOME", 5) == 0) {
		snprintf(dst, dstlen, "%s%s", home ? home : "", src + 5);
	} else {
		snprintf(dst, dstlen, "%s", src);
	}
}

/* Modifier name-to-value lookup table */
static const struct {
	const char *name;
	unsigned int mod;
} mod_map[] = {
	{"super", WLR_MODIFIER_LOGO}, {"logo", WLR_MODIFIER_LOGO}, {"mod4", WLR_MODIFIER_LOGO},
	{"alt", WLR_MODIFIER_ALT}, {"mod1", WLR_MODIFIER_ALT},
	{"ctrl", WLR_MODIFIER_CTRL}, {"control", WLR_MODIFIER_CTRL},
	{"shift", WLR_MODIFIER_SHIFT},
};

static unsigned int
parse_modifier_value(const char *name)
{
	for (size_t i = 0; i < LENGTH(mod_map); i++) {
		if (strcasecmp(name, mod_map[i].name) == 0)
			return mod_map[i].mod;
	}
	return 0;
}

/* Config entry types for the lookup table */
enum ConfigType {
	CFG_INT,
	CFG_UINT,
	CFG_FLOAT,
	CFG_DOUBLE,
	CFG_COLOR,
	CFG_STR,
	CFG_CUSTOM,
};

typedef struct {
	const char *key;
	enum ConfigType type;
	void *ptr;
	size_t str_size;
	void (*custom)(const char *value);
} ConfigEntry;

static void config_set_font(const char *value)
{
	char *copy = config_strdup(value);
	char *saveptr, *tok;
	int i = 0;
	for (tok = strtok_r(copy, ",", &saveptr); tok && i < 7; tok = strtok_r(NULL, ",", &saveptr)) {
		config_trim(tok);
		if (*tok) runtime_fonts[i++] = config_strdup(tok);
	}
	runtime_fonts[i] = NULL;
	runtime_fonts_set = 1;
	free(copy);
}

static void config_set_modkey(const char *value)
{
	unsigned int m = parse_modifier_value(value);
	if (m) modkey = m;
}

static void config_set_monitorkey(const char *value)
{
	unsigned int m = parse_modifier_value(value);
	if (m) monitorkey = m;
}

static void config_set_htpc_wallpaper(const char *value)
{
	config_expand_path(value, htpc_wallpaper_path, sizeof(htpc_wallpaper_path));
}

static void config_set_client_download(const char *value)
{
	client_download_mbps = atoi(value);
	if (client_download_mbps < 1) client_download_mbps = 1;
	wlr_log(WLR_INFO, "Client download bandwidth: %d Mbps", client_download_mbps);
}

static void config_set_media_server(const char *value)
{
	if (*value) {
		char url[256];
		if (strncmp(value, "http://", 7) != 0 && strncmp(value, "https://", 8) != 0) {
			snprintf(url, sizeof(url), "http://%s", value);
		} else {
			strncpy(url, value, sizeof(url) - 1);
			url[sizeof(url) - 1] = '\0';
		}
		add_media_server(url, 0, 1);
	}
}

static void config_set_accel_profile(const char *value)
{
	if (strcmp(value, "flat") == 0) accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT;
	else if (strcmp(value, "adaptive") == 0) accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
}

static void config_set_scroll_method(const char *value)
{
	if (strcmp(value, "none") == 0) scroll_method = LIBINPUT_CONFIG_SCROLL_NO_SCROLL;
	else if (strcmp(value, "2fg") == 0) scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;
	else if (strcmp(value, "edge") == 0) scroll_method = LIBINPUT_CONFIG_SCROLL_EDGE;
	else if (strcmp(value, "button") == 0) scroll_method = LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
}

static void config_set_click_method(const char *value)
{
	if (strcmp(value, "none") == 0) click_method = LIBINPUT_CONFIG_CLICK_METHOD_NONE;
	else if (strcmp(value, "button_areas") == 0) click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;
	else if (strcmp(value, "clickfinger") == 0) click_method = LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
}

static void config_set_button_map(const char *value)
{
	if (strcmp(value, "lrm") == 0) button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;
	else if (strcmp(value, "lmr") == 0) button_map = LIBINPUT_CONFIG_TAP_MAP_LMR;
}

static const ConfigEntry config_entries[] = {
	/* General appearance */
	{"sloppyfocus",              CFG_INT,    &sloppyfocus,              0, NULL},
	{"bypass_surface_visibility",CFG_INT,    &bypass_surface_visibility,0, NULL},
	{"smartgaps",                CFG_INT,    &smartgaps,                0, NULL},
	{"gaps",                     CFG_INT,    &gaps,                     0, NULL},
	{"gappx",                    CFG_UINT,   &gappx,                   0, NULL},
	{"borderpx",                 CFG_UINT,   &borderpx,                0, NULL},
	{"lock_cursor",              CFG_INT,    &lock_cursor,              0, NULL},
	/* nixlytile mode */
	{"nixlytile_mode",           CFG_INT,    &nixlytile_mode,          0, NULL},
	{"htpc_wallpaper",           CFG_CUSTOM, NULL, 0, config_set_htpc_wallpaper},
	{"htpc_page_pcgaming",       CFG_INT,    &htpc_page_pcgaming,      0, NULL},
	{"htpc_page_retrogaming",    CFG_INT,    &htpc_page_retrogaming,   0, NULL},
	{"htpc_page_movies",         CFG_INT,    &htpc_page_movies,        0, NULL},
	{"htpc_page_tvshows",        CFG_INT,    &htpc_page_tvshows,       0, NULL},
	{"htpc_page_quit",           CFG_INT,    &htpc_page_quit,          0, NULL},
	/* Streaming */
	{"client_download_mbps",     CFG_CUSTOM, NULL, 0, config_set_client_download},
	{"media_server",             CFG_CUSTOM, NULL, 0, config_set_media_server},
	/* PC Gaming services */
	{"gaming_steam_enabled",     CFG_INT,    &gaming_service_enabled[GAMING_SERVICE_STEAM],   0, NULL},
	{"gaming_heroic_enabled",    CFG_INT,    &gaming_service_enabled[GAMING_SERVICE_HEROIC],  0, NULL},
	{"gaming_lutris_enabled",    CFG_INT,    &gaming_service_enabled[GAMING_SERVICE_LUTRIS],  0, NULL},
	{"gaming_bottles_enabled",   CFG_INT,    &gaming_service_enabled[GAMING_SERVICE_BOTTLES], 0, NULL},
	/* Colors */
	{"rootcolor",                CFG_COLOR,  rootcolor,                 0, NULL},
	{"bordercolor",              CFG_COLOR,  bordercolor,               0, NULL},
	{"focuscolor",               CFG_COLOR,  focuscolor,                0, NULL},
	{"urgentcolor",              CFG_COLOR,  urgentcolor,               0, NULL},
	{"fullscreen_bg",            CFG_COLOR,  fullscreen_bg,             0, NULL},
	/* Statusbar */
	{"statusbar_height",         CFG_UINT,   &statusbar_height,         0, NULL},
	{"statusbar_module_spacing", CFG_UINT,   &statusbar_module_spacing, 0, NULL},
	{"statusbar_module_padding", CFG_UINT,   &statusbar_module_padding, 0, NULL},
	{"statusbar_icon_text_gap",  CFG_UINT,   &statusbar_icon_text_gap,  0, NULL},
	{"statusbar_top_gap",        CFG_UINT,   &statusbar_top_gap,        0, NULL},
	{"statusbar_fg",             CFG_COLOR,  statusbar_fg,              0, NULL},
	{"statusbar_bg",             CFG_COLOR,  statusbar_bg,              0, NULL},
	{"statusbar_popup_bg",       CFG_COLOR,  statusbar_popup_bg,        0, NULL},
	{"statusbar_volume_muted_fg",CFG_COLOR,  statusbar_volume_muted_fg, 0, NULL},
	{"statusbar_mic_muted_fg",   CFG_COLOR,  statusbar_mic_muted_fg,    0, NULL},
	{"statusbar_tag_bg",         CFG_COLOR,  statusbar_tag_bg,          0, NULL},
	{"statusbar_tag_active_bg",  CFG_COLOR,  statusbar_tag_active_bg,   0, NULL},
	{"statusbar_tag_hover_bg",   CFG_COLOR,  statusbar_tag_hover_bg,    0, NULL},
	{"statusbar_hover_fade_ms",  CFG_INT,    &statusbar_hover_fade_ms,  0, NULL},
	{"statusbar_tray_force_rgba",CFG_INT,    &statusbar_tray_force_rgba,0, NULL},
	{"statusbar_font_spacing",   CFG_INT,    &statusbar_font_spacing,   0, NULL},
	{"statusbar_font_force_color",CFG_INT,   &statusbar_font_force_color,0, NULL},
	{"statusbar_workspace_padding",CFG_INT,  &statusbar_workspace_padding,0, NULL},
	{"statusbar_workspace_spacing",CFG_INT,  &statusbar_workspace_spacing,0, NULL},
	{"statusbar_thumb_height",   CFG_INT,    &statusbar_thumb_height,   0, NULL},
	{"statusbar_thumb_gap",      CFG_INT,    &statusbar_thumb_gap,      0, NULL},
	{"statusbar_thumb_window",   CFG_COLOR,  statusbar_thumb_window,    0, NULL},
	{"statusbar_font",           CFG_CUSTOM, NULL, 0, config_set_font},
	/* Keyboard */
	{"repeat_delay",             CFG_INT,    &repeat_delay,             0, NULL},
	{"repeat_rate",              CFG_INT,    &repeat_rate,              0, NULL},
	/* Trackpad */
	{"tap_to_click",             CFG_INT,    &tap_to_click,             0, NULL},
	{"tap_and_drag",             CFG_INT,    &tap_and_drag,             0, NULL},
	{"drag_lock",                CFG_INT,    &drag_lock,                0, NULL},
	{"natural_scrolling",        CFG_INT,    &natural_scrolling,        0, NULL},
	{"disable_while_typing",     CFG_INT,    &disable_while_typing,     0, NULL},
	{"left_handed",              CFG_INT,    &left_handed,              0, NULL},
	{"middle_button_emulation",  CFG_INT,    &middle_button_emulation,  0, NULL},
	{"accel_speed",              CFG_DOUBLE, &accel_speed,              0, NULL},
	{"accel_profile",            CFG_CUSTOM, NULL, 0, config_set_accel_profile},
	{"scroll_method",            CFG_CUSTOM, NULL, 0, config_set_scroll_method},
	{"click_method",             CFG_CUSTOM, NULL, 0, config_set_click_method},
	{"button_map",               CFG_CUSTOM, NULL, 0, config_set_button_map},
	/* Resizing */
	{"resize_factor",            CFG_FLOAT,  &resize_factor,            0, NULL},
	{"resize_interval_ms",       CFG_UINT,   &resize_interval_ms,       0, NULL},
	{"resize_min_pixels",        CFG_DOUBLE, &resize_min_pixels,        0, NULL},
	{"resize_ratio_epsilon",     CFG_FLOAT,  &resize_ratio_epsilon,     0, NULL},
	{"modal_file_search_minlen", CFG_INT,    &modal_file_search_minlen, 0, NULL},
	/* Wallpaper and autostart */
	{"wallpaper",                CFG_STR,    wallpaper_path,            sizeof(wallpaper_path), NULL},
	{"autostart",                CFG_STR,    autostart_cmd,             sizeof(autostart_cmd), NULL},
	/* Modifier keys */
	{"modkey",                   CFG_CUSTOM, NULL, 0, config_set_modkey},
	{"monitorkey",               CFG_CUSTOM, NULL, 0, config_set_monitorkey},
	/* Spawn commands */
	{"terminal",                 CFG_STR,    spawn_cmd_terminal,        sizeof(spawn_cmd_terminal), NULL},
	{"terminal_alt",             CFG_STR,    spawn_cmd_terminal_alt,    sizeof(spawn_cmd_terminal_alt), NULL},
	{"browser",                  CFG_STR,    spawn_cmd_browser,         sizeof(spawn_cmd_browser), NULL},
	{"filemanager",              CFG_STR,    spawn_cmd_filemanager,     sizeof(spawn_cmd_filemanager), NULL},
	{"launcher",                 CFG_STR,    spawn_cmd_launcher,        sizeof(spawn_cmd_launcher), NULL},
};

void config_set_value(const char *key, const char *value)
{
	for (size_t i = 0; i < LENGTH(config_entries); i++) {
		const ConfigEntry *e = &config_entries[i];
		if (strcmp(key, e->key) != 0)
			continue;

		switch (e->type) {
		case CFG_INT:
			*(int *)e->ptr = atoi(value);
			break;
		case CFG_UINT:
			*(unsigned int *)e->ptr = (unsigned int)atoi(value);
			break;
		case CFG_FLOAT:
			*(float *)e->ptr = (float)atof(value);
			break;
		case CFG_DOUBLE:
			*(double *)e->ptr = atof(value);
			break;
		case CFG_COLOR:
			config_parse_color(value, (float *)e->ptr);
			break;
		case CFG_STR:
			snprintf((char *)e->ptr, e->str_size, "%s", value);
			break;
		case CFG_CUSTOM:
			if (e->custom) e->custom(value);
			break;
		}
		return;
	}
}

unsigned int config_parse_modifiers(const char *mods_str)
{
	unsigned int mods = 0;
	char *copy = config_strdup(mods_str);
	char *saveptr, *tok;

	for (tok = strtok_r(copy, "+", &saveptr); tok; tok = strtok_r(NULL, "+", &saveptr)) {
		unsigned int m;
		config_trim(tok);
		if (strcasecmp(tok, "mod") == 0)
			mods |= modkey;
		else if ((m = parse_modifier_value(tok)) != 0)
			mods |= m;
	}
	free(copy);
	return mods;
}

int config_parse_binding(const char *line)
{
	char mods_key[128], action[128], arg_str[256];
	char *plus, *mods_str, *key_str;
	unsigned int mods;
	xkb_keysym_t keysym;
	const FuncEntry *fe;
	Key *k;
	int n;

	if (runtime_keys_count >= MAX_KEYS) return 0;

	/* Parse: mods+key action [arg] */
	arg_str[0] = '\0';
	n = sscanf(line, "%127s %127s %255[^\n]", mods_key, action, arg_str);
	if (n < 2) return 0;

	/* Split mods+key */
	plus = strrchr(mods_key, '+');
	if (plus) {
		*plus = '\0';
		mods_str = mods_key;
		key_str = plus + 1;
	} else {
		mods_str = "";
		key_str = mods_key;
	}

	/* Parse modifiers */
	mods = config_parse_modifiers(mods_str);

	/* Parse key */
	keysym = config_parse_keysym(key_str);
	if (keysym == XKB_KEY_NoSymbol) {
		wlr_log(WLR_ERROR, "Unknown key: %s", key_str);
		return 0;
	}

	/* Find function */
	for (fe = func_table; fe->name; fe++) {
		if (strcasecmp(action, fe->name) == 0) break;
	}
	if (!fe->name) {
		wlr_log(WLR_ERROR, "Unknown action: %s", action);
		return 0;
	}

	/* Add keybinding */
	k = &runtime_keys[runtime_keys_count];
	k->mod = mods;
	k->keysym = keysym;
	k->func = fe->func;

	/* Parse argument - cast away const to set the value */
	config_trim(arg_str);
	{
		Arg *argp = (Arg *)&k->arg;
		switch (fe->arg_type) {
		case 0: /* none */
			argp->i = 0;
			break;
		case 1: /* int */
			argp->i = atoi(arg_str);
			break;
		case 2: /* uint */
			if (strcmp(arg_str, "all") == 0 || strcmp(arg_str, "~0") == 0)
				argp->ui = ~0u;
			else if (strcasecmp(arg_str, "up") == 0)
				argp->ui = DIR_UP;
			else if (strcasecmp(arg_str, "down") == 0)
				argp->ui = DIR_DOWN;
			else if (strcasecmp(arg_str, "left") == 0)
				argp->ui = DIR_LEFT;
			else if (strcasecmp(arg_str, "right") == 0)
				argp->ui = DIR_RIGHT;
			else
				argp->ui = (unsigned int)atoi(arg_str);
			break;
		case 3: /* float */
			argp->f = (float)atof(arg_str);
			break;
		case 4: /* spawn */
			if (runtime_spawn_cmd_count < MAX_KEYS) {
				runtime_spawn_cmds[runtime_spawn_cmd_count] = config_strdup(arg_str);
				argp->v = runtime_spawn_cmds[runtime_spawn_cmd_count];
				runtime_spawn_cmd_count++;
			}
			break;
		}
	}

	runtime_keys_count++;
	return 1;
}

int config_parse_transform(const char *str)
{
	static const struct { const char *name; int transform; } transform_map[] = {
		{"normal",      WL_OUTPUT_TRANSFORM_NORMAL},
		{"0",           WL_OUTPUT_TRANSFORM_NORMAL},
		{"90",          WL_OUTPUT_TRANSFORM_90},
		{"rotate-90",   WL_OUTPUT_TRANSFORM_90},
		{"180",         WL_OUTPUT_TRANSFORM_180},
		{"rotate-180",  WL_OUTPUT_TRANSFORM_180},
		{"270",         WL_OUTPUT_TRANSFORM_270},
		{"rotate-270",  WL_OUTPUT_TRANSFORM_270},
		{"flipped",     WL_OUTPUT_TRANSFORM_FLIPPED},
		{"flipped-90",  WL_OUTPUT_TRANSFORM_FLIPPED_90},
		{"flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180},
		{"flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270},
	};

	if (!str || !*str)
		return WL_OUTPUT_TRANSFORM_NORMAL;

	for (size_t i = 0; i < LENGTH(transform_map); i++) {
		if (strcasecmp(str, transform_map[i].name) == 0)
			return transform_map[i].transform;
	}
	return WL_OUTPUT_TRANSFORM_NORMAL;
}

int config_parse_monitor(const char *line)
{
	RuntimeMonitorConfig *mon;
	char name[64], pos_str[32], rest[256];
	char *token, *saveptr, *rest_copy;
	int n;

	if (runtime_monitor_count >= MAX_MONITORS) {
		wlr_log(WLR_ERROR, "Too many monitor configurations (max %d)", MAX_MONITORS);
		return 0;
	}

	/* Parse: name position [rest...] */
	rest[0] = '\0';
	n = sscanf(line, "%63s %31s %255[^\n]", name, pos_str, rest);
	if (n < 2) {
		wlr_log(WLR_ERROR, "Invalid monitor config: %s", line);
		return 0;
	}

	mon = &runtime_monitors[runtime_monitor_count];
	memset(mon, 0, sizeof(*mon));
	strncpy(mon->name, name, sizeof(mon->name) - 1);
	mon->position = config_parse_monitor_position(pos_str);
	mon->width = 0;      /* 0 = auto */
	mon->height = 0;
	mon->refresh = 0;    /* 0 = auto */
	mon->scale = 1.0f;
	mon->mfact = 0.55f;
	mon->nmaster = 1;
	mon->enabled = 1;
	mon->transform = WL_OUTPUT_TRANSFORM_NORMAL;

	if (mon->position == MON_POS_MASTER)
		monitor_master_set = 1;

	/* Parse optional parameters from rest */
	if (rest[0]) {
		rest_copy = config_strdup(rest);
		if (rest_copy) {
			for (token = strtok_r(rest_copy, " \t", &saveptr); token; token = strtok_r(NULL, " \t", &saveptr)) {
				/* Check for WxH or WxH@Hz format */
				int w, h;
				float hz;
				if (sscanf(token, "%dx%d@%f", &w, &h, &hz) == 3) {
					mon->width = w;
					mon->height = h;
					mon->refresh = hz;
				} else if (sscanf(token, "%dx%d", &w, &h) == 2) {
					mon->width = w;
					mon->height = h;
				} else if (strncasecmp(token, "scale=", 6) == 0) {
					mon->scale = (float)atof(token + 6);
					if (mon->scale < 0.5f) mon->scale = 0.5f;
					if (mon->scale > 4.0f) mon->scale = 4.0f;
				} else if (strncasecmp(token, "transform=", 10) == 0) {
					mon->transform = config_parse_transform(token + 10);
				} else if (strncasecmp(token, "mfact=", 6) == 0) {
					mon->mfact = (float)atof(token + 6);
					if (mon->mfact < 0.1f) mon->mfact = 0.1f;
					if (mon->mfact > 0.9f) mon->mfact = 0.9f;
				} else if (strncasecmp(token, "nmaster=", 8) == 0) {
					mon->nmaster = atoi(token + 8);
					if (mon->nmaster < 1) mon->nmaster = 1;
				} else if (strcasecmp(token, "disabled") == 0 || strcasecmp(token, "off") == 0) {
					mon->enabled = 0;
				}
			}
			free(rest_copy);
		}
	}

	wlr_log(WLR_INFO, "Monitor config: %s at %s (%dx%d@%.2f scale=%.2f)",
		mon->name,
		pos_str,
		mon->width, mon->height, mon->refresh, mon->scale);

	runtime_monitor_count++;
	return 1;
}

int get_connector_priority(const char *name)
{
	static const struct { const char *prefix; int len; int priority; } connector_prio[] = {
		{"HDMI", 4, 1}, {"DP-", 3, 2}, {"eDP", 3, 3}, {"VGA", 3, 4}, {"DVI", 3, 5},
	};

	for (size_t i = 0; i < LENGTH(connector_prio); i++) {
		if (strncmp(name, connector_prio[i].prefix, connector_prio[i].len) == 0)
			return connector_prio[i].priority;
	}
	return 10;
}

void calculate_monitor_position(Monitor *m, RuntimeMonitorConfig *cfg, int *out_x, int *out_y)
{
	Monitor *other;
	int master_x = 0, master_y = 0, master_w = 1920, master_h = 1080;
	int left_x = 0, left_w = 1920;
	int right_x = 0;

	/* Find master monitor dimensions for positioning */
	wl_list_for_each(other, &mons, link) {
		RuntimeMonitorConfig *other_cfg = find_monitor_config(other->wlr_output->name);
		if (other_cfg && other_cfg->position == MON_POS_MASTER) {
			master_x = other->m.x;
			master_y = other->m.y;
			master_w = other->m.width > 0 ? other->m.width : 1920;
			master_h = other->m.height > 0 ? other->m.height : 1080;
			break;
		}
	}

	/* Find left monitor for top-left positioning */
	wl_list_for_each(other, &mons, link) {
		RuntimeMonitorConfig *other_cfg = find_monitor_config(other->wlr_output->name);
		if (other_cfg && other_cfg->position == MON_POS_LEFT) {
			left_x = other->m.x;
			left_w = other->m.width > 0 ? other->m.width : 1920;
			break;
		}
	}

	/* Find right monitor x position for top-right positioning */
	wl_list_for_each(other, &mons, link) {
		RuntimeMonitorConfig *other_cfg = find_monitor_config(other->wlr_output->name);
		if (other_cfg && other_cfg->position == MON_POS_RIGHT) {
			right_x = other->m.x;
			break;
		}
	}

	/* Get this monitor's dimensions */
	int my_w = m->m.width > 0 ? m->m.width : (cfg->width > 0 ? cfg->width : 1920);
	int my_h = m->m.height > 0 ? m->m.height : (cfg->height > 0 ? cfg->height : 1080);

	switch (cfg->position) {
	case MON_POS_MASTER:
		*out_x = 0;
		*out_y = 0;
		break;
	case MON_POS_LEFT:
		*out_x = master_x - my_w;
		*out_y = master_y;
		break;
	case MON_POS_RIGHT:
		*out_x = master_x + master_w;
		*out_y = master_y;
		break;
	case MON_POS_TOP_LEFT:
		*out_x = left_x;
		*out_y = master_y - my_h;
		break;
	case MON_POS_TOP_RIGHT:
		*out_x = right_x > 0 ? right_x : (master_x + master_w);
		*out_y = master_y - my_h;
		break;
	case MON_POS_BOTTOM_LEFT:
		*out_x = left_x;
		*out_y = master_y + master_h;
		break;
	case MON_POS_BOTTOM_RIGHT:
		*out_x = right_x > 0 ? right_x : (master_x + master_w);
		*out_y = master_y + master_h;
		break;
	case MON_POS_AUTO:
	default:
		*out_x = -1;  /* Auto-placement */
		*out_y = -1;
		break;
	}
}

void
load_config(void)
{
	const char *home = getenv("HOME");
	const char *config_home = getenv("XDG_CONFIG_HOME");
	char config_path[PATH_MAX];
	char fallback_path[PATH_MAX];
	FILE *f;
	char line[1024];
	const char *xdg_data_dirs;
	char *data_dirs_copy, *dir, *saveptr;
	int found_fallback = 0;

	/* Try XDG_CONFIG_HOME first, then ~/.config */
	if (config_home && *config_home) {
		snprintf(config_path, sizeof(config_path), "%s/nixlytile/config.conf", config_home);
	} else if (home) {
		snprintf(config_path, sizeof(config_path), "%s/.config/nixlytile/config.conf", home);
	} else {
		return; /* No home directory, use defaults */
	}

	/* Cache the config path for hot-reload watching */
	strncpy(config_path_cached, config_path, sizeof(config_path_cached) - 1);

	f = fopen(config_path, "r");
	if (!f) {
		/* Try to find config.conf.example in XDG_DATA_DIRS */
		xdg_data_dirs = getenv("XDG_DATA_DIRS");
		if (xdg_data_dirs && *xdg_data_dirs) {
			data_dirs_copy = strdup(xdg_data_dirs);
			if (data_dirs_copy) {
				for (dir = strtok_r(data_dirs_copy, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
					snprintf(fallback_path, sizeof(fallback_path), "%s/nixlytile/config.conf.example", dir);
					f = fopen(fallback_path, "r");
					if (f) {
						found_fallback = 1;
						strncpy(config_path_cached, fallback_path, sizeof(config_path_cached) - 1);
						wlr_log(WLR_INFO, "Using fallback config from %s", fallback_path);
						break;
					}
				}
				free(data_dirs_copy);
			}
		}
		if (!found_fallback) {
			wlr_log(WLR_INFO, "No config file found at %s, using defaults", config_path);
			return;
		}
	} else {
		wlr_log(WLR_INFO, "Loading config from %s", config_path);
	}

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		char *key, *value, *eq;

		/* Skip leading whitespace */
		while (*p && isspace((unsigned char)*p)) p++;

		/* Skip empty lines and comments */
		if (!*p || *p == '#' || *p == ';') continue;

		/* Remove trailing newline/whitespace */
		config_trim(p);

		/* Find '=' separator */
		eq = strchr(p, '=');
		if (!eq) continue;

		*eq = '\0';
		key = p;
		value = eq + 1;

		config_trim(key);
		config_trim(value);

		/* Remove quotes from value if present */
		if (*value == '"' || *value == '\'') {
			char quote = *value++;
			char *end = strrchr(value, quote);
			if (end) *end = '\0';
		}

		/* Handle special config types */
		if (strcmp(key, "bind") == 0) {
			config_parse_binding(value);
		} else if (strcmp(key, "monitor") == 0) {
			config_parse_monitor(value);
		} else if (*key && *value) {
			config_set_value(key, value);
		}
	}

	fclose(f);
}

void
init_keybindings(void)
{
	if (runtime_keys_count > 0) {
		keys = runtime_keys;
		keys_count = runtime_keys_count;
		wlr_log(WLR_INFO, "Using %zu custom keybindings from config", keys_count);
	} else {
		keys = default_keys;
		keys_count = default_keys_count;
		wlr_log(WLR_INFO, "Using %zu default keybindings", keys_count);
	}
}

void
reset_runtime_config(void)
{
	int i;

	/* Free allocated spawn commands */
	for (i = 0; i < runtime_spawn_cmd_count; i++) {
		if (runtime_spawn_cmds[i]) {
			free(runtime_spawn_cmds[i]);
			runtime_spawn_cmds[i] = NULL;
		}
	}
	runtime_spawn_cmd_count = 0;

	/* Reset keybindings */
	runtime_keys_count = 0;
	memset(runtime_keys, 0, sizeof(runtime_keys));

	/* Reset monitor config */
	runtime_monitor_count = 0;
	monitor_master_set = 0;
	memset(runtime_monitors, 0, sizeof(runtime_monitors));
}

void
reload_config(void)
{
	Monitor *m;
	int i;

	wlr_log(WLR_INFO, "Hot-reloading config file...");

	/* Reset runtime state */
	reset_runtime_config();

	/* Re-load config */
	load_config();

	/* Re-initialize keybindings */
	init_keybindings();

	/* Update visual settings on all monitors */
	wl_list_for_each(m, &mons, link) {
		/* Update gaps setting */
		m->gaps = gaps;

		/* Update border width on all clients */
		Client *c;
		wl_list_for_each(c, &clients, link) {
			if (c->mon == m && !c->isfullscreen)
				c->bw = borderpx;
		}

		/* Re-arrange with new settings (this also updates statusbar) */
		arrange(m);
	}

	/* Update root background color */
	if (root_bg)
		wlr_scene_rect_set_color(root_bg, rootcolor);

	/* Update locked background if exists */
	if (locked_bg)
		wlr_scene_rect_set_color(locked_bg, (float[]){0.1f, 0.1f, 0.1f, 1.0f});

	/* Reload font if needed */
	if (statusfont.font) {
		struct StatusFont new_font = {0};
		if (loadstatusfont()) {
			/* Font loaded successfully, update tray icons */
			tray_update_icons_text();
		}
		(void)new_font;
	}

	/* Force redraw of all monitors */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output)
			wlr_output_schedule_frame(m->wlr_output);
	}

	wlr_log(WLR_INFO, "Config reload complete: %zu keybindings, %d monitors configured",
		keys_count, runtime_monitor_count);
}

int
config_rewatch_timer_cb(void *data)
{
	(void)data;

	if (config_needs_rewatch && config_path_cached[0]) {
		config_watch_wd = inotify_add_watch(config_inotify_fd,
			config_path_cached, IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);
		config_needs_rewatch = 0;
		reload_config();
	}

	return 0;
}

int
config_watch_handler(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	const struct inotify_event *event;
	ssize_t len;
	char *ptr;
	int should_reload = 0;

	(void)mask;
	(void)data;

	while ((len = read(fd, buf, sizeof(buf))) > 0) {
		for (ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
			event = (const struct inotify_event *)ptr;

			/* Check for modify, close_write, or move events */
			if (event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO)) {
				should_reload = 1;
			}

			/* If the file was deleted or moved away, re-add watch */
			if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
				if (config_watch_wd >= 0) {
					inotify_rm_watch(config_inotify_fd, config_watch_wd);
					config_watch_wd = -1;
				}
				/* Schedule timer to re-add watch (non-blocking) */
				config_needs_rewatch = 1;
				if (config_rewatch_timer)
					wl_event_source_timer_update(config_rewatch_timer, 100); /* 100ms */
				/* Don't reload now, timer will do it */
				continue;
			}
		}
	}

	if (should_reload)
		reload_config();

	return 0;
}

void
setup_config_watch(void)
{
	char dir_path[PATH_MAX];
	char *slash;

	if (!config_path_cached[0]) {
		wlr_log(WLR_INFO, "No config path to watch");
		return;
	}

	/* Create inotify instance */
	config_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (config_inotify_fd < 0) {
		wlr_log(WLR_ERROR, "Failed to create inotify instance: %s", strerror(errno));
		return;
	}

	/* Watch the config file for changes */
	config_watch_wd = inotify_add_watch(config_inotify_fd, config_path_cached,
		IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF);

	if (config_watch_wd < 0) {
		/* File doesn't exist yet, watch the directory instead */
		strncpy(dir_path, config_path_cached, sizeof(dir_path) - 1);
		slash = strrchr(dir_path, '/');
		if (slash) {
			*slash = '\0';
			/* Create directory if it doesn't exist */
			mkdir(dir_path, 0755);
			config_watch_wd = inotify_add_watch(config_inotify_fd, dir_path,
				IN_CREATE | IN_MOVED_TO);
		}
		if (config_watch_wd < 0) {
			wlr_log(WLR_ERROR, "Failed to watch config: %s", strerror(errno));
			close(config_inotify_fd);
			config_inotify_fd = -1;
			return;
		}
		wlr_log(WLR_INFO, "Watching config directory for file creation: %s", dir_path);
	} else {
		wlr_log(WLR_INFO, "Watching config file for changes: %s", config_path_cached);
	}

	/* Add to event loop */
	config_watch_source = wl_event_loop_add_fd(event_loop, config_inotify_fd,
		WL_EVENT_READABLE, config_watch_handler, NULL);

	/* Create timer for delayed re-watch */
	config_rewatch_timer = wl_event_loop_add_timer(event_loop, config_rewatch_timer_cb, NULL);
	if (!config_watch_source) {
		wlr_log(WLR_ERROR, "Failed to add config watcher to event loop");
		close(config_inotify_fd);
		config_inotify_fd = -1;
	}
}

