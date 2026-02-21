/* nixlytile.c - Core compositor: setup, run, cleanup, main */
#include "nixlytile.h"
#include "client.h"


int
has_nixlytile_session_target(void)
{
	const char *paths[] = {
		"/etc/systemd/user/nixlytile-session.target",
		"/usr/lib/systemd/user/nixlytile-session.target",
		"/lib/systemd/user/nixlytile-session.target",
		NULL
	};

	if (has_nixlytile_session_target_cached >= 0)
		return has_nixlytile_session_target_cached;

	for (int i = 0; paths[i]; i++) {
		if (access(paths[i], F_OK) == 0) {
			has_nixlytile_session_target_cached = 1;
			return 1;
		}
	}
	has_nixlytile_session_target_cached = 0;
	return 0;
}


/* function implementations */







/* Forward declarations for pixman buffer impl (defined in draw.c) */
void pixman_buffer_destroy(struct wlr_buffer *wlr_buffer);
bool pixman_buffer_get_dmabuf(struct wlr_buffer *wlr_buffer, struct wlr_dmabuf_attributes *attribs);
bool pixman_buffer_get_shm(struct wlr_buffer *wlr_buffer, struct wlr_shm_attributes *attribs);
bool pixman_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags, void **data, uint32_t *format, size_t *stride);
void pixman_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer);

const struct wlr_buffer_impl pixman_buffer_impl = {
	.destroy = pixman_buffer_destroy,
	.get_dmabuf = pixman_buffer_get_dmabuf,
	.get_shm = pixman_buffer_get_shm,
	.begin_data_ptr_access = pixman_buffer_begin_data_ptr_access,
	.end_data_ptr_access = pixman_buffer_end_data_ptr_access,
};













































































/* RAM Popup Functions */









/* Forward declaration for readulong (defined later) */




















/* Find Discord client - checks multiple possible app_id variations.
 * Discord on NixOS/Linux can have various app_ids like:
 * "discord", "Discord", ".Discord-wrapped", "vesktop", etc. */




















uint64_t
monotonic_msec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}





























/* Sudo password popup functions */









/* ==================== ON-SCREEN KEYBOARD ==================== */

/* Keyboard layout - Norwegian QWERTY with special characters */
const char *osk_layout_lower[OSK_ROWS][OSK_COLS] = {
	{"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "+", "\\"},
	{"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "å", "¨"},
	{"a", "s", "d", "f", "g", "h", "j", "k", "l", "ø", "æ", "'"},
	{"<", "z", "x", "c", "v", "b", "n", "m", ",", ".", "-", "⌫"},
	{"⇧", "@", "#", " ", " ", " ", " ", " ", "?", "!", "↵", "↵"},
};

const char *osk_layout_upper[OSK_ROWS][OSK_COLS] = {
	{"!", "\"", "#", "¤", "%", "&", "/", "(", ")", "=", "?", "`"},
	{"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "Å", "^"},
	{"A", "S", "D", "F", "G", "H", "J", "K", "L", "Ø", "Æ", "*"},
	{">", "Z", "X", "C", "V", "B", "N", "M", ";", ":", "_", "⌫"},
	{"⇧", "@", "#", " ", " ", " ", " ", " ", "?", "!", "↵", "↵"},
};






/* Send a Unicode character to the focused surface via wlr_seat */




/* OSK d-pad repeat timer callback - called repeatedly while d-pad is held */

/* Start OSK d-pad repeat for a direction */

/* Stop OSK d-pad repeat */



/* Transfer visible status menus to target monitor when mouse moves */











VpnConnection *
vpn_connection_at_index(int idx)
{
	VpnConnection *v;
	int i = 0;
	wl_list_for_each(v, &vpn_connections, link) {
		if (i == idx)
			return v;
		i++;
	}
	return NULL;
}













































/*
 * Game refocus timer - re-focuses game clients after a short delay.
 * This fixes keyboard input not working immediately for XWayland games
 * (like Witcher 3 via Steam/Proton) which need time to fully initialize
 * their input handling after the window maps.
 */




WifiNetwork *
wifi_network_at_index(int idx)
{
	WifiNetwork *n;
	int i = 0;
	wl_list_for_each(n, &wifi_networks, link) {
		if (i == idx)
			return n;
		i++;
	}
	return NULL;
}




















uint32_t
random_status_delay_ms(void)
{
	seed_status_rng();
	return 30000u + (uint32_t)(rand() % 30001);
}


















/* Debounce timer callback - starts search after typing pause */

/* Schedule a debounced file search (150ms delay) */


/* Git project search functions */














/* Check if any client is currently fullscreen AND visible, return the client if found */

/* Check if any client is currently fullscreen */

/*
 * Get the PID of a Wayland client.
 * Returns 0 if the PID cannot be determined.
 */

/*
 * Apply high-priority scheduling to a game process for optimal performance.
 *
 * This sets:
 * 1. Nice value to -5 (higher CPU priority than normal processes)
 * 2. I/O priority to real-time class (faster disk access)
 * 3. SCHED_RR for compositor thread (optional, requires CAP_SYS_NICE)
 *
 * These optimizations reduce latency and ensure the game gets CPU time
 * when it needs it, similar to what gamemode daemon does.
 */

/*
 * Restore normal priority for a process after game mode ends.
 */

/*
 * Check system memory pressure for performance monitoring.
 * Returns a value 0-100 indicating memory pressure (0=no pressure, 100=critical).
 */

/*
 * Ultra Game Mode - Maximum performance when fullscreen game detected.
 *
 * This is an aggressive optimization mode that minimizes ALL compositor
 * overhead when a game is running fullscreen:
 *
 * 1. Stops ALL background timers (cache updates, status polling, wifi scans, etc.)
 * 2. Hides the statusbar completely
 * 3. Hides all popups and menus
 * 4. Disables compositor animations
 * 5. Direct scanout is automatically activated by wlroots when conditions are met
 * 6. Tearing is enabled for games that request it (lowest latency)
 *
 * The goal is minimal latency path - the game's buffer goes directly to display
 * without any compositor processing or system interrupts from background tasks.
 */

/* Set GE-Proton as the default compatibility tool in Steam */
void
steam_set_ge_proton_default(void)
{
	char compat_dir[PATH_MAX];
	char config_path[PATH_MAX];
	char ge_proton_name[128] = {0};
	int best_major = 0, best_minor = 0;
	const char *home = getenv("HOME");
	DIR *dir;
	struct dirent *entry;
	FILE *fp;
	char *config_content = NULL;
	size_t config_size = 0;

	if (!home)
		return;

	/* Find newest GE-Proton in compatibilitytools.d */
	snprintf(compat_dir, sizeof(compat_dir), "%s/.steam/root/compatibilitytools.d", home);
	dir = opendir(compat_dir);
	if (!dir) {
		snprintf(compat_dir, sizeof(compat_dir), "%s/.local/share/Steam/compatibilitytools.d", home);
		dir = opendir(compat_dir);
	}

	if (dir) {
		while ((entry = readdir(dir)) != NULL) {
			if (strncmp(entry->d_name, "GE-Proton", 9) == 0) {
				int major = 0, minor = 0;
				/* Parse GE-ProtonX-Y format */
				if (sscanf(entry->d_name, "GE-Proton%d-%d", &major, &minor) >= 1) {
					if (major > best_major || (major == best_major && minor > best_minor)) {
						best_major = major;
						best_minor = minor;
						snprintf(ge_proton_name, sizeof(ge_proton_name), "%s", entry->d_name);
					}
				}
			}
		}
		closedir(dir);
	}

	if (!ge_proton_name[0]) {
		wlr_log(WLR_INFO, "No GE-Proton found in compatibilitytools.d");
		return;
	}

	wlr_log(WLR_INFO, "Setting %s as default Steam compatibility tool", ge_proton_name);

	/* Read current config.vdf */
	snprintf(config_path, sizeof(config_path), "%s/.steam/steam/config/config.vdf", home);
	fp = fopen(config_path, "r");
	if (!fp) {
		snprintf(config_path, sizeof(config_path), "%s/.local/share/Steam/config/config.vdf", home);
		fp = fopen(config_path, "r");
	}

	if (!fp) {
		wlr_log(WLR_ERROR, "Could not open Steam config.vdf");
		return;
	}

	/* Read entire file */
	fseek(fp, 0, SEEK_END);
	config_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	config_content = malloc(config_size + 4096); /* Extra space for modifications */
	if (!config_content) {
		fclose(fp);
		return;
	}
	fread(config_content, 1, config_size, fp);
	config_content[config_size] = '\0';
	fclose(fp);

	/* Check if CompatToolMapping already exists with our GE-Proton */
	if (strstr(config_content, ge_proton_name)) {
		wlr_log(WLR_INFO, "%s already configured in Steam", ge_proton_name);
		free(config_content);
		return;
	}

	/* Find CompatToolMapping section or create it */
	char *compat_section = strstr(config_content, "\"CompatToolMapping\"");
	if (compat_section) {
		/* Find the "0" entry (global default) within CompatToolMapping */
		char *zero_entry = strstr(compat_section, "\"0\"");
		if (zero_entry && zero_entry < compat_section + 500) {
			/* Replace existing "0" entry - find its closing brace */
			char *entry_start = zero_entry;
			char *brace_open = strchr(entry_start, '{');
			if (brace_open) {
				int depth = 1;
				char *brace_close = brace_open + 1;
				while (*brace_close && depth > 0) {
					if (*brace_close == '{') depth++;
					else if (*brace_close == '}') depth--;
					brace_close++;
				}
				/* Replace content between braces */
				char new_entry[512];
				snprintf(new_entry, sizeof(new_entry),
					"{\n\t\t\t\t\t\t\"name\"\t\t\"%s\"\n"
					"\t\t\t\t\t\t\"config\"\t\t\"\"\n"
					"\t\t\t\t\t\t\"priority\"\t\t\"75\"\n"
					"\t\t\t\t\t}",
					ge_proton_name);

				/* Build new config */
				size_t prefix_len = brace_open - config_content + 1;
				size_t suffix_start = brace_close - config_content;
				char *new_config = malloc(config_size + 4096);
				if (new_config) {
					memcpy(new_config, config_content, prefix_len);
					strcpy(new_config + prefix_len, new_entry + 1); /* Skip first { */
					strcat(new_config, config_content + suffix_start);

					fp = fopen(config_path, "w");
					if (fp) {
						fputs(new_config, fp);
						fclose(fp);
						wlr_log(WLR_INFO, "Updated Steam config with %s as default", ge_proton_name);
					}
					free(new_config);
				}
			}
		}
	}

	free(config_content);
}

/* HTPC mode - optimized for controller/TV usage */














/* Fast update of just the search field text (avoids full re-render) */

/* Timer callback - updates and renders after typing stops (300ms) */

/* Schedule delayed full render (resets on each keystroke) */


/* Fast results-only render for navigation - just updates the results area */

/* Ultra-fast selection update - just toggles highlight visibility */

/* ===== Nixpkgs Install Popup ===== */











/* Get layout metrics for nixpkgs popup (simplified - no tabs) */






/* Toast notification functions */






/* ============================================================================
 * Gamepad Controller Menu Implementation
 * ============================================================================ */






/* Handle mouse click on gamepad menu - returns 1 if click was inside menu */


/* Handle gamepad button press - returns 1 if handled */

/* ==================== PC GAMING VIEW ==================== */



/* Save games cache to file for fast loading */

/* Load games from cache file */

/* Fetch Steam game names using parallel API requests */

/* Load Steam playtime from localconfig.vdf */

/* Sort games by acquisition date (newest first) using merge sort */
GameEntry *
pc_gaming_merge_sorted(GameEntry *a, GameEntry *b)
{
	if (!a) return b;
	if (!b) return a;

	GameEntry *result = NULL;
	int a_played = (a->acquired_time > 0);
	int b_played = (b->acquired_time > 0);

	/* Played games come first */
	if (a_played && !b_played) {
		result = a;
		result->next = pc_gaming_merge_sorted(a->next, b);
	} else if (!a_played && b_played) {
		result = b;
		result->next = pc_gaming_merge_sorted(a, b->next);
	} else if (a_played && b_played) {
		/* Both played - sort by most recently played first */
		if (a->acquired_time >= b->acquired_time) {
			result = a;
			result->next = pc_gaming_merge_sorted(a->next, b);
		} else {
			result = b;
			result->next = pc_gaming_merge_sorted(a, b->next);
		}
	} else {
		/* Neither played - sort by app ID (lower ID = older game, comes first) */
		if (atoi(a->id) <= atoi(b->id)) {
			result = a;
			result->next = pc_gaming_merge_sorted(a->next, b);
		} else {
			result = b;
			result->next = pc_gaming_merge_sorted(a, b->next);
		}
	}
	return result;
}




/* Known Proton, runtime, and tool app IDs to always filter */

/* Filter out non-games (DLC, tools, servers, etc.) */

/* Update installation status for games (check download progress) */

/* Load and scale game icon to tile dimensions */




/* Callback when games cache file changes - triggers realtime UI update */

/* Set up inotify watch on games cache file */

/* Ensure selected tile is visible by adjusting scroll */


/* Timer callback for updating installation progress every 2 seconds */















/* ============================================================================
 * Retro Gaming View Implementation
 * ============================================================================ */

struct wl_event_source *retro_anim_timer = NULL;







/* =========================================================== */
/* Media Grid View (Movies & TV-shows) */
/* =========================================================== */


/* Server info with priority for multi-server deduplication */
MediaServer media_servers[MAX_MEDIA_SERVERS];
int media_server_count = 0;

/* Discovered server URL (auto-discovered or fallback to localhost) */
char discovered_server_url[256] = "";
int server_discovered = 0;
uint64_t last_discovery_attempt_ms = 0;

/* Add server to list (avoiding duplicates) */

/* Try to discover nixly-server on local network via UDP broadcast
 * Returns number of servers discovered */

/* Try localhost as fallback */

/* Run discovery for local servers (always runs, even with configured servers) */

/* Get best server URL (highest priority) */

/* Get all servers (for fetching from multiple sources) */

/* Calculate if buffering is needed and how long
 * Returns buffer time in seconds, 0 if direct playback is possible */

/* Load resume position for media ID */

/* Save resume position for media ID */

/* Launch integrated video player for local files or streaming URLs
 * Uses FFmpeg + PipeWire for HDR, lossless video/audio, and proper frame timing */

/* Launch integrated video player (no resume) */

/* Stop integrated video player */

/* Start media playback (with buffering check) */

/* Check buffering progress and start playback when ready */

/* Cancel buffering */

MediaGridView *
media_get_view(Monitor *m, MediaViewType type)
{
	if (!m) return NULL;
	return (type == MEDIA_VIEW_MOVIES) ? &m->movies_view : &m->tvshows_view;
}


/* Simple JSON string extraction helper with escape sequence handling */



/* Poll timer callback - refresh media views every 3 seconds */

/* Parse media items from JSON buffer into temp array for deduplication */
int
parse_media_json(const char *buffer, const char *server_url, MediaItem **out_items, int max_items)
{
	const char *pos = strchr(buffer, '[');
	if (!pos) return 0;
	pos++;

	int count = 0;
	while (*pos && count < max_items) {
		const char *obj_start = strchr(pos, '{');
		if (!obj_start) break;

		int depth = 1;
		const char *obj_end = obj_start + 1;
		while (*obj_end && depth > 0) {
			if (*obj_end == '{') depth++;
			else if (*obj_end == '}') depth--;
			obj_end++;
		}
		if (depth != 0) break;

		size_t obj_len = obj_end - obj_start;
		char *obj_json = malloc(obj_len + 1);
		if (!obj_json) break;
		strncpy(obj_json, obj_start, obj_len);
		obj_json[obj_len] = '\0';

		MediaItem *item = calloc(1, sizeof(MediaItem));
		if (item) {
			item->id = json_extract_int(obj_json, "id");
			item->type = json_extract_int(obj_json, "type");
			json_extract_string(obj_json, "show_name", item->show_name, sizeof(item->show_name));
			if (!json_extract_string(obj_json, "tmdb_title", item->title, sizeof(item->title)) ||
			    item->title[0] == '\0') {
				if (item->show_name[0])
					snprintf(item->title, sizeof(item->title), "%s", item->show_name);
				else
					json_extract_string(obj_json, "title", item->title, sizeof(item->title));
			}
			item->season = json_extract_int(obj_json, "season");
			item->episode = json_extract_int(obj_json, "episode");
			item->duration = json_extract_int(obj_json, "duration");
			item->year = json_extract_int(obj_json, "year");
			item->rating = json_extract_float(obj_json, "rating");
			json_extract_string(obj_json, "poster", item->poster_path, sizeof(item->poster_path));
			json_extract_string(obj_json, "backdrop", item->backdrop_path, sizeof(item->backdrop_path));
			json_extract_string(obj_json, "overview", item->overview, sizeof(item->overview));
			json_extract_string(obj_json, "genres", item->genres, sizeof(item->genres));
			item->tmdb_total_seasons = json_extract_int(obj_json, "tmdb_total_seasons");
			item->tmdb_total_episodes = json_extract_int(obj_json, "tmdb_total_episodes");
			item->tmdb_episode_runtime = json_extract_int(obj_json, "tmdb_episode_runtime");
			json_extract_string(obj_json, "tmdb_status", item->tmdb_status, sizeof(item->tmdb_status));
			json_extract_string(obj_json, "tmdb_next_episode", item->tmdb_next_episode, sizeof(item->tmdb_next_episode));
			item->tmdb_id = json_extract_int(obj_json, "tmdb_id");
			json_extract_string(obj_json, "server_id", item->server_id, sizeof(item->server_id));
			item->server_priority = json_extract_int(obj_json, "server_priority");
			strncpy(item->server_url, server_url, sizeof(item->server_url) - 1);

			out_items[count++] = item;
		}
		free(obj_json);
		pos = obj_end;
	}
	return count;
}

/* Fetch media list from ALL servers, deduplicate by tmdb_id keeping highest priority */

/* Load poster image for a media item - scale-to-fill (cover) the target area */

/* Load backdrop image for detail view - scale-to-fill */

/* Free seasons list */

/* Free episodes list */

/* Fetch seasons for a TV show */
void
media_view_fetch_seasons(MediaGridView *view, const char *show_name)
{
	FILE *fp;
	char cmd[1024];
	char buffer[8192];
	size_t bytes_read;

	media_view_free_seasons(view);

	/* URL-encode spaces in show name */
	char encoded_name[512];
	char *dst = encoded_name;
	for (const char *src = show_name; *src && dst < encoded_name + sizeof(encoded_name) - 4; src++) {
		if (*src == ' ') {
			*dst++ = '%';
			*dst++ = '2';
			*dst++ = '0';
		} else {
			*dst++ = *src;
		}
	}
	*dst = '\0';

	snprintf(cmd, sizeof(cmd), "curl -s '%s/api/show/%s/seasons' 2>/dev/null",
	         view->server_url, encoded_name);

	fp = popen(cmd, "r");
	if (!fp) return;

	bytes_read = fread(buffer, 1, sizeof(buffer) - 1, fp);
	pclose(fp);

	if (bytes_read == 0) return;
	buffer[bytes_read] = '\0';

	/* Parse JSON array */
	const char *pos = strchr(buffer, '[');
	if (!pos) return;
	pos++;

	MediaSeason *last = NULL;
	int count = 0;

	while (*pos) {
		const char *obj_start = strchr(pos, '{');
		if (!obj_start) break;

		int depth = 1;
		const char *obj_end = obj_start + 1;
		while (*obj_end && depth > 0) {
			if (*obj_end == '{') depth++;
			else if (*obj_end == '}') depth--;
			obj_end++;
		}

		if (depth != 0) break;

		size_t obj_len = obj_end - obj_start;
		char *obj_json = malloc(obj_len + 1);
		if (!obj_json) break;
		strncpy(obj_json, obj_start, obj_len);
		obj_json[obj_len] = '\0';

		MediaSeason *season = calloc(1, sizeof(MediaSeason));
		if (season) {
			season->season = json_extract_int(obj_json, "season");
			season->episode_count = json_extract_int(obj_json, "episode_count");

			if (!view->seasons) {
				view->seasons = season;
			} else {
				last->next = season;
			}
			last = season;
			count++;
		}

		free(obj_json);
		pos = obj_end;
	}

	view->season_count = count;
}

/* Fetch episodes for a specific season */
void
media_view_fetch_episodes(MediaGridView *view, const char *show_name, int season)
{
	FILE *fp;
	char cmd[1024];
	char buffer[1024 * 256];
	size_t bytes_read;

	media_view_free_episodes(view);

	/* URL-encode spaces in show name */
	char encoded_name[512];
	char *dst = encoded_name;
	for (const char *src = show_name; *src && dst < encoded_name + sizeof(encoded_name) - 4; src++) {
		if (*src == ' ') {
			*dst++ = '%';
			*dst++ = '2';
			*dst++ = '0';
		} else {
			*dst++ = *src;
		}
	}
	*dst = '\0';

	snprintf(cmd, sizeof(cmd), "curl -s '%s/api/show/%s/episodes/%d' 2>/dev/null",
	         view->server_url, encoded_name, season);

	fp = popen(cmd, "r");
	if (!fp) return;

	bytes_read = fread(buffer, 1, sizeof(buffer) - 1, fp);
	pclose(fp);

	if (bytes_read == 0) return;
	buffer[bytes_read] = '\0';

	/* Parse JSON array */
	const char *pos = strchr(buffer, '[');
	if (!pos) return;
	pos++;

	MediaItem *last = NULL;
	int count = 0;

	while (*pos) {
		const char *obj_start = strchr(pos, '{');
		if (!obj_start) break;

		int depth = 1;
		const char *obj_end = obj_start + 1;
		while (*obj_end && depth > 0) {
			if (*obj_end == '{') depth++;
			else if (*obj_end == '}') depth--;
			obj_end++;
		}

		if (depth != 0) break;

		size_t obj_len = obj_end - obj_start;
		char *obj_json = malloc(obj_len + 1);
		if (!obj_json) break;
		strncpy(obj_json, obj_start, obj_len);
		obj_json[obj_len] = '\0';

		MediaItem *item = calloc(1, sizeof(MediaItem));
		if (item) {
			item->id = json_extract_int(obj_json, "id");
			item->type = json_extract_int(obj_json, "type");
			json_extract_string(obj_json, "title", item->title, sizeof(item->title));
			json_extract_string(obj_json, "show_name", item->show_name, sizeof(item->show_name));
			item->season = json_extract_int(obj_json, "season");
			item->episode = json_extract_int(obj_json, "episode");
			item->duration = json_extract_int(obj_json, "duration");
			item->year = json_extract_int(obj_json, "year");
			item->rating = json_extract_float(obj_json, "rating");
			json_extract_string(obj_json, "poster", item->poster_path, sizeof(item->poster_path));
			json_extract_string(obj_json, "backdrop", item->backdrop_path, sizeof(item->backdrop_path));
			json_extract_string(obj_json, "overview", item->overview, sizeof(item->overview));
			json_extract_string(obj_json, "episode_title", item->episode_title, sizeof(item->episode_title));
			json_extract_string(obj_json, "filepath", item->filepath, sizeof(item->filepath));

			if (!view->episodes) {
				view->episodes = item;
			} else {
				last->next = item;
			}
			last = item;
			count++;
		}

		free(obj_json);
		pos = obj_end;
	}

	view->episode_count = count;
}

/* Render buffering overlay */

/* Render detail view for a movie or TV show */

/* Enter detail view for selected item */

/* Exit detail view back to grid */

/* Ensure selected item is visible (scroll if needed) */

/* Render the media grid view */





/* Find monitor with visible Retro Gaming view */


/* Handle playback OSD menu navigation */

/* Handle detail view button input */


/* Handle playback keyboard input */

/* Handle detail view keyboard input */


/* =========================================================== */
/* End Media Grid View */
/* =========================================================== */

/* Check if device is a gamepad (not keyboard/mouse) */




/* Grab exclusive access to gamepad (prevents Steam from receiving input) */

/* Release exclusive access to gamepad (allows Steam to receive input) */

/*
 * Determine if nixlytile should grab gamepads.
 * We grab when we need the input (HTPC views on non-Steam tags).
 * We release when Steam/games need it (Steam focused or on Steam tag).
 */

/* Update grab state for all gamepads based on current context */

/* Timer callback to process pending gamepad devices */



/* Joystick navigation state for PC gaming view */
uint64_t joystick_nav_last_move = 0;
int joystick_nav_repeat_started = 0;

/* Update cursor position based on joystick input from all gamepads */


/* Switch TV/Monitor to this device via HDMI-CEC */

/* ============================================================================
 * Bluetooth Controller Auto-Pairing
 * ============================================================================ */

/* Check if device name matches known gamepad patterns */

/* Async callback for trust property set */

/* Trust a Bluetooth device asynchronously (allows auto-reconnect) */

/* Async callback for Pair method */

/* Pair with a Bluetooth device (async) */

/* Async callback for Connect method */

/* Connect to a paired Bluetooth device (async) */

/* Handle new device discovery signal from BlueZ */

/* Async callback for GetManagedObjects - checks and connects gamepads */

/* Check existing devices and pair any gamepads (async) */

/* Async callback for StartDiscovery */

/* Start Bluetooth discovery (async) */

/* Async callback for StopDiscovery */

/* Stop Bluetooth discovery (async) */

/* Timer callback for Bluetooth scanning */

/* Bluetooth bus event callback - processes D-Bus messages without blocking */

/* Initialize Bluetooth controller auto-pairing */

/* Cleanup Bluetooth controller auto-pairing */

/* Check if any monitor is active (enabled and not asleep) */

/* Turn off Xbox controller LED via sysfs */

/* Async callback for Bluetooth Disconnect method */

/* Async callback for GetManagedObjects - finds and disconnects the device */

/* Disconnect Bluetooth device by name - completely powers off the controller (async) */

/* Suspend gamepad - power it off completely */

/* Resume gamepad - wake it up */

/* Gamepad inactivity timer callback */











/* Check if a view is active (visible and on current tag) */

/* Update HTPC view visibility based on current tag */










/* Internal handler for pointer button events - used by both real mouse and gamepad */




void
cleanup(void)
{
	TrayItem *it, *tmp;

	wlr_log(WLR_ERROR, "cleanup() called - starting cleanup sequence");
	/* Cleanup video player */
	if (active_videoplayer) {
		videoplayer_destroy(active_videoplayer);
		active_videoplayer = NULL;
	}
	cleanuplisteners();
	gamepad_cleanup();
	bt_controller_cleanup();
	drop_net_icon_buffer();
	drop_cpu_icon_buffer();
	drop_clock_icon_buffer();
	drop_light_icon_buffer();
	drop_ram_icon_buffer();
	drop_battery_icon_buffer();
	drop_mic_icon_buffer();
	drop_volume_icon_buffer();
	drop_bluetooth_icon_buffer();
	drop_steam_icon_buffer();
	drop_discord_icon_buffer();
	stop_public_ip_fetch();
	stop_ssid_fetch();
	wifi_scan_finish();
	net_menu_hide_all();
	wifi_networks_clear();
	last_clock_render[0] = last_cpu_render[0] = last_ram_render[0] = '\0';
	last_light_render[0] = last_volume_render[0] = last_mic_render[0] = last_battery_render[0] = '\0';
	last_net_render[0] = '\0';
	last_clock_h = last_cpu_h = last_ram_h = last_light_h = last_volume_h = last_mic_h = last_battery_h = last_net_h = 0;
	if (wifi_scan_timer) {
		wl_event_source_remove(wifi_scan_timer);
		wifi_scan_timer = NULL;
	}
	if (status_timer) {
		wl_event_source_remove(status_timer);
		status_timer = NULL;
	}
	if (status_cpu_timer) {
		wl_event_source_remove(status_cpu_timer);
		status_cpu_timer = NULL;
	}
	if (status_hover_timer) {
		wl_event_source_remove(status_hover_timer);
		status_hover_timer = NULL;
	}
	if (cpu_popup_refresh_timer) {
		wl_event_source_remove(cpu_popup_refresh_timer);
		cpu_popup_refresh_timer = NULL;
	}
	if (ram_popup_refresh_timer) {
		wl_event_source_remove(ram_popup_refresh_timer);
		ram_popup_refresh_timer = NULL;
	}
	if (popup_delay_timer) {
		wl_event_source_remove(popup_delay_timer);
		popup_delay_timer = NULL;
	}
	if (pc_gaming_install_timer) {
		wl_event_source_remove(pc_gaming_install_timer);
		pc_gaming_install_timer = NULL;
	}
	/* Clean up config rewatch timer */
	if (config_rewatch_timer) {
		wl_event_source_remove(config_rewatch_timer);
		config_rewatch_timer = NULL;
	}
	config_needs_rewatch = 0;
	tray_menu_hide_all();
	if (tray_event) {
		wl_event_source_remove(tray_event);
		tray_event = NULL;
	}
	if (tray_vtable_slot)
		sd_bus_slot_unref(tray_vtable_slot);
	if (tray_fdo_vtable_slot)
		sd_bus_slot_unref(tray_fdo_vtable_slot);
	if (tray_name_slot)
		sd_bus_slot_unref(tray_name_slot);
	if (tray_bus)
		sd_bus_unref(tray_bus);
	wl_list_for_each_safe(it, tmp, &tray_items, link) {
		if (it->icon_buf)
			wlr_buffer_drop(it->icon_buf);
		wl_list_remove(&it->link);
		free(it);
	}
#ifdef XWAYLAND
	wlr_xwayland_destroy(xwayland);
	xwayland = NULL;
#endif
	/* Stop systemd target */
	if (fork() == 0) {
		setsid();
		if (has_nixlytile_session_target()) {
			execvp("systemctl", (char *const[]) {
				"systemctl", "--user", "stop", "nixlytile-session.target", NULL
			});
		}
		exit(1);
	}

	/* Send close to all clients and give them time to save state */
	{
		Client *c;
		int has_clients = 0;
		wl_list_for_each(c, &clients, link) {
			client_send_close(c);
			has_clients = 1;
		}
		/* Wait for clients to close gracefully */
		if (has_clients) {
			struct timespec ts = {0, 500000000}; /* 500ms */
			nanosleep(&ts, NULL);
		}
	}

	wl_display_destroy_clients(dpy);
	if (child_pid > 0) {
		kill(-child_pid, SIGTERM);
		waitpid(child_pid, NULL, 0);
	}
	freestatusfont();
	if (fcft_initialized) {
		fcft_fini();
		fcft_initialized = 0;
	}
	wlr_xcursor_manager_destroy(cursor_mgr);

	destroykeyboardgroup(&kb_group->destroy, NULL);

	/* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
	 * Destroy it until it's fixed on the wlroots side */
	wlr_backend_destroy(backend);

	wl_display_destroy(dpy);
	/* Destroy after the wayland display (when the monitors are already destroyed)
	   to avoid destroying them with an invalid scene output. */
	wlr_scene_node_destroy(&scene->tree.node);
}


void
cleanuplisteners(void)
{
	wl_list_remove(&cursor_axis.link);
	wl_list_remove(&cursor_button.link);
	wl_list_remove(&cursor_frame.link);
	wl_list_remove(&cursor_motion.link);
	wl_list_remove(&cursor_motion_absolute.link);
	wl_list_remove(&gpu_reset.link);
	wl_list_remove(&new_idle_inhibitor.link);
	wl_list_remove(&layout_change.link);
	wl_list_remove(&new_input_device.link);
	wl_list_remove(&new_virtual_keyboard.link);
	wl_list_remove(&new_virtual_pointer.link);
	wl_list_remove(&new_pointer_constraint.link);
	wl_list_remove(&new_output.link);
	wl_list_remove(&new_xdg_toplevel.link);
	wl_list_remove(&new_xdg_decoration.link);
	wl_list_remove(&new_xdg_popup.link);
	wl_list_remove(&new_layer_surface.link);
	wl_list_remove(&output_mgr_apply.link);
	wl_list_remove(&output_mgr_test.link);
	wl_list_remove(&output_power_mgr_set_mode.link);
	wl_list_remove(&request_activate.link);
	wl_list_remove(&request_cursor.link);
	wl_list_remove(&request_set_psel.link);
	wl_list_remove(&request_set_sel.link);
	wl_list_remove(&request_set_cursor_shape.link);
	wl_list_remove(&request_start_drag.link);
	wl_list_remove(&start_drag.link);
	wl_list_remove(&new_session_lock.link);
#ifdef XWAYLAND
	wl_list_remove(&new_xwayland_surface.link);
	wl_list_remove(&xwayland_ready.link);
#endif
}













/* Find a mode matching resolution and optionally refresh rate */






















/* Warp cursor to center of monitor in specified direction */

/* Get monitor by index (0-based) */

/* Move focused client to monitor by index */


/* We probably should change the name of this: it sounds like it
 * will focus the topmost client of this mon, when actually will
 * only return that client */



void
handlesig(int signo)
{
	if (signo == SIGCHLD) {
		while (waitpid(-1, NULL, WNOHANG) > 0);
	} else if (signo == SIGINT) {
		write(STDERR_FILENO, "handlesig: SIGINT received, quitting\n", 37);
		quit(NULL);
	} else if (signo == SIGTERM) {
		write(STDERR_FILENO, "handlesig: SIGTERM received, quitting\n", 38);
		quit(NULL);
	} else if (signo == SIGPIPE) {
		write(STDERR_FILENO, "handlesig: SIGPIPE received (ignored)\n", 38);
	}
}













/* node_contains_client moved to layout.c */




















void
quit(const Arg *arg)
{
	wlr_log(WLR_ERROR, "quit() called - terminating display");
	wl_display_terminate(dpy);
}

/*
 * Get current time in nanoseconds (monotonic clock).
 * Used for precise frame timing measurements.
 */
uint64_t
get_time_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/*
 * Render a monitor frame with optimal frame pacing.
 *
 * Frame pacing strategy (inspired by Gamescope):
 *
 * For GAMES (content-type=game or tearing hint):
 *   - Use async page flips (tearing) for minimum input latency
 *   - Frame is displayed immediately when GPU finishes
 *   - Best for competitive/fast-paced games
 *
 * For VIDEO (content-type=video or detected video framerate):
 *   - Use VRR if available for judder-free playback
 *   - Otherwise use vsync with matched refresh rate
 *   - Critical: consistent frame timing is more important than latency
 *
 * For NORMAL content:
 *   - Standard vsync'd commits
 *   - Balance between latency and smoothness
 *
 * Frame timing tracking:
 *   - Track commit duration for predicting future commits
 *   - Use rolling average to handle spikes gracefully
 *   - Skip unnecessary commits when nothing has changed
 *   - Compositor-controlled frame pacing for optimal display timing
 */

/*
 * Presentation feedback callback - called when a frame is actually displayed.
 * This gives us precise vblank timing for optimal frame pacing.
 */





void
run(const char *startup_cmd)
{
	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(dpy);
	if (!socket)
		die("startup: display_add_socket_auto");
	setenv("WAYLAND_DISPLAY", socket, 1);

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(backend))
		die("startup: backend_start");

	/* Now that the socket exists and the backend is started, run the startup command */
	if (startup_cmd) {
		int piperw[2];
		if (pipe(piperw) < 0)
			die("startup: pipe:");
		if ((child_pid = fork()) < 0)
			die("startup: fork:");
		if (child_pid == 0) {
			setsid();
			dup2(piperw[0], STDIN_FILENO);
			close(piperw[0]);
			close(piperw[1]);
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
			die("startup: execl:");
		}
		dup2(piperw[1], STDOUT_FILENO);
		close(piperw[1]);
		close(piperw[0]);
	}

	/* Import environment variables then start systemd target */
	if (fork() == 0) {
		pid_t import_pid;
		setsid();

		/* First: import environment variables */
		import_pid = fork();
		if (import_pid == 0) {
			execvp("systemctl", (char *const[]) {
				"systemctl", "--user", "import-environment",
				"DISPLAY", "WAYLAND_DISPLAY", NULL
			});
			exit(1);
		}

		/* Wait for import to complete */
		waitpid(import_pid, NULL, 0);

		/* Second: start target */
		if (has_nixlytile_session_target()) {
			execvp("systemctl", (char *const[]) {
				"systemctl", "--user", "start", "nixlytile-session.target", NULL
			});
		}

		exit(1);
	}

	/* Mark stdout as non-blocking to avoid the startup script
	 * causing dwl to freeze when a user neither closes stdin
	 * nor consumes standard input in his startup script */

	if (fd_set_nonblock(STDOUT_FILENO) < 0)
		close(STDOUT_FILENO);

	printstatus();

	/* At this point the outputs are initialized, choose initial selmon based on
	 * cursor position, and set default cursor image */
	selmon = xytomon(cursor->x, cursor->y);

	/* TODO hack to get cursor to display in its initial location (100, 100)
	 * instead of (0, 0) and then jumping. Still may not be fully
	 * initialized, as the image/coordinates are not transformed for the
	 * monitor when displayed here */
	wlr_cursor_warp_closest(cursor, NULL, cursor->x, cursor->y);
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wl_display_run(dpy);
}


/*
 * Dynamic Game VRR - Matches display refresh rate to game framerate.
 *
 * When VRR is enabled and a game is running fullscreen, the compositor
 * dynamically adjusts the display's refresh rate to match the game's
 * actual framerate. This eliminates judder when games run below the
 * monitor's native refresh rate.
 *
 * Key features:
 * - Hysteresis: Requires stable FPS for 30+ frames before adjusting
 * - Deadband: Ignores FPS changes < 3 FPS to avoid jitter
 * - Rate limiting: No more than one adjustment per 500ms
 * - Smooth transitions: VRR handles the actual rate change smoothly
 */


/*
 * Enable game VRR mode - activates adaptive sync for game framerate matching.
 */

/*
 * Disable game VRR mode - returns to fixed refresh rate.
 */

/*
 * Update game VRR based on current measured FPS.
 *
 * This is called from rendermon() when a game is running fullscreen.
 * It implements hysteresis and rate-limiting to avoid jittery behavior.
 */

/* Check if a client's surface has video content type */

/* Check if a client's surface has game content type */

/*
 * Check if a client wants tearing (async page flips).
 * Games may request this for lowest latency.
 */

/*
 * Set output to a refresh rate optimized for video playback.
 * For content-type based detection, we start frame tracking to determine
 * the actual fps, then set custom mode.
 */

/* Restore output to maximum refresh rate and disable VRR */

/*
 * ============================================================================
 * HDR AND 10-BIT COLOR SUPPORT
 * ============================================================================
 *
 * This section handles:
 * - Detection of 10-bit and HDR capable monitors
 * - Setting optimal render format (XRGB2101010 for 10-bit, XRGB8888 for 8-bit)
 * - Configuring DRM connector properties for best color output:
 *   - max bpc: Maximum bits per channel (typically 8, 10, 12, or 16)
 *   - Colorspace/output_color_format: RGB 4:4:4 vs YCbCr 4:2:2/4:2:0
 *
 * Note: Full HDR (PQ/BT.2020) requires wlroots with HDR10 support which is
 * still pending merge (MR !5002). This code prepares the infrastructure.
 */

/*
 * Check if an output supports 10-bit or higher color depth.
 * This queries the DRM connector's max bpc property and supported formats.
 */

/*
 * Set DRM connector properties for optimal color output.
 * This sets:
 * - max bpc: Maximum bits per channel (10 for 10-bit, 12 for HDR, etc.)
 * - Colorspace: BT709/BT2020 (for HDR)
 *
 * Note: These are low-level DRM properties, accessed via libdrm.
 */

/*
 * Enable 10-bit rendering for a monitor.
 * This sets the render format to XRGB2101010 for better color gradients.
 */

/*
 * Initialize HDR and color depth settings for a new monitor.
 * Called from createmon() after the output is initialized.
 */

/* Track frame commit timestamp for a client (only when buffer changes) */

/*
 * Analyze frame times to detect exact video framerate.
 * Returns exact Hz value (e.g., 23.976, 29.97, 59.94) or 0.0 if not video.
 * Inspired by Gamescope's approach of calculating precise frame timing.
 */

/*
 * Video playback quality scoring system.
 * Higher score = better quality (smoother playback, less judder).
 *
 * Score components:
 * - Perfect sync (integer multiple): 100 points base
 * - VRR: 150 points (best possible - true frame-by-frame sync)
 * - Lower multiplier: bonus (2x > 3x > 4x)
 * - Precision penalty: deduct for non-exact matches
 */

/*
 * Calculate judder (frame timing error) for a given mode/video Hz combination.
 * Returns estimated judder in milliseconds per frame.
 * 0 = perfect sync (integer multiple or VRR)
 */

/*
 * Score a video mode candidate. Higher score = better.
 */

/*
 * Find the absolute best video mode for the given framerate.
 * Evaluates ALL options and returns the one with highest quality score.
 *
 * Priority (by quality):
 * 1. VRR if available - true judder-free
 * 2. Exact integer multiple mode (120Hz for 24fps, etc)
 * 3. Custom CVT mode at exact multiple
 * 4. Closest available mode
 */
VideoModeCandidate
find_best_video_mode(Monitor *m, float video_hz)
{
	VideoModeCandidate best = {0};
	VideoModeCandidate candidate;
	struct wlr_output_mode *mode;
	int width, height;

	best.score = -1.0f;
	best.judder_ms = 999.0f;
	best.target_hz = video_hz;

	if (!m || !m->wlr_output || !m->wlr_output->current_mode || video_hz <= 0.0f)
		return best;

	width = m->wlr_output->current_mode->width;
	height = m->wlr_output->current_mode->height;

	wlr_log(WLR_INFO, "Evaluating video modes for %.3f Hz on %s:", video_hz, m->wlr_output->name);

	/* Option 1: VRR (best quality if available) */
	if (m->vrr_capable && fullscreen_adaptive_sync_enabled) {
		candidate.method = 3;
		candidate.mode = NULL;
		candidate.multiplier = 1;
		candidate.target_hz = video_hz;
		candidate.actual_hz = video_hz;
		candidate.judder_ms = 0.0f;
		candidate.score = score_video_mode(3, video_hz, video_hz, 1);

		wlr_log(WLR_INFO, "  VRR: score=%.1f (judder=0ms)", candidate.score);

		if (candidate.score > best.score) {
			best = candidate;
		}
	}

	/* Option 2: Scan existing modes for integer multiples */
	wl_list_for_each(mode, &m->wlr_output->modes, link) {
		if (mode->width != width || mode->height != height)
			continue;

		float mode_hz = mode->refresh / 1000.0f;
		float ratio = mode_hz / video_hz;
		int multiple = (int)(ratio + 0.5f);

		if (multiple < 1 || multiple > 8)
			continue;

		float expected_hz = video_hz * multiple;
		float diff = fabsf(mode_hz - expected_hz);

		/* Only consider if within 0.5% tolerance */
		if (diff > expected_hz * 0.005f)
			continue;

		candidate.method = 1;
		candidate.mode = mode;
		candidate.multiplier = multiple;
		candidate.target_hz = video_hz;
		candidate.actual_hz = mode_hz;
		candidate.judder_ms = calculate_judder_ms(video_hz, mode_hz);
		candidate.score = score_video_mode(1, video_hz, mode_hz, multiple);

		wlr_log(WLR_INFO, "  Mode %d.%03dHz: %dx mult, score=%.1f, judder=%.2fms",
				mode->refresh / 1000, mode->refresh % 1000,
				multiple, candidate.score, candidate.judder_ms);

		if (candidate.score > best.score) {
			best = candidate;
		}
	}

	/* Option 3: Check if CVT custom modes could work (score them theoretically) */
	for (int mult = 1; mult <= 5; mult++) {
		float target_display_hz = video_hz * mult;

		/* Skip if below typical panel minimum or above maximum */
		if (target_display_hz < 48.0f || target_display_hz > 240.0f)
			continue;

		/* Check if we already found this rate in existing modes */
		int found = 0;
		wl_list_for_each(mode, &m->wlr_output->modes, link) {
			if (mode->width != width || mode->height != height)
				continue;
			float mode_hz = mode->refresh / 1000.0f;
			if (fabsf(mode_hz - target_display_hz) < 0.5f) {
				found = 1;
				break;
			}
		}

		if (!found && wlr_output_is_drm(m->wlr_output)) {
			candidate.method = 2;
			candidate.mode = NULL;
			candidate.multiplier = mult;
			candidate.target_hz = video_hz;
			candidate.actual_hz = target_display_hz;
			candidate.judder_ms = 0.0f; /* Custom CVT will be exact */
			candidate.score = score_video_mode(2, video_hz, target_display_hz, mult);

			/* Slightly penalize CVT modes as they're less reliable */
			candidate.score -= 5.0f;

			wlr_log(WLR_INFO, "  CVT %.3fHz: %dx mult, score=%.1f (theoretical)",
					target_display_hz, mult, candidate.score);

			if (candidate.score > best.score) {
				best = candidate;
			}
		}
	}

	if (best.score > 0) {
		const char *method_str = "none";
		switch (best.method) {
		case 1: method_str = "existing mode"; break;
		case 2: method_str = "custom CVT"; break;
		case 3: method_str = "VRR"; break;
		}
		wlr_log(WLR_INFO, "Best option: %s at %.3f Hz (%dx), score=%.1f, judder=%.2fms",
				method_str, best.actual_hz, best.multiplier, best.score, best.judder_ms);
	}

	return best;
}

/*
 * Find a mode that is an integer multiple of the target Hz.
 * For example, 23.976 Hz video looks smooth at 47.952 Hz, 71.928 Hz, 119.88 Hz, etc.
 * Returns NULL if no suitable mode found.
 */

/*
 * Enable VRR mode for judder-free video playback.
 * With VRR, frames are displayed exactly when ready - no need to match refresh rate.
 * Returns 1 on success, 0 on failure.
 */

/*
 * Disable VRR mode and restore normal operation.
 */

/*
 * Apply the best video mode based on scoring evaluation.
 * This is the main entry point for automatic video mode selection.
 * Returns 1 on success, 0 on failure.
 */

/*
 * Set optimal video mode using intelligent prioritization:
 * 1. VRR (Variable Refresh Rate) - best option, true judder-free
 * 2. Exact mode match (integer multiple of video Hz)
 * 3. Custom CVT mode generation
 *
 * Returns 1 on success, 0 on failure.
 */

/* Timer callback to check fullscreen clients for video content */

/* Schedule video check timer */

/* Hz OSD - show refresh rate change notification */



/* Test Hz OSD - show current monitor refresh rate */

/* Playback OSD timeout callback - hide OSD after delay */

/* Hide playback OSD bar */

/* Render playback OSD bar at bottom of screen */

/*
 * Generate CVT (Coordinated Video Timings) modeline for a given resolution and refresh rate.
 * This creates a full drmModeModeInfo structure with proper timing parameters.
 * Based on VESA CVT 1.2 standard formula (reduced blanking variant).
 */

/*
 * Generate a fixed mode by copying timings from base mode and adjusting only the clock.
 * This is the method Gamescope uses for dynamic refresh rate changes - it preserves
 * all timing parameters and just changes the pixel clock to achieve the target refresh.
 * Much more likely to be accepted by the display than CVT-generated timings.
 */

/* Set a custom refresh rate from keybind - uses arg.f as Hz (e.g., 25.55) */

/*
 * Check if any fullscreen client is playing video and adjust refresh rate.
 * Uses intelligent scoring system to find optimal mode for silky smooth playback.
 * Implements 3-retry logic when no video is detected initially.
 */






/* arg > 1.0 will set mfact absolutely */




/* ========== Runtime Configuration Parser ========== */





/* Keysym name to XKB keysym lookup */
xkb_keysym_t config_parse_keysym(const char *name)
{
	xkb_keysym_t sym;

	/* Try XKB lookup first */
	sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
	if (sym != XKB_KEY_NoSymbol) return sym;

	/* Try case-insensitive */
	sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym != XKB_KEY_NoSymbol) return sym;

	/* Common aliases */
	if (strcasecmp(name, "enter") == 0) return XKB_KEY_Return;
	if (strcasecmp(name, "esc") == 0) return XKB_KEY_Escape;
	if (strcasecmp(name, "backspace") == 0) return XKB_KEY_BackSpace;
	if (strcasecmp(name, "tab") == 0) return XKB_KEY_Tab;
	if (strcasecmp(name, "space") == 0) return XKB_KEY_space;
	if (strcasecmp(name, "up") == 0) return XKB_KEY_Up;
	if (strcasecmp(name, "down") == 0) return XKB_KEY_Down;
	if (strcasecmp(name, "left") == 0) return XKB_KEY_Left;
	if (strcasecmp(name, "right") == 0) return XKB_KEY_Right;
	if (strcasecmp(name, "delete") == 0) return XKB_KEY_Delete;
	if (strcasecmp(name, "home") == 0) return XKB_KEY_Home;
	if (strcasecmp(name, "end") == 0) return XKB_KEY_End;
	if (strcasecmp(name, "pageup") == 0) return XKB_KEY_Page_Up;
	if (strcasecmp(name, "pagedown") == 0) return XKB_KEY_Page_Down;
	if (strcasecmp(name, "print") == 0) return XKB_KEY_Print;

	return XKB_KEY_NoSymbol;
}

/* Parse modifier string like "super+shift" */

/* Stored spawn commands for runtime bindings */
char *runtime_spawn_cmds[MAX_KEYS];
int runtime_spawn_cmd_count = 0;

/* ========== Runtime Monitor Configuration ========== */
RuntimeMonitorConfig runtime_monitors[MAX_MONITORS];
int runtime_monitor_count = 0;

/* Default monitor assignments by connector type priority */
int monitor_master_set = 0;  /* Track if master was explicitly configured */

/* Function lookup for keybindings */
const FuncEntry func_table[] = {
	{ "quit",              quit,              0 },
	{ "killclient",        killclient,        0 },
	{ "focusstack",        focusstack,        1 },
	{ "incnmaster",        incnmaster,        1 },
	{ "setmfact",          setmfact,          3 },
	{ "zoom",              zoom,              0 },
	{ "view",              view,              2 },
	{ "tag",               tag,               2 },
	{ "toggleview",        toggleview,        2 },
	{ "toggletag",         toggletag,         2 },
	{ "togglefloating",    togglefloating,    0 },
	{ "togglefullscreen",  togglefullscreen,  0 },
	{ "togglegaps",        togglegaps,        0 },
	{ "togglestatusbar",   togglestatusbar,   0 },
	{ "htpc_mode_toggle",  htpc_mode_toggle,  0 },
	{ "focusmon",          focusmon,          1 },
	{ "tagmon",            tagmon,            1 },
	{ "setlayout",         setlayout,         0 },
	{ "spawn",             spawn,             4 },
	{ "modal_show",        modal_show,        0 },
	{ "modal_show_files",  modal_show_files,  0 },
	{ "modal_show_git",    modal_show_git,    0 },
	{ "nixpkgs_show",      nixpkgs_show,      0 },
	{ "focusdir",          focusdir,          2 },
	{ "swapclients",       swapclients,       1 },
	{ "setratio_h",        setratio_h,        3 },
	{ "setratio_v",        setratio_v,        3 },
	{ "rotate_clients",    rotate_clients,    1 },
	{ "warptomonitor",     warptomonitor,     1 },
	{ "tagtomonitornum",   tagtomonitornum,   2 },
	{ "chvt",              chvt,              2 },
	{ "gamepanel", gamepanel, 0 },
	{ NULL, NULL, 0 }
};

/* Parse a single keybinding line: bind = mod+key action [arg] */

/* Parse monitor position string */
MonitorPosition config_parse_monitor_position(const char *pos)
{
	if (!pos || !*pos) return MON_POS_AUTO;
	if (strcasecmp(pos, "master") == 0 || strcasecmp(pos, "primary") == 0 || strcasecmp(pos, "1") == 0)
		return MON_POS_MASTER;
	if (strcasecmp(pos, "left") == 0 || strcasecmp(pos, "2") == 0)
		return MON_POS_LEFT;
	if (strcasecmp(pos, "right") == 0 || strcasecmp(pos, "3") == 0)
		return MON_POS_RIGHT;
	if (strcasecmp(pos, "top-left") == 0 || strcasecmp(pos, "topleft") == 0 || strcasecmp(pos, "4") == 0)
		return MON_POS_TOP_LEFT;
	if (strcasecmp(pos, "top-right") == 0 || strcasecmp(pos, "topright") == 0 || strcasecmp(pos, "5") == 0)
		return MON_POS_TOP_RIGHT;
	if (strcasecmp(pos, "bottom-left") == 0 || strcasecmp(pos, "bottomleft") == 0)
		return MON_POS_BOTTOM_LEFT;
	if (strcasecmp(pos, "bottom-right") == 0 || strcasecmp(pos, "bottomright") == 0)
		return MON_POS_BOTTOM_RIGHT;
	if (strcasecmp(pos, "auto") == 0)
		return MON_POS_AUTO;
	return MON_POS_AUTO;
}

/* Parse transform string */

/* Parse monitor configuration line:
 * monitor = name position [WxH@Hz] [scale=X] [transform=X] [mfact=X] [nmaster=X] [disabled]
 * Examples:
 *   monitor = HDMI-A-1 master
 *   monitor = DP-1 left 1920x1080@144 scale=1.0
 *   monitor = eDP-1 master 2560x1440@60 scale=1.5
 *   monitor = * auto
 */

/* Find runtime config for a monitor by name */
RuntimeMonitorConfig *find_monitor_config(const char *name)
{
	int i;
	for (i = 0; i < runtime_monitor_count; i++) {
		/* Support wildcards: "*" matches all, "DP-*" matches DP-1, DP-2, etc. */
		const char *pattern = runtime_monitors[i].name;
		if (strcmp(pattern, "*") == 0)
			return &runtime_monitors[i];
		if (strchr(pattern, '*')) {
			/* Simple prefix wildcard: "DP-*" */
			size_t prefix_len = strchr(pattern, '*') - pattern;
			if (strncmp(name, pattern, prefix_len) == 0)
				return &runtime_monitors[i];
		} else if (strcmp(name, pattern) == 0) {
			return &runtime_monitors[i];
		}
	}
	return NULL;
}

/* Get monitor connector type priority (lower = higher priority for master) */

/* Calculate monitor position based on slot and other monitors */


/* Initialize keybindings - uses runtime keys if any were loaded, otherwise defaults */

/* Reset runtime config state before reload */

/* Reload config file and apply changes */

/* Timer callback to re-add config watch */

/* Handle inotify events for config file changes */

/* Set up inotify watch for config file */

void
ensure_shell_env(void)
{
	const char *shell = getenv("SHELL");
	struct passwd *pw;
	int looks_like_minimal_nix_bash = 0;

	if (shell && *shell) {
		looks_like_minimal_nix_bash = strstr(shell, "/nix/store/") &&
				strstr(shell, "bash-") &&
				!strstr(shell, "bash-interactive");
		if (!looks_like_minimal_nix_bash)
			return;
	}

	pw = getpwuid(getuid());
	if (pw && pw->pw_shell && *pw->pw_shell)
		setenv("SHELL", pw->pw_shell, 1);
}

void
setup(void)
{
	int drm_fd, i, sig[] = {SIGCHLD, SIGINT, SIGTERM, SIGPIPE};
	struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = handlesig};
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < (int)LENGTH(sig); i++)
		sigaction(sig[i], &sa, NULL);

	wlr_log_init(log_level, NULL);

	/* Make sure spawned terminals get the real login shell, not the minimal wrapper shell */
	ensure_shell_env();

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	dpy = wl_display_create();
	event_loop = wl_display_get_event_loop(dpy);
	wl_list_init(&mons);
	wl_list_init(&tray_items);
	status_timer = wl_event_loop_add_timer(event_loop, updatestatusclock, NULL);
	status_cpu_timer = wl_event_loop_add_timer(event_loop, updatestatuscpu, NULL);
	status_hover_timer = wl_event_loop_add_timer(event_loop, updatehoverfade, NULL);
	cache_update_timer = wl_event_loop_add_timer(event_loop, cache_update_timer_cb, NULL);
	nixpkgs_cache_timer = wl_event_loop_add_timer(event_loop, nixpkgs_cache_timer_cb, NULL);
	tray_init();
	fcft_initialized = fcft_init(FCFT_LOG_COLORIZE_NEVER, 0, FCFT_LOG_CLASS_ERROR);
	if (!fcft_initialized)
		die("couldn't initialize fcft");
	init_net_icon_paths();
	if (!loadstatusfont())
		die("couldn't load statusbar font");
	tray_update_icons_text();
	ensure_desktop_entries_loaded();
	backlight_available = findbacklightdevice(backlight_brightness_path,
			sizeof(backlight_brightness_path),
			backlight_max_path, sizeof(backlight_max_path));
	bluetooth_available = findbluetoothdevice();

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	if (!(backend = wlr_backend_autocreate(event_loop, &session)))
		die("couldn't create backend");

	/* Initialize the scene graph used to lay out windows */
	scene = wlr_scene_create();
	root_bg = wlr_scene_rect_create(&scene->tree, 0, 0, rootcolor);
	for (i = 0; i < NUM_LAYERS; i++)
		layers[i] = wlr_scene_tree_create(&scene->tree);
	drag_icon = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_place_below(&drag_icon->node, &layers[LyrBlock]->node);

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("couldn't create renderer");
	wl_signal_add(&drw->events.lost, &gpu_reset);

	/* Create shm, drm and linux_dmabuf interfaces by ourselves.
	 * The simplest way is to call:
	 *      wlr_renderer_init_wl_display(drw);
	 * but we need to create the linux_dmabuf interface manually to integrate it
	 * with wlr_scene. */
	wlr_renderer_init_wl_shm(drw, dpy);

	if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
		wlr_drm_create(dpy, drw);
		wlr_scene_set_linux_dmabuf_v1(scene,
				wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
	}

	if ((drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 && drw->features.timeline
			&& backend->features.timeline)
		wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't create allocator");

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the setsel() function. */
	compositor = wlr_compositor_create(dpy, 6, drw);
	wlr_subcompositor_create(dpy);
	wlr_data_device_manager_create(dpy);
	wlr_export_dmabuf_manager_v1_create(dpy);
	wlr_screencopy_manager_v1_create(dpy);
	wlr_data_control_manager_v1_create(dpy);
	wlr_primary_selection_v1_device_manager_create(dpy);
	wlr_viewporter_create(dpy);
	wlr_single_pixel_buffer_manager_v1_create(dpy);
	wlr_fractional_scale_manager_v1_create(dpy, 1);
	wlr_presentation_create(dpy, backend, 2);
	wlr_alpha_modifier_v1_create(dpy);
	content_type_mgr = wlr_content_type_manager_v1_create(dpy, 1);
	tearing_control_mgr = wlr_tearing_control_manager_v1_create(dpy, 1);

	/* Initializes the interface used to implement urgency hints */
	activation = wlr_xdg_activation_v1_create(dpy);
	wl_signal_add(&activation->events.request_activate, &request_activate);

	wlr_scene_set_gamma_control_manager_v1(scene, wlr_gamma_control_manager_v1_create(dpy));

	power_mgr = wlr_output_power_manager_v1_create(dpy);
	wl_signal_add(&power_mgr->events.set_mode, &output_power_mgr_set_mode);

	/* Creates an output layout, which is a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	output_layout = wlr_output_layout_create(dpy);
	wl_signal_add(&output_layout->events.change, &layout_change);

    wlr_xdg_output_manager_v1_create(dpy, output_layout);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_signal_add(&backend->events.new_output, &new_output);

	/* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell is a
	 * Wayland protocol which is used for application windows. For more
	 * detail on shells, refer to the article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&clients);
	wl_list_init(&fstack);
	wl_list_init(&text_inputs);

	xdg_shell = wlr_xdg_shell_create(dpy, 6);
	wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
	wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

	layer_shell = wlr_layer_shell_v1_create(dpy, 3);
	wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

	idle_notifier = wlr_idle_notifier_v1_create(dpy);

	idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
	wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &new_idle_inhibitor);

	session_lock_mgr = wlr_session_lock_manager_v1_create(dpy);
	wl_signal_add(&session_lock_mgr->events.new_lock, &new_session_lock);
	locked_bg = wlr_scene_rect_create(layers[LyrBlock], sgeom.width, sgeom.height,
			(float [4]){0.1f, 0.1f, 0.1f, 1.0f});
	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
			wlr_server_decoration_manager_create(dpy),
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
	wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration, &new_xdg_decoration);

	pointer_constraints = wlr_pointer_constraints_v1_create(dpy);
	wl_signal_add(&pointer_constraints->events.new_constraint, &new_pointer_constraint);

	relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(dpy);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). Scaled cursors will be loaded with each output. */
	cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	setenv("XCURSOR_SIZE", "24", 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	wl_signal_add(&cursor->events.motion, &cursor_motion);
	wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
	wl_signal_add(&cursor->events.button, &cursor_button);
	wl_signal_add(&cursor->events.axis, &cursor_axis);
	wl_signal_add(&cursor->events.frame, &cursor_frame);

	cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(dpy, 1);
	wl_signal_add(&cursor_shape_mgr->events.request_set_shape, &request_set_cursor_shape);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_signal_add(&backend->events.new_input, &new_input_device);
	virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
	wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
			&new_virtual_keyboard);
	virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(dpy);
    wl_signal_add(&virtual_pointer_mgr->events.new_virtual_pointer,
            &new_virtual_pointer);
	text_input_mgr = wlr_text_input_manager_v3_create(dpy);
	wl_signal_add(&text_input_mgr->events.text_input, &new_text_input);

	seat = wlr_seat_create(dpy, "seat0");
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
	wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
	wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
	wl_signal_add(&seat->events.start_drag, &start_drag);

	kb_group = createkeyboardgroup();
	wl_list_init(&kb_group->destroy.link);

	output_mgr = wlr_output_manager_v1_create(dpy);
	wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
	wl_signal_add(&output_mgr->events.test, &output_mgr_test);

	/* Make sure XWayland clients don't connect to the parent X server,
	 * e.g when running in the x11 backend or the wayland backend and the
	 * compositor has Xwayland support */
	unsetenv("DISPLAY");
#ifdef XWAYLAND
	/*
	 * Initialise the XWayland X server.
	 * It will be started when the first X client is started.
	 */
	if ((xwayland = wlr_xwayland_create(dpy, compositor, 1))) {
		wl_signal_add(&xwayland->events.ready, &xwayland_ready);
		wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

		setenv("DISPLAY", xwayland->display_name, 1);
	} else {
		wlr_log(WLR_ERROR, "failed to setup XWayland X server, continuing without it");
	}
#endif

	initial_status_refresh();
	if (status_timer)
		schedule_status_timer();
	if (status_cpu_timer) {
		init_status_refresh_tasks();
		schedule_next_status_refresh();
	}
	if (!wifi_scan_timer)
		wifi_scan_timer = wl_event_loop_add_timer(event_loop, wifi_scan_timer_cb, NULL);
	if (status_hover_timer)
		wl_event_source_timer_update(status_hover_timer, 0);
}

/* Detect all GPUs in the system */

/* Programs that should use dedicated GPU */
const char *dgpu_programs[] = {
	"steam", "gamescope", "mangohud",
	"blender", "freecad", "openscad", "kicad",
	"obs", "obs-studio", "kdenlive", "davinci-resolve",
	"godot", "unity", "unreal",
	"wine", "wine64", "proton",
	"vkcube", "vulkaninfo", "glxgears", "glxinfo",
	"darktable", "rawtherapee", "gimp",
	"prusa-slicer", "cura", "superslicer",
	NULL
};



/* Set Steam-specific environment variables */

/* Check if command is Steam */

/* Launch Steam Big Picture mode if not already running */

/* Kill Steam process immediately */

/* Kill Chrome kiosk instances used for live TV streaming */

/* Check if client is Steam main window */

/* Check if client is a Steam popup/dialog (not main window) */

/*
 * Check if a process is a child of Steam (i.e., a game launched by Steam).
 * This reads /proc/pid/stat to get the parent PID and checks recursively.
 */

/*
 * Check if client is a Steam game (launched by Steam, not Steam itself).
 * Steam games are child processes of Steam and typically:
 * - Don't have "steam" in their app_id
 * - Are not floating (popups are floating)
 * - May request fullscreen after a delay
 */

/* Check if client is a browser (Chrome, Chromium, Firefox, etc.) */

/*
 * Check if client looks like a game based on various heuristics:
 * - Launched by Steam
 * - Has game-related app_id patterns
 * - Uses content-type game hint
 * - Requests tearing (for low latency)
 */

void
spawn(const Arg *arg)
{
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();

		const char *cmd = (const char *)arg->v;
		if (!cmd || !cmd[0]) {
			_exit(1);
		}

		/* Detect if arg->v is a char** array (default keys) or string (runtime config).
		 * Runtime config strings are stored in runtime_spawn_cmds[] which are heap-allocated.
		 * Default keys use static char* arrays.
		 * We can check if the pointer is within runtime_spawn_cmds range. */
		int is_runtime_string = 0;
		for (int i = 0; i < runtime_spawn_cmd_count; i++) {
			if (arg->v == runtime_spawn_cmds[i]) {
				is_runtime_string = 1;
				break;
			}
		}

		if (!is_runtime_string) {
			/* Default keybindings: arg->v is char** array */
			char **argv = (char **)arg->v;
			if (argv[0] && argv[0][0] != '\0') {
				if (should_use_dgpu(argv[0]))
					set_dgpu_env();
				if (is_steam_cmd(argv[0])) {
					set_steam_env();
					/* Launch Steam - Big Picture in HTPC mode, normal otherwise */
					if (htpc_mode_active) {
						execlp("steam", "steam", "-bigpicture", "-cef-force-gpu", "-cef-disable-sandbox", "steam://open/games", (char *)NULL);
					} else {
						execlp("steam", "steam", "-cef-force-gpu", "-cef-disable-sandbox", (char *)NULL);
					}
				}
				execvp(argv[0], argv);
				/* If execvp fails, die */
				die("nixlytile: execvp %s failed:", argv[0]);
			}
		}

		/* Runtime config: arg->v is a string, execute via shell */
		if (should_use_dgpu(cmd))
			set_dgpu_env();
		if (is_steam_cmd(cmd)) {
			set_steam_env();
			/* If cmd is just "steam" or starts with "steam ", launch with GPU flags */
			if (strcmp(cmd, "steam") == 0) {
				/* Launch Steam - Big Picture in HTPC mode, normal otherwise */
				if (htpc_mode_active) {
					execlp("steam", "steam", "-bigpicture", "-cef-force-gpu", "-cef-disable-sandbox", "steam://open/games", (char *)NULL);
				} else {
					execlp("steam", "steam", "-cef-force-gpu", "-cef-disable-sandbox", (char *)NULL);
				}
			}
		}
		execl("/bin/sh", "sh", "-c", cmd, NULL);
		die("nixlytile: execl %s failed:", cmd);
	}
}









/* Ease-out cubic for smooth deceleration */

/* Animation timer callback - updates panel position */

/* Refresh timer callback - updates stats content */

/* Toggle stats panel visibility with slide animation */

/* Check if stats panel is visible on any monitor */

/* Handle keyboard input when stats panel is visible */

/* togglesticky moved to client.c */













/* Text input support for virtual keyboard */






/* Notify all text inputs about focus change */







#ifdef XWAYLAND
void
activatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, activate);

	/* Only "managed" windows can be activated */
	if (!client_is_unmanaged(c))
		wlr_xwayland_surface_activate(c->surface.xwayland, 1);
}

void
associatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, associate);

	LISTEN(&client_surface(c)->events.map, &c->map, mapnotify);
	LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmapnotify);
}

void
configurex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	if (!client_surface(c) || !client_surface(c)->mapped) {
		wlr_xwayland_surface_configure(c->surface.xwayland,
				event->x, event->y, event->width, event->height);
		return;
	}
	if (client_is_unmanaged(c)) {
		wlr_scene_node_set_position(&c->scene->node, event->x, event->y);
		wlr_xwayland_surface_configure(c->surface.xwayland,
				event->x, event->y, event->width, event->height);
		return;
	}
	if ((c->isfloating && c != grabc) || !c->mon->lt[c->mon->sellt]->arrange) {
		resize(c, (struct wlr_box){.x = event->x - c->bw,
				.y = event->y - c->bw, .width = event->width + c->bw * 2,
				.height = event->height + c->bw * 2}, 0);
	} else {
		arrange(c->mon);
	}
}

void
minimizenotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, minimize);
	struct wlr_xwayland_surface *xsurface = c->surface.xwayland;
	struct wlr_xwayland_minimize_event *e = data;
	int focused;

	if (xsurface->surface == NULL || !xsurface->surface->mapped)
		return;

	focused = seat->keyboard_state.focused_surface == xsurface->surface;
	wlr_xwayland_surface_set_minimized(xsurface, !focused && e->minimize);
}

void
createnotifyx11(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_surface *xsurface = data;
	Client *c;

	/* Allocate a Client for this surface */
	c = xsurface->data = ecalloc(1, sizeof(*c));
	c->surface.xwayland = xsurface;
	c->type = X11;
	c->bw = client_is_unmanaged(c) ? 0 : borderpx;

	/* Listen to the various events it can emit */
	LISTEN(&xsurface->events.associate, &c->associate, associatex11);
	LISTEN(&xsurface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociatex11);
	LISTEN(&xsurface->events.request_activate, &c->activate, activatex11);
	LISTEN(&xsurface->events.request_minimize, &c->minimize, minimizenotify);
	LISTEN(&xsurface->events.request_configure, &c->configure, configurex11);
	LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&xsurface->events.set_hints, &c->set_hints, sethints);
	LISTEN(&xsurface->events.set_title, &c->set_title, updatetitle);
}

void
dissociatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, dissociate);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
}

void
sethints(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_hints);
	struct wlr_surface *surface = client_surface(c);
	if (c == focustop(selmon) || !c->surface.xwayland->hints)
		return;

	c->isurgent = xcb_icccm_wm_hints_get_urgency(c->surface.xwayland->hints);
	printstatus();

	if (c->isurgent && surface && surface->mapped)
		client_set_border_color(c, urgentcolor);
}

void
xwaylandready(struct wl_listener *listener, void *data)
{
	struct wlr_xcursor *xcursor;

	/* assign the one and only seat */
	wlr_xwayland_set_seat(xwayland, seat);

	/* Set the default XWayland cursor to match the rest of dwl. */
	if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1)))
		wlr_xwayland_set_cursor(xwayland,
				xcursor->images[0]->buffer, xcursor->images[0]->width * 4,
				xcursor->images[0]->width, xcursor->images[0]->height,
				xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);
}
#endif

int
main(int argc, char *argv[])
{
	const char *startup_cmd = NULL;
	int c;

	while ((c = getopt(argc, argv, "s:hdv")) != -1) {
		if (c == 's')
			startup_cmd = optarg;
		else if (c == 'd')
			log_level = WLR_DEBUG;
		else if (c == 'v')
			die("nixlytile " VERSION);
		else
			goto usage;
	}
	if (optind < argc)
		goto usage;

	/* Load runtime config before applying defaults */
	load_config();
	init_keybindings();

	/* Detect available GPUs for PRIME offloading */
	detect_gpus();

	/* Build autostart command with wallpaper path if not overridden */
	{
		char expanded_wp[PATH_MAX];
		config_expand_path(wallpaper_path, expanded_wp, sizeof(expanded_wp));
		/* Only rebuild if autostart_cmd still has the default wallpaper reference */
		if (strstr(autostart_cmd, ".nixlyos/wallpapers/beach.jpg")) {
			snprintf(autostart_cmd, sizeof(autostart_cmd),
				"eval $(gnome-keyring-daemon --start --components=secrets,ssh,pkcs11) & "
				"thunar --daemon & swaybg -i \"%s\" -m fill <&-", expanded_wp);
		}
	}

	if (!startup_cmd)
		startup_cmd = autostart_cmd;

	/* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
	if (!getenv("XDG_RUNTIME_DIR"))
		die("XDG_RUNTIME_DIR must be set");
	setup();
	setup_config_watch();
	gamepad_setup();
	bt_controller_setup();
	pc_gaming_cache_watch_setup();

	/* Set GE-Proton as default Steam compatibility tool */
	steam_set_ge_proton_default();

	/* HTPC mode (nixlytile_mode == 2) is entered from updatemons()
	 * once the first monitor is ready and selmon is set */

	/* Start only git cache immediately, stagger others via timer
	 * Phase 0=git (done), 1=file (in 30s), 2=gaming (in 60s) */
	git_cache_update_start();
	cache_update_phase = 1; /* Next: file cache */
	if (cache_update_timer)
		wl_event_source_timer_update(cache_update_timer, 30000); /* 30 seconds */
	/* Generate nixpkgs cache now if missing, then schedule weekly updates */
	nixpkgs_cache_update_start();
	schedule_nixpkgs_cache_timer();
	run(startup_cmd);
	cleanup();
	return EXIT_SUCCESS;

usage:
	die("Usage: %s [-v] [-d] [-s startup command]", argv[0]);
}
