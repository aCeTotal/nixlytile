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
	/* Fixed 45s interval — consolidates wakeups for better CPU sleep.
	 * Previously random 30-60s, which spread wakeups preventing deep idle. */
	return 45000u;
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

	/* Find CompatToolMapping section */
	char *compat_section = strstr(config_content, "\"CompatToolMapping\"");
	if (compat_section) {
		/* Find the "0" entry (global default) within CompatToolMapping */
		char *zero_entry = strstr(compat_section, "\"0\"");
		if (zero_entry && zero_entry < compat_section + 500) {
			/*
			 * Check if the "0" entry already has a compatibility tool
			 * configured.  If the user has chosen ANY tool (GE-Proton,
			 * Proton Experimental, etc.), respect that choice and don't
			 * overwrite it.  Only set GE-Proton when nothing is set.
			 */
			char *brace_open = strchr(zero_entry, '{');
			if (brace_open) {
				char *name_key = strstr(brace_open, "\"name\"");
				if (name_key) {
					/* Find the value: skip "name", whitespace, opening quote */
					char *p = name_key + 6; /* past "name" */
					while (*p && (*p == ' ' || *p == '\t')) p++;
					if (*p == '"') {
						p++; /* skip opening quote */
						if (*p != '"' && *p != '\0') {
							/* Non-empty name → user already has a tool set */
							char existing[128] = {0};
							int i = 0;
							while (*p && *p != '"' && i < (int)sizeof(existing) - 1)
								existing[i++] = *p++;
							existing[i] = '\0';
							wlr_log(WLR_INFO,
								"Steam already has compatibility tool '%s' "
								"configured — preserving user choice",
								existing);
							free(config_content);
							return;
						}
					}
				}

				/* No tool set — write GE-Proton as default */
				int depth = 1;
				char *brace_close = brace_open + 1;
				while (*brace_close && depth > 0) {
					if (*brace_close == '{') depth++;
					else if (*brace_close == '}') depth--;
					brace_close++;
				}
				char new_entry[512];
				snprintf(new_entry, sizeof(new_entry),
					"{\n\t\t\t\t\t\t\"name\"\t\t\"%s\"\n"
					"\t\t\t\t\t\t\"config\"\t\t\"\"\n"
					"\t\t\t\t\t\t\"priority\"\t\t\"250\"\n"
					"\t\t\t\t\t}",
					ge_proton_name);

				size_t prefix_len = brace_open - config_content + 1;
				size_t suffix_start = brace_close - config_content;
				char *new_config = malloc(config_size + 4096);
				if (new_config) {
					memcpy(new_config, config_content, prefix_len);
					strcpy(new_config + prefix_len, new_entry + 1);
					strcat(new_config, config_content + suffix_start);

					fp = fopen(config_path, "w");
					if (fp) {
						fputs(new_config, fp);
						fclose(fp);
						wlr_log(WLR_INFO, "Set %s as default Steam compatibility tool", ge_proton_name);
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




static void close_logging(void);

void
cleanup(void)
{
	TrayItem *it, *tmp;

	wlr_log(WLR_ERROR, "cleanup() called - starting cleanup sequence");
	/* Shut down game mode background worker (unfreezes processes if needed) */
	gm_bg_cleanup();
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
	last_net_render[0] = last_fan_render[0] = '\0';
	last_clock_h = last_cpu_h = last_ram_h = last_light_h = last_volume_h = last_mic_h = last_battery_h = last_net_h = last_fan_h = 0;
	/* Stop thermal fan management and restore BIOS control */
	fan_thermal_stop();
	if (fan_thermal_timer) {
		wl_event_source_remove(fan_thermal_timer);
		fan_thermal_timer = NULL;
	}
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
		fork_detach();
		if (has_nixlytile_session_target()) {
			execvp("systemctl", (char *const[]) {
				"systemctl", "--user", "stop", "nixlytile-session.target", NULL
			});
		}
		_exit(1);
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
	cpu_cursor_buffer_destroy(cpu_cursor_buf);
	cpu_cursor_buf = NULL;
	cpu_cursor_active = 0;
	wlr_xcursor_manager_destroy(cursor_mgr);

	destroykeyboardgroup(&kb_group->destroy, NULL);

	/* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
	 * Destroy it until it's fixed on the wlroots side */
	wlr_backend_destroy(backend);

	wl_display_destroy(dpy);
	/* Destroy after the wayland display (when the monitors are already destroyed)
	   to avoid destroying them with an invalid scene output. */
	wlr_scene_node_destroy(&scene->tree.node);
	close_logging();
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
		(void)write(STDERR_FILENO, "handlesig: SIGINT received, quitting\n", 37);
		quit(NULL);
	} else if (signo == SIGTERM) {
		(void)write(STDERR_FILENO, "handlesig: SIGTERM received, quitting\n", 38);
		quit(NULL);
	} else if (signo == SIGPIPE) {
		(void)write(STDERR_FILENO, "handlesig: SIGPIPE received (ignored)\n", 38);
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

	/* Apply monitors.conf grid positions now that all monitors exist.
	 *
	 * load_monitors_conf() (called earlier) parsed the config into
	 * runtime_monitors[], and createmon() applied mode/transform/scale
	 * for each output.  However, grid-based positioning (grid=X,Y)
	 * requires ALL monitors to be present so column widths and row
	 * heights can be computed.  createmon() can't do this because
	 * monitors are created one at a time.  The grid→pixel conversion
	 * lives in reload_monitors_conf(), which was previously only called
	 * on hot-reload (inotify), not at startup. */
	if (runtime_monitor_count > 0)
		reload_monitors_conf();

	/* Now that the socket exists and the backend is started, run the startup command */
	if (startup_cmd) {
		int piperw[2];
		if (pipe(piperw) < 0)
			die("startup: pipe:");
		if ((child_pid = fork()) < 0)
			die("startup: fork:");
		if (child_pid == 0) {
			setsid();
			fork_detach();
			dup2(piperw[0], STDIN_FILENO);
			close(piperw[0]);
			close(piperw[1]);
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
			_exit(127);
		}
		dup2(piperw[1], STDOUT_FILENO);
		close(piperw[1]);
		close(piperw[0]);
	}

	/* Import environment variables then start systemd target */
	if (fork() == 0) {
		pid_t import_pid;
		setsid();

		fork_detach();

		/* First: import environment variables */
		import_pid = fork();
		if (import_pid == 0) {
			execvp("systemctl", (char *const[]) {
				"systemctl", "--user", "import-environment",
				"DISPLAY", "WAYLAND_DISPLAY", NULL
			});
			_exit(1);
		}

		/* Wait for import to complete */
		if (import_pid > 0)
			waitpid(import_pid, NULL, 0);

		/* Second: start target */
		if (has_nixlytile_session_target()) {
			execvp("systemctl", (char *const[]) {
				"systemctl", "--user", "start", "nixlytile-session.target", NULL
			});
		}

		_exit(1);
	}

	/* Mark stdout as non-blocking to avoid the startup script
	 * causing dwl to freeze when a user neither closes stdin
	 * nor consumes standard input in his startup script */

	if (fd_set_nonblock(STDOUT_FILENO) < 0)
		close(STDOUT_FILENO);

	printstatus();

	/* At this point the outputs are initialized.  Place the cursor on the
	 * center monitor (odd count) or the highest-resolution monitor (even
	 * count / 2 monitors) so that the user always starts on the most
	 * useful screen. */
	warp_cursor_to_startup_monitor();
	nixly_cursor_set_xcursor("default");

	/* Per-monitor summary to game debug log */
	{
		Monitor *m;
		wl_list_for_each(m, &mons, link) {
			if (!m->wlr_output)
				continue;
			game_log("MONITOR: name='%s' %dx%d@%.2fHz "
				"vrr=%s ll_cursor=%s cpu_cursor=%s",
				m->wlr_output->name,
				m->m.width, m->m.height,
				m->wlr_output->current_mode
					? m->wlr_output->current_mode->refresh / 1000.0
					: 0.0,
				m->vrr_capable ? "capable" : "no",
				m->ll_cursor_active ? "active" : "no",
				cpu_cursor_active ? "yes" : "no");
		}
	}

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

	wlr_log(WLR_DEBUG, "Evaluating video modes for %.3f Hz on %s:", video_hz, m->wlr_output->name);

	/* Option 1: VRR (best quality if available) */
	if (m->vrr_capable && fullscreen_adaptive_sync_enabled) {
		candidate.method = 3;
		candidate.mode = NULL;
		candidate.multiplier = 1;
		candidate.target_hz = video_hz;
		candidate.actual_hz = video_hz;
		candidate.judder_ms = 0.0f;
		candidate.score = score_video_mode(3, video_hz, video_hz, 1);

		wlr_log(WLR_DEBUG, "  VRR: score=%.1f (judder=0ms)", candidate.score);

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

		wlr_log(WLR_DEBUG, "  Mode %d.%03dHz: %dx mult, score=%.1f, judder=%.2fms",
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

			wlr_log(WLR_DEBUG, "  CVT %.3fHz: %dx mult, score=%.1f (theoretical)",
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
		wlr_log(WLR_DEBUG, "Best option: %s at %.3f Hz (%dx), score=%.1f, judder=%.2fms",
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
	{ "screenshot_begin", screenshot_begin, 0 },
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

static void
log_callback(enum wlr_log_importance importance, const char *fmt, va_list args)
{
	if (!log_file)
		return;

	struct timespec ts;
	struct tm tm;
	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);

	const char *level = "???";
	switch (importance) {
	case WLR_ERROR: level = "ERROR"; break;
	case WLR_INFO:  level = "INFO";  break;
	case WLR_DEBUG: level = "DEBUG"; break;
	default: break;
	}

	char prefix[64];
	snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d.%03ld] [%s] ",
		tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, level);

	/* Always write to log file */
	va_list args_copy;
	va_copy(args_copy, args);
	fprintf(log_file, "%s", prefix);
	vfprintf(log_file, fmt, args_copy);
	fputc('\n', log_file);
	fflush(log_file);
	va_end(args_copy);

	/* Always write to WLR_DEBUG.log (full debug capture) */
	if (debug_log_file) {
		va_copy(args_copy, args);
		fprintf(debug_log_file, "%s", prefix);
		vfprintf(debug_log_file, fmt, args_copy);
		fputc('\n', debug_log_file);
		fflush(debug_log_file);
		va_end(args_copy);
	}

	/* Also print to real terminal when -d flag is used */
	if (log_stderr_fd >= 0) {
		dprintf(log_stderr_fd, "%s", prefix);
		va_copy(args_copy, args);
		vdprintf(log_stderr_fd, fmt, args_copy);
		va_end(args_copy);
		dprintf(log_stderr_fd, "\n");
	}
}

/* ================================================================
 *  Diagnostics Logging System
 *  Periodic (5s) structured logging to /tmp/nixlylogging/
 * ================================================================ */

/* Per-thread CPU snapshot (file-static) */
struct diag_thread_snap {
	pid_t tid;
	char name[32];
	unsigned long long utime, stime;
};
static struct diag_thread_snap *diag_prev_threads = NULL;  /* Lazy: allocated on first use */
static int diag_prev_thread_count;
static unsigned long long diag_prev_total_cpu;

/* Per-system-process CPU snapshot */
struct diag_proc_snap { pid_t pid; char name[64]; unsigned long long ticks; };
static struct diag_proc_snap *diag_prev_procs = NULL;  /* Lazy: allocated on first use */
static int diag_prev_proc_count;

/* I/O snapshot */
static unsigned long long diag_prev_read_bytes, diag_prev_write_bytes;
static uint64_t diag_prev_io_time_ms;

static void
diag_log_cpu_breakdown(void)
{
	if (diag_log_fd < 0) return;

	/* Lazy-allocate diagnostic snapshot arrays on first use */
	if (!diag_prev_threads) {
		diag_prev_threads = calloc(64, sizeof(*diag_prev_threads));
		if (!diag_prev_threads) return;
	}
	if (!diag_prev_procs) {
		diag_prev_procs = calloc(128, sizeof(*diag_prev_procs));
		if (!diag_prev_procs) return;
	}

	struct timespec now_ts;
	clock_gettime(CLOCK_REALTIME, &now_ts);
	struct tm tm;
	localtime_r(&now_ts.tv_sec, &tm);

	/* Read total system CPU from /proc/stat */
	unsigned long long total_cpu = 0;
	{
		FILE *f = fopen("/proc/stat", "r");
		if (f) {
			char line[256];
			if (fgets(line, sizeof(line), f)) {
				unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
				if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
					&user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) >= 4) {
					total_cpu = user + nice + system + idle + iowait + irq + softirq + steal;
				}
			}
			fclose(f);
		}
	}

	unsigned long long delta_total = total_cpu - diag_prev_total_cpu;
	if (delta_total == 0) delta_total = 1;

	/* Read per-thread CPU for our process */
	struct diag_thread_snap cur_threads[64];
	int cur_count = 0;
	{
		DIR *d = opendir("/proc/self/task");
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) && cur_count < 64) {
				if (de->d_name[0] == '.') continue;
				pid_t tid = atoi(de->d_name);
				char path[128], buf[512];
				int n;

				/* Read thread name */
				snprintf(path, sizeof(path), "/proc/self/task/%s/comm", de->d_name);
				FILE *f = fopen(path, "r");
				if (f) {
					if (fgets(cur_threads[cur_count].name, sizeof(cur_threads[cur_count].name), f)) {
						char *nl = strchr(cur_threads[cur_count].name, '\n');
						if (nl) *nl = '\0';
					}
					fclose(f);
				} else {
					snprintf(cur_threads[cur_count].name, sizeof(cur_threads[cur_count].name), "tid-%d", tid);
				}

				/* Read thread CPU times from stat */
				snprintf(path, sizeof(path), "/proc/self/task/%s/stat", de->d_name);
				f = fopen(path, "r");
				if (f) {
					n = fread(buf, 1, sizeof(buf)-1, f);
					buf[n] = '\0';
					fclose(f);
					/* Skip past the comm field (second field in parentheses) */
					char *p = strrchr(buf, ')');
					if (p) {
						unsigned long long utime = 0, stime = 0;
						/* Fields after ')': state, ppid, pgrp, session, tty_nr, tpgid,
						   flags, minflt, cminflt, majflt, cmajflt, utime, stime */
						if (sscanf(p+2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
							&utime, &stime) == 2) {
							cur_threads[cur_count].tid = tid;
							cur_threads[cur_count].utime = utime;
							cur_threads[cur_count].stime = stime;
							cur_count++;
						}
					}
				}
			}
			closedir(d);
		}
	}

	/* Write CPU header */
	char out[4096];
	int off = 0;
	off += snprintf(out+off, sizeof(out)-off,
		"\n[%02d:%02d:%02d] === CPU (5s) ===\n",
		tm.tm_hour, tm.tm_min, tm.tm_sec);

	/* Calculate process total */
	unsigned long long proc_delta = 0;
	for (int i = 0; i < cur_count; i++) {
		for (int j = 0; j < diag_prev_thread_count; j++) {
			if (cur_threads[i].tid == diag_prev_threads[j].tid) {
				proc_delta += (cur_threads[i].utime + cur_threads[i].stime)
					- (diag_prev_threads[j].utime + diag_prev_threads[j].stime);
				break;
			}
		}
	}
	off += snprintf(out+off, sizeof(out)-off,
		"  nixlytile total  : %5.1f%%\n",
		100.0 * proc_delta / delta_total);

	/* Per-thread breakdown */
	for (int i = 0; i < cur_count && off < (int)sizeof(out)-100; i++) {
		unsigned long long tdelta = 0;
		for (int j = 0; j < diag_prev_thread_count; j++) {
			if (cur_threads[i].tid == diag_prev_threads[j].tid) {
				tdelta = (cur_threads[i].utime + cur_threads[i].stime)
					- (diag_prev_threads[j].utime + diag_prev_threads[j].stime);
				break;
			}
		}
		double pct = 100.0 * tdelta / delta_total;
		if (pct >= 0.1)
			off += snprintf(out+off, sizeof(out)-off,
				"    %-15s: %5.1f%%\n", cur_threads[i].name, pct);
	}

	/* System top 5 processes */
	struct { pid_t pid; char name[64]; unsigned long long ticks; } cur_procs[128];
	int cur_pcount = 0;
	{
		DIR *d = opendir("/proc");
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) && cur_pcount < 128) {
				if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
				pid_t pid = atoi(de->d_name);
				char path[128], buf[512];
				int n;
				snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
				FILE *f = fopen(path, "r");
				if (!f) continue;
				n = fread(buf, 1, sizeof(buf)-1, f);
				buf[n] = '\0';
				fclose(f);
				/* Extract name from (comm) */
				char *lp = strchr(buf, '(');
				char *rp = strrchr(buf, ')');
				if (!lp || !rp) continue;
				int nlen = rp - lp - 1;
				if (nlen >= 64) nlen = 63;
				memcpy(cur_procs[cur_pcount].name, lp+1, nlen);
				cur_procs[cur_pcount].name[nlen] = '\0';
				unsigned long long utime = 0, stime = 0;
				if (sscanf(rp+2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
					&utime, &stime) == 2) {
					cur_procs[cur_pcount].pid = pid;
					cur_procs[cur_pcount].ticks = utime + stime;
					cur_pcount++;
				}
			}
			closedir(d);
		}
	}

	/* Compute deltas and find top 5 */
	struct { double pct; pid_t pid; const char *name; } top[5] = {{0}};
	for (int i = 0; i < cur_pcount; i++) {
		unsigned long long pdelta = 0;
		for (int j = 0; j < diag_prev_proc_count; j++) {
			if (cur_procs[i].pid == diag_prev_procs[j].pid) {
				pdelta = cur_procs[i].ticks - diag_prev_procs[j].ticks;
				break;
			}
		}
		double pct = 100.0 * pdelta / delta_total;
		/* Skip self (nixlytile) */
		if (cur_procs[i].pid == getpid()) continue;
		for (int t = 0; t < 5; t++) {
			if (pct > top[t].pct) {
				for (int s = 4; s > t; s--) top[s] = top[s-1];
				top[t].pct = pct;
				top[t].pid = cur_procs[i].pid;
				top[t].name = cur_procs[i].name;
				break;
			}
		}
	}

	off += snprintf(out+off, sizeof(out)-off, "  --- System Top 5 ---\n");
	for (int i = 0; i < 5 && top[i].pct >= 0.5 && off < (int)sizeof(out)-80; i++) {
		off += snprintf(out+off, sizeof(out)-off,
			"    %-15s: %5.1f%%  (pid=%d)\n",
			top[i].name, top[i].pct, top[i].pid);
	}

	(void)!write(diag_log_fd, out, off);

	/* Save snapshots */
	memcpy(diag_prev_threads, cur_threads, sizeof(cur_threads[0]) * cur_count);
	diag_prev_thread_count = cur_count;
	diag_prev_total_cpu = total_cpu;
	memcpy(diag_prev_procs, cur_procs, sizeof(cur_procs[0]) * cur_pcount);
	diag_prev_proc_count = cur_pcount;
}

static void
diag_log_io_stats(void)
{
	if (diag_log_fd < 0) return;

	struct timespec now_ts;
	clock_gettime(CLOCK_REALTIME, &now_ts);
	struct tm tm;
	localtime_r(&now_ts.tv_sec, &tm);
	uint64_t now_ms = now_ts.tv_sec * 1000ULL + now_ts.tv_nsec / 1000000ULL;

	unsigned long long read_bytes = 0, write_bytes = 0;
	FILE *f = fopen("/proc/self/io", "r");
	if (f) {
		char line[128];
		while (fgets(line, sizeof(line), f)) {
			if (strncmp(line, "read_bytes:", 11) == 0)
				sscanf(line+11, "%llu", &read_bytes);
			else if (strncmp(line, "write_bytes:", 12) == 0)
				sscanf(line+12, "%llu", &write_bytes);
		}
		fclose(f);
	}

	uint64_t dt_ms = now_ms - diag_prev_io_time_ms;
	if (dt_ms == 0) dt_ms = 1;
	double read_mbs = (double)(read_bytes - diag_prev_read_bytes) / (dt_ms / 1000.0) / (1024*1024);
	double write_mbs = (double)(write_bytes - diag_prev_write_bytes) / (dt_ms / 1000.0) / (1024*1024);

	char out[256];
	int off = snprintf(out, sizeof(out),
		"[%02d:%02d:%02d] === I/O (5s) ===\n"
		"  Disk: read=%.2f MB/s  write=%.2f MB/s\n"
		"  Net:  down=%.1f Mbps  up=%.1f Mbps  (%s)\n",
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		read_mbs, write_mbs,
		net_last_down_bps > 0 ? net_last_down_bps / 1e6 : 0.0,
		net_last_up_bps > 0 ? net_last_up_bps / 1e6 : 0.0,
		net_iface[0] ? net_iface : "none");

	(void)!write(diag_log_fd, out, off);

	diag_prev_read_bytes = read_bytes;
	diag_prev_write_bytes = write_bytes;
	diag_prev_io_time_ms = now_ms;
}

static void
diag_log_nvidia(void)
{
	if (diag_log_fd < 0) return;
	if (discrete_gpu_idx < 0 || detected_gpus[discrete_gpu_idx].vendor != GPU_VENDOR_NVIDIA)
		return;

	struct timespec now_ts;
	struct tm tm;
	clock_gettime(CLOCK_REALTIME, &now_ts);
	localtime_r(&now_ts.tv_sec, &tm);

	/* Query multiple fields in one nvidia-smi call */
	char buf[512] = "";
	FILE *p = popen("nvidia-smi --query-gpu=utilization.gpu,utilization.memory,"
		"temperature.gpu,clocks.current.graphics,clocks.current.memory,"
		"power.draw,power.limit,pstate "
		"--format=csv,noheader,nounits 2>/dev/null", "r");
	if (!p) {
		diag_log_error("NVIDIA", "nvidia-smi popen failed");
		return;
	}
	if (!fgets(buf, sizeof(buf), p)) {
		pclose(p);
		diag_log_error("NVIDIA", "nvidia-smi query failed (no output)");
		return;
	}
	pclose(p);

	/* Parse CSV: gpu_util, mem_util, temp, gpu_clk, mem_clk, power, power_limit, pstate */
	int gpu_util = 0, mem_util = 0, temp = 0, gpu_clk = 0, mem_clk = 0;
	float power = 0, power_limit = -1;
	char pstate[8] = "";

	/* Use strtok to handle [N/A] fields from nvidia-smi */
	char *fields[8];
	int nfields = 0;
	char *saveptr = NULL;
	for (char *tok = strtok_r(buf, ",", &saveptr); tok && nfields < 8;
	     tok = strtok_r(NULL, ",", &saveptr)) {
		while (*tok == ' ') tok++;
		fields[nfields++] = tok;
	}
	if (nfields < 8) {
		diag_log_error("NVIDIA", "nvidia-smi parse failed (got %d fields): %s", nfields, buf);
		return;
	}
	gpu_util = atoi(fields[0]);
	mem_util = atoi(fields[1]);
	temp     = atoi(fields[2]);
	gpu_clk  = atoi(fields[3]);
	mem_clk  = atoi(fields[4]);
	power    = strtof(fields[5], NULL);
	if (strstr(fields[6], "[N/A]") == NULL)
		power_limit = strtof(fields[6], NULL);
	snprintf(pstate, sizeof(pstate), "%s", fields[7]);
	/* Strip trailing whitespace from pstate */
	for (int i = strlen(pstate)-1; i >= 0 && (pstate[i] == '\n' || pstate[i] == ' '); i--)
		pstate[i] = '\0';

	char power_str[32];
	if (power_limit < 0)
		snprintf(power_str, sizeof(power_str), "%.0f/N/A W", power);
	else
		snprintf(power_str, sizeof(power_str), "%.0f/%.0f W", power, power_limit);

	char out[256];
	int off = snprintf(out, sizeof(out),
		"[%02d:%02d:%02d] === NVIDIA GPU ===\n"
		"  Util: %d%% GPU, %d%% VRAM | Temp: %d°C\n"
		"  Clocks: %d/%d MHz | Power: %s | %s\n",
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		gpu_util, mem_util, temp,
		gpu_clk, mem_clk, power_str, pstate);
	(void)!write(diag_log_fd, out, off);

	/* Error conditions */
	if (temp >= 90)
		diag_log_error("NVIDIA", "GPU temp critical: %d°C (threshold: 90°C)", temp);
	if (game_mode_active && pstate[0] == 'P' && pstate[1] >= '5')
		diag_log_error("NVIDIA", "Unexpected PState %s during game mode (expected P0-P2)", pstate);
}

static void
diag_log_audio_summary(void)
{
	if (diag_log_fd < 0) return;

	struct timespec now_ts;
	struct tm tm;
	clock_gettime(CLOCK_REALTIME, &now_ts);
	localtime_r(&now_ts.tv_sec, &tm);

	VideoPlayer *vp = active_videoplayer;
	if (!vp || vp->state == VP_STATE_IDLE) {
		char out[128];
		int off = snprintf(out, sizeof(out),
			"[%02d:%02d:%02d] === Audio ===\n  No active player\n",
			tm.tm_hour, tm.tm_min, tm.tm_sec);
		(void)!write(diag_log_fd, out, off);
		return;
	}

	size_t ring_avail = 0;
	size_t ring_size = vp->audio.ring.size;
	pthread_mutex_lock(&vp->audio.ring.lock);
	ring_avail = vp->audio.ring.available;
	pthread_mutex_unlock(&vp->audio.ring.lock);

	int ring_pct = ring_size ? (int)(100ULL * ring_avail / ring_size) : 0;
	const char *state_str = vp->audio.stream_interrupted ? "INTERRUPTED" :
		(vp->state == VP_STATE_PLAYING ? "STREAMING" : "PAUSED");

	char out[256];
	int off = snprintf(out, sizeof(out),
		"[%02d:%02d:%02d] === Audio ===\n"
		"  Ring: %zu/%zu (%d%%) | Underruns: %d | Stalls: %d\n"
		"  State: %s | Rate: %d Hz | Ch: %d\n",
		tm.tm_hour, tm.tm_min, tm.tm_sec,
		ring_avail, ring_size, ring_pct,
		vp->debug_audio_underruns, vp->audio.write_stall_count,
		state_str, vp->audio.sample_rate, vp->audio.channels);
	(void)!write(diag_log_fd, out, off);
}

static int
diag_timer_cb(void *data)
{
	(void)data;

	/* Stop diagnostics timer if logging is no longer active */
	if (diag_log_fd < 0)
		return 0;

	diag_log_cpu_breakdown();
	diag_log_io_stats();
	diag_log_nvidia();
	diag_log_audio_summary();

	/* Reschedule at 10s instead of 5s — halves wakeups while still useful */
	if (diag_timer)
		wl_event_source_timer_update(diag_timer, 10000);
	return 0;
}

static void
init_logging(void)
{
	#define NIXLY_LOG_DIR "/tmp/nixlylogging"
	mkdir(NIXLY_LOG_DIR, 0755);

	/* Save original stderr before redirect */
	log_stderr_fd = dup(STDERR_FILENO);

	/* Open wlroots log file */
	log_file = fopen(NIXLY_LOG_DIR "/wlroots.log", "w");
	if (log_file) {
		struct timespec ts;
		struct tm tm;
		clock_gettime(CLOCK_REALTIME, &ts);
		localtime_r(&ts.tv_sec, &tm);
		fprintf(log_file, "=== nixlytile started %04d-%02d-%02d %02d:%02d:%02d ===\n",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
		fflush(log_file);
	}

	/* Open full debug log (always active, captures everything) */
	debug_log_file = fopen(NIXLY_LOG_DIR "/WLR_DEBUG.log", "w");
	if (debug_log_file) {
		struct timespec ts;
		struct tm tm;
		clock_gettime(CLOCK_REALTIME, &ts);
		localtime_r(&ts.tv_sec, &tm);
		fprintf(debug_log_file, "=== WLR_DEBUG full log %04d-%02d-%02d %02d:%02d:%02d (PID %d) ===\n",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec, getpid());
		fflush(debug_log_file);
	}

	/* Redirect stderr → file so XWayland crash output is captured.
	 * XWayland is a child process that inherits our stderr. */
	int stderr_log = open(NIXLY_LOG_DIR "/xwayland.log",
		O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (stderr_log >= 0) {
		/* Write header */
		struct timespec ts;
		struct tm tm;
		clock_gettime(CLOCK_REALTIME, &ts);
		localtime_r(&ts.tv_sec, &tm);
		dprintf(stderr_log, "=== stderr/xwayland log started %04d-%02d-%02d %02d:%02d:%02d ===\n",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
		dup2(stderr_log, STDERR_FILENO);
		close(stderr_log);
	}
	/* Open diagnostics log files */
	{
		struct timespec ts;
		struct tm tm;
		clock_gettime(CLOCK_REALTIME, &ts);
		localtime_r(&ts.tv_sec, &tm);
		char hdr[256];

		diag_log_fd = open(NIXLY_LOG_DIR "/diagnostics.log",
			O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (diag_log_fd >= 0) {
			int n = snprintf(hdr, sizeof(hdr),
				"=== nixlytile diagnostics %04d-%02d-%02d %02d:%02d:%02d (PID %d) ===\n",
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
				tm.tm_hour, tm.tm_min, tm.tm_sec, getpid());
			(void)!write(diag_log_fd, hdr, n);
		}

		audio_log_fd = open(NIXLY_LOG_DIR "/audio.log",
			O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (audio_log_fd >= 0) {
			int n = snprintf(hdr, sizeof(hdr),
				"=== audio log %04d-%02d-%02d %02d:%02d:%02d ===\n",
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
				tm.tm_hour, tm.tm_min, tm.tm_sec);
			(void)!write(audio_log_fd, hdr, n);
		}

		error_log_fd = open(NIXLY_LOG_DIR "/errors.log",
			O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (error_log_fd >= 0) {
			int n = snprintf(hdr, sizeof(hdr),
				"=== error log %04d-%02d-%02d %02d:%02d:%02d ===\n",
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
				tm.tm_hour, tm.tm_min, tm.tm_sec);
			(void)!write(error_log_fd, hdr, n);
		}

		game_log_fd = open(NIXLY_LOG_DIR "/game_debug.log",
			O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (game_log_fd >= 0) {
			int n = snprintf(hdr, sizeof(hdr),
				"=== game debug log %04d-%02d-%02d %02d:%02d:%02d (PID %d) ===\n",
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
				tm.tm_hour, tm.tm_min, tm.tm_sec, getpid());
			(void)!write(game_log_fd, hdr, n);
		}
	}

	#undef NIXLY_LOG_DIR
}

static void
close_logging(void)
{
	if (diag_timer) {
		wl_event_source_remove(diag_timer);
		diag_timer = NULL;
	}
	if (diag_log_fd >= 0) {
		dprintf(diag_log_fd, "\n=== diagnostics ended ===\n");
		close(diag_log_fd);
		diag_log_fd = -1;
	}
	if (audio_log_fd >= 0) {
		dprintf(audio_log_fd, "\n=== audio log ended ===\n");
		close(audio_log_fd);
		audio_log_fd = -1;
	}
	if (error_log_fd >= 0) {
		dprintf(error_log_fd, "\n=== error log ended ===\n");
		close(error_log_fd);
		error_log_fd = -1;
	}
	if (game_log_fd >= 0) {
		dprintf(game_log_fd, "\n=== game debug log ended ===\n");
		close(game_log_fd);
		game_log_fd = -1;
	}
	if (debug_log_file) {
		fprintf(debug_log_file, "=== WLR_DEBUG log ended ===\n");
		fclose(debug_log_file);
		debug_log_file = NULL;
	}
	if (log_file) {
		fclose(log_file);
		log_file = NULL;
	}
	if (log_stderr_fd >= 0) {
		dup2(log_stderr_fd, STDERR_FILENO);
		close(log_stderr_fd);
		log_stderr_fd = -1;
	}
}

void
setup(void)
{
	int drm_fd, i, sig[] = {SIGCHLD, SIGINT, SIGTERM, SIGPIPE};
	struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = handlesig};
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < (int)LENGTH(sig); i++)
		sigaction(sig[i], &sa, NULL);

	init_logging();
	pthread_setname_np(pthread_self(), "nixlyOS");
	wlr_log_init(WLR_DEBUG, log_callback);

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
	diag_timer = wl_event_loop_add_timer(event_loop, diag_timer_cb, NULL);
	fan_thermal_start();
	gm_bg_init();
	netlink_monitor_setup();
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

	/* Detect GPUs early — env vars (WLR_DRM_NO_ATOMIC, GBM_BACKEND etc.)
	 * and WLR_DRM_DEVICES filtering must happen before backend creation.
	 * Also sets up dGPU power management (D3cold prevention). */
	detect_gpus();
	filter_igpu_without_display();
	dgpu_power_watchdog_start();

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

	/* Autocreates a Vulkan renderer. The build only links the Vulkan
	 * backend — Pixman/GL are not available. The renderer is responsible
	 * for defining the various pixel formats it supports for shared
	 * memory, this configures that for clients. */
	if (!(drw = wlr_renderer_autocreate(backend)))
		die("couldn't create renderer");
	if (!wlr_renderer_is_vk(drw))
		die("renderer is not Vulkan — only Vulkan is supported");
	wl_signal_add(&drw->events.lost, &gpu_reset);

	/* Log renderer and GPU state for diagnostics */
	{
		int is_nvidia = discrete_gpu_idx >= 0 &&
			detected_gpus[discrete_gpu_idx].vendor == GPU_VENDOR_NVIDIA;
		drm_fd = wlr_renderer_get_drm_fd(drw);
		if (is_nvidia) {
			wlr_log(WLR_INFO,
				"NVIDIA: renderer created (drm_fd=%d), "
				"timeline=%d, backend_timeline=%d",
				drm_fd,
				drw->features.timeline,
				backend->features.timeline);
		}
	}

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
	} else {
		wlr_log(WLR_ERROR,
			"Renderer has NO DMA-BUF support — "
			"Xwayland zero-copy buffer sharing will not work");
		if (discrete_gpu_idx >= 0) {
			switch (detected_gpus[discrete_gpu_idx].vendor) {
			case GPU_VENDOR_NVIDIA:
				wlr_log(WLR_ERROR,
					"NVIDIA: check that nvidia-drm.modeset=1 is set "
					"and the Nvidia EGL/GBM stack is installed");
				break;
			case GPU_VENDOR_AMD:
				wlr_log(WLR_ERROR,
					"AMD: check that mesa is installed and "
					"amdgpu kernel module is loaded");
				break;
			case GPU_VENDOR_INTEL:
				wlr_log(WLR_ERROR,
					"Intel: check that mesa is installed and "
					"i915/xe kernel module is loaded");
				break;
			default:
				break;
			}
		} else if (integrated_gpu_idx >= 0) {
			wlr_log(WLR_ERROR,
				"Using integrated GPU (%s) — ensure mesa/GL "
				"libraries are installed",
				detected_gpus[integrated_gpu_idx].driver);
		}
	}

	g_explicit_sync_ok = 0;
	if ((drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 && drw->features.timeline
			&& backend->features.timeline) {
		wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);
		wlr_log(WLR_INFO, "Explicit sync (DRM syncobj) enabled");
		g_explicit_sync_ok = 1;
	} else {
		wlr_log(WLR_INFO,
			"Explicit sync NOT available (drm_fd=%d, "
			"renderer_timeline=%d, backend_timeline=%d)",
			drm_fd, drw->features.timeline, backend->features.timeline);
		if (discrete_gpu_idx >= 0 &&
		    detected_gpus[discrete_gpu_idx].vendor == GPU_VENDOR_NVIDIA) {
			wlr_log(WLR_ERROR,
				"NVIDIA: without explicit sync, Xwayland apps may flicker. "
				"Upgrade to Nvidia 555+ driver for DRM syncobj support.");
		}
	}

	/*
	 * Gaming capability summary.
	 *
	 * Consolidated single-line status for diagnosing Vulkan game
	 * performance issues. This is the first place to look when games
	 * are stuttering — tells you at a glance whether the low-level
	 * stack is in a known-good state.
	 *
	 * Key flags:
	 *   ESYNC   — DRM syncobj explicit sync. Required for tear-free
	 *             NVIDIA Xwayland and modern Wayland explicit-sync clients.
	 *             NVIDIA needs driver 555+.
	 *   DMABUF  — DMA-BUF texture import. Required for zero-copy
	 *             client buffer sharing.
	 *   VULKAN  — Renderer is Vulkan-based. Gives access to the
	 *             persistent pipeline cache added in this tree.
	 *   10BIT   — HDR/10-bit render format supported by the renderer.
	 */
	{
		const char *renderer_name =
			wlr_renderer_is_vk(drw) ? "Vulkan" : "other";
		int has_dmabuf =
			wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF) != NULL;
		const char *gpu_label = "unknown";
		if (discrete_gpu_idx >= 0) {
			switch (detected_gpus[discrete_gpu_idx].vendor) {
			case GPU_VENDOR_NVIDIA: gpu_label = "NVIDIA"; break;
			case GPU_VENDOR_AMD:    gpu_label = "AMD";    break;
			case GPU_VENDOR_INTEL:  gpu_label = "Intel";  break;
			default: gpu_label = "other"; break;
			}
		} else if (integrated_gpu_idx >= 0) {
			switch (detected_gpus[integrated_gpu_idx].vendor) {
			case GPU_VENDOR_AMD:    gpu_label = "AMD (iGPU)";    break;
			case GPU_VENDOR_INTEL:  gpu_label = "Intel (iGPU)";  break;
			default: gpu_label = "iGPU other"; break;
			}
		}

		wlr_log(WLR_INFO,
			"Gaming stack ready: gpu=%s renderer=%s esync=%s dmabuf=%s",
			gpu_label,
			renderer_name,
			g_explicit_sync_ok ? "YES" : "NO",
			has_dmabuf ? "YES" : "NO");

		/* Per-vendor hints */
		if (discrete_gpu_idx >= 0 &&
		    detected_gpus[discrete_gpu_idx].vendor == GPU_VENDOR_NVIDIA) {
			if (!g_explicit_sync_ok) {
				wlr_log(WLR_ERROR,
					"NVIDIA: game performance will be degraded without "
					"explicit sync. Check: 1) driver >= 555, "
					"2) nvidia-drm.modeset=1, 3) nvidia-drm.fbdev=1");
			}
		}
	}

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
	ext_data_control_mgr = wlr_ext_data_control_manager_v1_create(dpy, 1);
	wlr_primary_selection_v1_device_manager_create(dpy);
	wlr_viewporter_create(dpy);
	wlr_single_pixel_buffer_manager_v1_create(dpy);
	wlr_fractional_scale_manager_v1_create(dpy, 1);
	wlr_presentation_create(dpy, backend, 2);
	wlr_alpha_modifier_v1_create(dpy);
	content_type_mgr = wlr_content_type_manager_v1_create(dpy, 1);
	tearing_control_mgr = wlr_tearing_control_manager_v1_create(dpy, 1);
	protocol_fixes = wlr_fixes_create(dpy, 1);
	security_ctx_mgr = wlr_security_context_manager_v1_create(dpy);
	xdg_dialog_mgr = wlr_xdg_wm_dialog_v1_create(dpy, 1);
	system_bell = wlr_xdg_system_bell_v1_create(dpy, 1);
	pointer_gestures = wlr_pointer_gestures_v1_create(dpy);

	/* xdg-foreign v2 — cross-process window parenting (Firefox file dialogs) */
	foreign_registry = wlr_xdg_foreign_registry_create(dpy);
	xdg_foreign = wlr_xdg_foreign_v2_create(dpy, foreign_registry);

	/* Keyboard shortcuts inhibitor — let fullscreen games capture all keys */
	kb_shortcuts_inhibit_mgr = wlr_keyboard_shortcuts_inhibit_v1_create(dpy);
	wl_signal_add(&kb_shortcuts_inhibit_mgr->events.new_inhibitor,
		&new_kb_shortcuts_inhibitor);

	/* Foreign toplevel list — expose window list to external tools */
	foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(dpy, 1);

	/* Modern screen capture (ext-image-copy-capture-v1) */
	image_copy_capture_mgr = wlr_ext_image_copy_capture_manager_v1_create(dpy, 1);
	wlr_ext_output_image_capture_source_manager_v1_create(dpy, 1);

	/* Initializes the interface used to implement urgency hints */
	activation = wlr_xdg_activation_v1_create(dpy);
	wl_signal_add(&activation->events.request_activate, &request_activate);

	wlr_scene_set_gamma_control_manager_v1(scene, wlr_gamma_control_manager_v1_create(dpy));

	/* Color management v1 — let apps negotiate color spaces (sRGB, P3, BT.2020, HDR PQ) */
	{
		enum wp_color_manager_v1_render_intent intents[] = {
			WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
			WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE,
			WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE_BPC,
		};
		enum wp_color_manager_v1_transfer_function *tfs = NULL;
		size_t tfs_len = 0;
		enum wp_color_manager_v1_primaries *pris = NULL;
		size_t pris_len = 0;
		tfs = wlr_color_manager_v1_transfer_function_list_from_renderer(drw, &tfs_len);
		pris = wlr_color_manager_v1_primaries_list_from_renderer(drw, &pris_len);
		struct wlr_color_manager_v1_options opts = {
			.features = {
				.icc_v2_v4 = true,
				.parametric = true,
				.set_primaries = true,
				.set_luminances = true,
				.set_mastering_display_primaries = true,
			},
			.render_intents = intents,
			.render_intents_len = sizeof(intents) / sizeof(intents[0]),
			.transfer_functions = tfs,
			.transfer_functions_len = tfs_len,
			.primaries = pris,
			.primaries_len = pris_len,
		};
		color_mgr = wlr_color_manager_v1_create(dpy, 1, &opts);
		free(tfs);
		free(pris);
		wlr_scene_set_color_manager_v1(scene, color_mgr);
	}

	/* Color representation v1 — YCbCr coefficients, alpha modes */
	color_repr_mgr = wlr_color_representation_manager_v1_create_with_renderer(
		dpy, 1, drw);

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

	/* Initialize CPU cursor buffer for Nvidia HW cursor plane.
	 * NVIDIA's proprietary driver doesn't support DRM_IOCTL_MODE_CREATE_DUMB
	 * on its render node, so prefer the iGPU for dumb buffer allocation.
	 * The DMA-BUF export works cross-GPU.
	 *
	 * Prefer the iGPU card (primary) node over the render node —
	 * DRM_IOCTL_MODE_CREATE_DUMB requires DRM_AUTH which the render
	 * node may not grant (EPERM on Linux 6.x with some drivers). */
	if (discrete_gpu_idx >= 0 &&
	    detected_gpus[discrete_gpu_idx].vendor == GPU_VENDOR_NVIDIA) {
		int cdrm_fd = -1;
		int cdrm_owns = 0;
		uint64_t cursor_w = 64, cursor_h = 64;

		/* Try iGPU card (primary) node first — compositor has DRM
		 * master for it, so CREATE_DUMB always succeeds. */
		if (integrated_gpu_idx >= 0 &&
		    detected_gpus[integrated_gpu_idx].card_path[0]) {
			cdrm_fd = open(detected_gpus[integrated_gpu_idx].card_path,
				O_RDWR | O_CLOEXEC);
			if (cdrm_fd >= 0) {
				cdrm_owns = 1;
				wlr_log(WLR_INFO,
					"NVIDIA: using iGPU card node %s for cursor dumb buffer",
					detected_gpus[integrated_gpu_idx].card_path);
			}
		}

		/* Try iGPU render node as fallback */
		if (cdrm_fd < 0 && integrated_gpu_idx >= 0 &&
		    detected_gpus[integrated_gpu_idx].render_path[0]) {
			cdrm_fd = open(detected_gpus[integrated_gpu_idx].render_path,
				O_RDWR | O_CLOEXEC);
			if (cdrm_fd >= 0) {
				cdrm_owns = 1;
				wlr_log(WLR_INFO,
					"NVIDIA: trying iGPU render node %s for cursor dumb buffer",
					detected_gpus[integrated_gpu_idx].render_path);
			}
		}

		/* Use the backend's DRM FD — it has DRM master, so
		 * CREATE_DUMB always succeeds.  On Nvidia-only systems this
		 * is the Nvidia card node; on hybrid it's the iGPU.  We do
		 * NOT own this FD (backend does). */
		if (cdrm_fd < 0) {
			int bfd = wlr_backend_get_drm_fd(backend);
			if (bfd >= 0) {
				cdrm_fd = bfd;
				cdrm_owns = 0;
				wlr_log(WLR_INFO,
					"NVIDIA: using backend DRM fd %d for cursor dumb buffer",
					bfd);
			}
		}

		/* Fall back to NVIDIA primary node (card path) — newer NVIDIA
		 * drivers (545+) support dumb buffers on the primary DRM node. */
		if (cdrm_fd < 0 && detected_gpus[discrete_gpu_idx].card_path[0]) {
			cdrm_fd = open(detected_gpus[discrete_gpu_idx].card_path,
				O_RDWR | O_CLOEXEC);
			if (cdrm_fd >= 0) {
				cdrm_owns = 1;
				wlr_log(WLR_INFO,
					"NVIDIA: trying dGPU primary node %s for cursor dumb buffer",
					detected_gpus[discrete_gpu_idx].card_path);
			}
		}

		/* Last resort: renderer FD (render node) */
		if (cdrm_fd < 0) {
			cdrm_fd = wlr_renderer_get_drm_fd(drw);
			cdrm_owns = 0;
		}

		if (cdrm_fd >= 0) {
			drmGetCap(cdrm_fd, DRM_CAP_CURSOR_WIDTH, &cursor_w);
			drmGetCap(cdrm_fd, DRM_CAP_CURSOR_HEIGHT, &cursor_h);
			if (cursor_w < 64) cursor_w = 64;
			if (cursor_h < 64) cursor_h = 64;
			cpu_cursor_buf = cpu_cursor_buffer_create(cdrm_fd,
				(uint32_t)cursor_w, (uint32_t)cursor_h, cdrm_owns);
			if (cpu_cursor_buf) {
				cpu_cursor_active = 1;
				wlr_log(WLR_INFO,
					"NVIDIA: CPU cursor buffer enabled "
					"(HW cursor plane with dumb buffer, %lux%lu)",
					(unsigned long)cursor_w, (unsigned long)cursor_h);
			} else {
				if (cdrm_owns)
					close(cdrm_fd);
				/* Don't set WLR_NO_HARDWARE_CURSORS globally —
				 * wlroots handles per-output fallback.  Non-Nvidia
				 * outputs (Intel/AMD) still get native HW cursor. */
				wlr_log(WLR_ERROR,
					"NVIDIA: CPU cursor buffer creation failed; "
					"non-Nvidia outputs still use HW cursor");
			}
		} else {
			wlr_log(WLR_ERROR,
				"NVIDIA: no DRM fd for cursor buffer; "
				"non-Nvidia outputs still use HW cursor");
		}
	}

	/* Vulkan renderer: enable CPU cursor for all GPUs.
	 * The Vulkan renderer's texture path may not produce buffers
	 * compatible with all HW cursor planes on multi-monitor.
	 * A dumb DRM buffer works universally. */
	if (!cpu_cursor_active && wlr_renderer_is_vk(drw)) {
		int bfd = wlr_backend_get_drm_fd(backend);
		if (bfd >= 0) {
			uint64_t cursor_w = 64, cursor_h = 64;
			drmGetCap(bfd, DRM_CAP_CURSOR_WIDTH, &cursor_w);
			drmGetCap(bfd, DRM_CAP_CURSOR_HEIGHT, &cursor_h);
			if (cursor_w < 64) cursor_w = 64;
			if (cursor_h < 64) cursor_h = 64;
			cpu_cursor_buf = cpu_cursor_buffer_create(bfd,
				(uint32_t)cursor_w, (uint32_t)cursor_h, 0);
			if (cpu_cursor_buf) {
				cpu_cursor_active = 1;
				wlr_log(WLR_INFO,
					"Vulkan: CPU cursor buffer enabled (%lux%lu)",
					(unsigned long)cursor_w, (unsigned long)cursor_h);
			}
		}
	}

	/* Renderer summary to game debug log */
	{
		const char *gpu = "unknown";
		if (discrete_gpu_idx >= 0)
			gpu = detected_gpus[discrete_gpu_idx].driver;
		else if (integrated_gpu_idx >= 0)
			gpu = detected_gpus[integrated_gpu_idx].driver;
		int has_dmabuf =
			wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF) != NULL;
		game_log("RENDERER: gpu=%s renderer=Vulkan esync=%s dmabuf=%s "
			"drm_fd=%d cpu_cursor=%s",
			gpu,
			g_explicit_sync_ok ? "YES" : "NO",
			has_dmabuf ? "YES" : "NO",
			drm_fd, cpu_cursor_active ? "YES" : "NO");
	}

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
	wl_signal_add(&text_input_mgr->events.new_text_input, &new_text_input);

	seat = wlr_seat_create(dpy, "seat0");
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
	wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
	wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
	wl_signal_add(&seat->events.start_drag, &start_drag);

	tablet_v2_mgr = wlr_tablet_v2_create(dpy);

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
	 * Xwayland binary auto-selection.
	 *
	 * wlroots uses the WLR_XWAYLAND env var to locate the Xwayland binary
	 * (xwayland/server.c).  If the user hasn't set it, find the best
	 * available binary automatically — preferring NixOS system path which
	 * is always the most up-to-date.
	 */
	if (!getenv("WLR_XWAYLAND")) {
		const char *xwl_paths[] = {
			"/run/current-system/sw/bin/Xwayland",
			NULL  /* PATH search handled below */
		};
		const char *xwl_chosen = NULL;

		/* Check fixed paths first (NixOS system path is most up-to-date) */
		for (int pi = 0; xwl_paths[pi]; pi++) {
			if (access(xwl_paths[pi], X_OK) == 0) {
				xwl_chosen = xwl_paths[pi];
				break;
			}
		}

		/* Search PATH as fallback (finds nix-profile and distro binaries) */
		static char xwl_path_buf[4200];
		if (!xwl_chosen) {
			const char *pathenv = getenv("PATH");
			if (pathenv) {
				char pathbuf[4096];
				snprintf(pathbuf, sizeof(pathbuf), "%s", pathenv);
				char *saveptr = NULL;
				for (char *dir = strtok_r(pathbuf, ":", &saveptr); dir;
				     dir = strtok_r(NULL, ":", &saveptr)) {
					snprintf(xwl_path_buf, sizeof(xwl_path_buf), "%s/Xwayland", dir);
					if (access(xwl_path_buf, X_OK) == 0) {
						xwl_chosen = xwl_path_buf;
						break;
					}
				}
			}
		}

		/* Standard distro fallback paths */
		if (!xwl_chosen) {
			const char *fallbacks[] = {
				"/usr/bin/Xwayland",
				"/usr/lib/xorg/Xwayland",
				NULL
			};
			for (int fi = 0; fallbacks[fi]; fi++) {
				if (access(fallbacks[fi], X_OK) == 0) {
					xwl_chosen = fallbacks[fi];
					break;
				}
			}
		}

		if (xwl_chosen) {
			setenv("WLR_XWAYLAND", xwl_chosen, 1);
			wlr_log(WLR_INFO, "Xwayland binary selected: %s", xwl_chosen);
		} else {
			wlr_log(WLR_ERROR,
				"Xwayland binary NOT FOUND — X11 apps will not work. "
				"Install the xwayland package (NixOS: services.xserver.enable or "
				"environment.systemPackages = [ pkgs.xwayland ])");
		}
	} else {
		wlr_log(WLR_INFO, "Xwayland binary (user-set): %s", getenv("WLR_XWAYLAND"));
	}

	/* Nvidia Xwayland performance environment */
	if (discrete_gpu_idx >= 0 &&
	    detected_gpus[discrete_gpu_idx].vendor == GPU_VENDOR_NVIDIA) {
		/* Ensure direct rendering (not indirect/software) */
		setenv("LIBGL_ALWAYS_INDIRECT", "0", 0);

		/* Nvidia experimental performance strategy */
		setenv("__GL_ExperimentalPerfStrategy", "1", 0);
	}

	/*
	 * Initialise the XWayland X server.
	 * It will be started when the first X client is started.
	 */
	if ((xwayland = wlr_xwayland_create(dpy, compositor, 1))) {
		wl_signal_add(&xwayland->events.ready, &xwayland_ready);
		wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

		setenv("DISPLAY", xwayland->display_name, 1);
		wlr_log(WLR_INFO, "XWayland initialized (lazy mode), DISPLAY=%s",
			xwayland->display_name);

		/* Log GPU-specific Xwayland environment for diagnostics */
		if (discrete_gpu_idx >= 0 &&
		    detected_gpus[discrete_gpu_idx].vendor == GPU_VENDOR_NVIDIA) {
			const char *gbm = getenv("GBM_BACKEND");
			const char *glx = getenv("__GLX_VENDOR_LIBRARY_NAME");
			const char *nomod = getenv("WLR_DRM_NO_MODIFIERS");
			wlr_log(WLR_INFO,
				"XWayland+NVIDIA: GBM_BACKEND=%s, __GLX_VENDOR_LIBRARY_NAME=%s, "
				"WLR_DRM_NO_MODIFIERS=%s, cpu_cursor=%s",
				gbm ? gbm : "(unset)", glx ? glx : "(unset)",
				nomod ? nomod : "(unset)",
				cpu_cursor_active ? "active" : "inactive");
		}
	} else {
		wlr_log(WLR_ERROR, "failed to setup XWayland X server, continuing without it");
		/* Vendor-specific troubleshooting hints */
		if (discrete_gpu_idx >= 0) {
			GpuInfo *dgpu = &detected_gpus[discrete_gpu_idx];
			switch (dgpu->vendor) {
			case GPU_VENDOR_NVIDIA:
				wlr_log(WLR_ERROR,
					"NVIDIA: XWayland creation failed. Ensure: "
					"(1) nvidia-drm.modeset=1 kernel parameter is set, "
					"(2) nvidia-drm.fbdev=1 on kernel 6.11+, "
					"(3) Nvidia driver >= 555 installed");
				break;
			case GPU_VENDOR_AMD:
				wlr_log(WLR_ERROR,
					"AMD: XWayland creation failed. Check: "
					"(1) amdgpu kernel module is loaded, "
					"(2) /dev/dri/renderD* is accessible (user in 'render' group), "
					"(3) mesa and xwayland packages are installed");
				break;
			case GPU_VENDOR_INTEL:
				wlr_log(WLR_ERROR,
					"Intel: XWayland creation failed. Check: "
					"(1) i915/xe kernel module is loaded, "
					"(2) /dev/dri/renderD* is accessible (user in 'render' group), "
					"(3) mesa and xwayland packages are installed");
				break;
			default:
				wlr_log(WLR_ERROR,
					"XWayland creation failed with unknown GPU vendor. "
					"Check /dev/dri/ devices exist and are accessible");
				break;
			}
		} else if (integrated_gpu_idx >= 0) {
			GpuInfo *igpu = &detected_gpus[integrated_gpu_idx];
			switch (igpu->vendor) {
			case GPU_VENDOR_INTEL:
				wlr_log(WLR_ERROR,
					"Intel iGPU only: XWayland creation failed. Check: "
					"(1) i915/xe module loaded, (2) mesa/xwayland installed, "
					"(3) /dev/dri/renderD* accessible");
				break;
			case GPU_VENDOR_AMD:
				wlr_log(WLR_ERROR,
					"AMD APU only: XWayland creation failed. Check: "
					"(1) amdgpu module loaded, (2) mesa/xwayland installed, "
					"(3) /dev/dri/renderD* accessible");
				break;
			default:
				wlr_log(WLR_ERROR,
					"XWayland creation failed. Check /dev/dri/ devices "
					"and GPU driver modules");
				break;
			}
		} else {
			wlr_log(WLR_ERROR,
				"No GPU detected — XWayland requires a functioning GPU. "
				"Check that DRI kernel modules are loaded and /dev/dri/ "
				"contains card and render nodes");
		}
	}

	/* === Xwayland diagnostic summary === */
	{
		int has_dmabuf = wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF) != NULL;
		int has_sync = (drm_fd >= 0 && drw->features.timeline && backend->features.timeline);
		wlr_log(WLR_INFO,
			"=== Xwayland diagnostic summary === "
			"GPUs=%d, discrete=%s, integrated=%s, "
			"DMA-BUF=%s, explicit_sync=%s, "
			"Xwayland=%s",
			detected_gpu_count,
			discrete_gpu_idx >= 0 ? detected_gpus[discrete_gpu_idx].driver : "none",
			integrated_gpu_idx >= 0 ? detected_gpus[integrated_gpu_idx].driver : "none",
			has_dmabuf ? "yes" : "NO",
			has_sync ? "yes" : "no",
			xwayland ? xwayland->display_name : "FAILED");
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
	if (diag_timer && diag_log_fd >= 0)
		wl_event_source_timer_update(diag_timer, 5000);
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

/*
 * Fork and exec a shell command string with full child cleanup.
 * Used by spawn() and every HTPC launch path so every child gets the
 * same treatment as a desktop-mode spawn: stdin → /dev/null, fresh
 * session, default signal handlers, ambient caps dropped, all
 * inherited compositor FDs (DRM master, PipeWire, epoll) closed,
 * $HOME as CWD, session env preserved. Returns child pid (parent)
 * or -1 on fork failure. Never returns in the child.
 */
pid_t
spawn_cmd(const char *cmd)
{
	pid_t pid;

	if (!cmd || !cmd[0])
		return -1;

	pid = fork();
	if (pid > 0) {
		if (selmon)
			pending_launch_add(pid,
				selmon->tagset[selmon->seltags],
				selmon->wlr_output->name);
		return pid;
	}
	if (pid < 0)
		return -1;

	/* Child */
	{
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			if (devnull > STDERR_FILENO)
				close(devnull);
		}
	}

	setsid();
	signal(SIGCHLD, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);

	syscall(SYS_close_range, 3, ~0U, 0);

	{
		const char *home = getenv("HOME");
		if (home && *home)
			(void)chdir(home);
	}

	ensure_nix_paths();

	const char *user_shell = getenv("SHELL");
	if (!user_shell || !user_shell[0])
		user_shell = "/bin/sh";

	char cmd_str[4096];
	snprintf(cmd_str, sizeof(cmd_str), "%s", cmd);

	if (is_steam_cmd(cmd_str)) {
		const char *steam_bin = "steam";
		if (access("/etc/profiles/per-user/total/bin/nixly_steam", X_OK) == 0)
			steam_bin = "nixly_steam";
		else if (access("/run/current-system/sw/bin/nixly_steam", X_OK) == 0)
			steam_bin = "nixly_steam";
		if (htpc_mode_active)
			snprintf(cmd_str, sizeof(cmd_str),
				"%s -bigpicture steam://open/games", steam_bin);
		else
			snprintf(cmd_str, sizeof(cmd_str), "%s", steam_bin);
	} else if (should_use_dgpu(cmd_str) && integrated_gpu_idx >= 0) {
		set_dgpu_env();
	}

	{
		int has_meta = 0;
		for (const char *p = cmd_str; *p; p++) {
			if (*p == '|' || *p == '&' ||
			    *p == ';' || *p == '$' ||
			    *p == '`' || *p == '(' ||
			    *p == ')' || *p == '>' ||
			    *p == '<' || *p == '{' ||
			    *p == '}' || *p == '~' ||
			    *p == '*' || *p == '?' ||
			    *p == '[' || *p == ']' ||
			    *p == '\'' || *p == '"' ||
			    *p == '\\') {
				has_meta = 1;
				break;
			}
		}

		if (!has_meta) {
			char buf[4096];
			char *argv[64];
			int argc = 0;
			char *s, *tok, *sv = NULL;

			snprintf(buf, sizeof(buf), "%s", cmd_str);
			for (s = buf; argc < 63; s = NULL) {
				tok = strtok_r(s, " \t", &sv);
				if (!tok)
					break;
				argv[argc++] = tok;
			}
			argv[argc] = NULL;

			if (argc > 0)
				execvp(argv[0], argv);
		}

		char wrapper[8192];
		snprintf(wrapper, sizeof(wrapper), "exec %s", cmd_str);
		execl(user_shell, user_shell, "-c", wrapper, NULL);
	}
	_exit(127);
}

void
spawn(const Arg *arg)
{
	const char *cmd = (const char *)arg->v;
	if (!cmd || !cmd[0])
		return;

	int is_runtime_string = 0;
	for (int i = 0; i < runtime_spawn_cmd_count; i++) {
		if (arg->v == runtime_spawn_cmds[i]) {
			is_runtime_string = 1;
			break;
		}
	}

	char cmd_str[4096];
	if (!is_runtime_string) {
		char **argv = (char **)arg->v;
		if (!argv[0] || !argv[0][0])
			return;
		int pos = 0;
		for (int i = 0; argv[i] && pos < (int)sizeof(cmd_str) - 2; i++) {
			if (i > 0 && pos < (int)sizeof(cmd_str) - 1)
				cmd_str[pos++] = ' ';
			int n = snprintf(cmd_str + pos, sizeof(cmd_str) - pos,
				"%s", argv[i]);
			if (n > 0)
				pos += n;
		}
		cmd_str[pos] = '\0';
	} else {
		snprintf(cmd_str, sizeof(cmd_str), "%s", cmd);
	}

	spawn_cmd(cmd_str);
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
	/*
	 * Fullscreen resize handling.
	 *
	 * Accept the client's requested size and center the surface
	 * visually within the monitor.  c->geom stays at full monitor
	 * geometry so input routing and scene bounds remain correct.
	 *
	 * Games must keep their own resolution — forcing native causes
	 * resolution cycling (the game fights the compositor) and breaks
	 * mouse coordinates because the X11 window size no longer matches
	 * the game's internal rendering resolution.
	 *
	 * The black-bar pointer fallback in motionnotify() routes pointer
	 * events from the letterbox area to the game's surface.
	 */
	if (c->isfullscreen && c->mon) {
		struct wlr_box fsgeom = fullscreen_mirror_geom(c->mon);
		int gw = event->width;
		int gh = event->height;

		wlr_log(WLR_INFO,
			"GAME_TRACE: configurex11 fullscreen "
			"appid='%s' requested=%dx%d monitor=%dx%d mon='%s'",
			client_get_appid(c) ? client_get_appid(c) : "(null)",
			gw, gh,
			fsgeom.width, fsgeom.height,
			c->mon->wlr_output ? c->mon->wlr_output->name : "(null)");

		/* Requested smaller than monitor → accept and center */
		if (gw < fsgeom.width || gh < fsgeom.height) {
			if (gw > fsgeom.width)  gw = fsgeom.width;
			if (gh > fsgeom.height) gh = fsgeom.height;

			int cx = fsgeom.x + (fsgeom.width  - gw) / 2;
			int cy = fsgeom.y + (fsgeom.height - gh) / 2;
			int dx = (fsgeom.width  - gw) / 2;
			int dy = (fsgeom.height - gh) / 2;

			wlr_log(WLR_INFO,
				"GAME_TRACE: configurex11 fullscreen centered "
				"appid='%s' size=%dx%d offset=%d,%d xpos=%d,%d",
				client_get_appid(c) ? client_get_appid(c) : "(null)",
				gw, gh, dx, dy, cx, cy);

			wlr_xwayland_surface_configure(c->surface.xwayland,
				cx, cy, gw, gh);
			wlr_scene_node_set_position(&c->scene_surface->node,
				dx, dy);

			if (c->geom.width != fsgeom.width
					|| c->geom.height != fsgeom.height
					|| c->geom.x != fsgeom.x
					|| c->geom.y != fsgeom.y) {
				c->geom = fsgeom;
				wlr_scene_node_set_position(&c->scene->node,
					fsgeom.x, fsgeom.y);
			}

			/* Refresh pointer focus so the cursor stays on the
			 * game surface after centering at a new resolution */
			motionnotify(0, NULL, 0, 0, 0, 0);
			return;
		}

		/* Requested >= monitor size → standard fullscreen */
		wlr_scene_node_set_position(&c->scene_surface->node, 0, 0);
		if (c->geom.width != fsgeom.width || c->geom.height != fsgeom.height
				|| c->geom.x != fsgeom.x || c->geom.y != fsgeom.y) {
			resize(c, fsgeom, 0);
		} else {
			wlr_xwayland_surface_configure(c->surface.xwayland,
				fsgeom.x, fsgeom.y, fsgeom.width, fsgeom.height);
		}

		/* Refresh pointer focus after resolution change */
		motionnotify(0, NULL, 0, 0, 0, 0);
		return;
	}
	if ((c->isfloating && c != grabc) || !c->mon->lt[c->mon->sellt]->arrange) {
		int bx = event->x - c->bw;
		int by = event->y - c->bw;
		int bw = event->width + c->bw * 2;
		int bh = event->height + c->bw * 2;

		/*
		 * Game clients occasionally request X11 positions at root
		 * coordinates that lie outside their assigned monitor (Squad
		 * asks for @-1,-1 or @0,0 which is on DP-2 when the game was
		 * assigned to DP-1). Honouring those positions makes the
		 * window appear on the wrong monitor briefly. Clamp the
		 * requested position into the assigned monitor for any client
		 * that looks like a game.
		 */
		if (c->mon && looks_like_game(c)) {
			int mx = c->mon->m.x;
			int my = c->mon->m.y;
			int mw = c->mon->m.width;
			int mh = c->mon->m.height;
			int old_bx = bx, old_by = by;
			if (bw > mw) bw = mw;
			if (bh > mh) bh = mh;
			if (bx < mx) bx = mx;
			if (by < my) by = my;
			if (bx + bw > mx + mw) bx = mx + mw - bw;
			if (by + bh > my + mh) by = my + mh - bh;
			if (bx != old_bx || by != old_by)
				wlr_log(WLR_INFO,
					"GAME_TRACE: configurex11 clamp appid='%s' "
					"req=@%d,%d → @%d,%d (mon='%s' %dx%d@%d,%d)",
					client_get_appid(c) ? client_get_appid(c) : "(null)",
					old_bx, old_by, bx, by,
					c->mon->wlr_output ? c->mon->wlr_output->name : "(null)",
					mw, mh, mx, my);
		}
		resize(c, (struct wlr_box){.x = bx, .y = by, .width = bw, .height = bh}, 0);
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
	LISTEN(&xsurface->events.set_override_redirect, &c->set_override_redirect, setoverrideredirect);
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
setoverrideredirect(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_override_redirect);

	/* When override_redirect changes on a mapped surface, we must
	 * unmap and remap the client so it moves between managed/unmanaged
	 * state (different layer, border, focus behavior). */
	if (c->surface.xwayland->surface &&
	    c->surface.xwayland->surface->mapped)
		unmapnotify(&c->unmap, NULL);

	c->bw = client_is_unmanaged(c) ? 0 : borderpx;

	if (c->surface.xwayland->surface &&
	    c->surface.xwayland->surface->mapped)
		mapnotify(&c->map, NULL);
}

void
xwaylandready(struct wl_listener *listener, void *data)
{
	struct wlr_xcursor *xcursor;
	xcb_connection_t *xc;
	int err;

	wlr_log(WLR_INFO, "XWayland server is ready (DISPLAY=%s)", xwayland->display_name);

	/* Verify XCB connection to the Xwayland server */
	xc = xcb_connect(xwayland->display_name, NULL);
	err = xcb_connection_has_error(xc);
	if (err) {
		/* Decode XCB error codes to human-readable descriptions */
		const char *err_desc;
		switch (err) {
		case XCB_CONN_ERROR:
			err_desc = "connection error (socket/pipe failure)";
			break;
		case XCB_CONN_CLOSED_EXT_NOTSUPPORTED:
			err_desc = "unsupported extension";
			break;
		case XCB_CONN_CLOSED_MEM_INSUFFICIENT:
			err_desc = "insufficient memory";
			break;
		case XCB_CONN_CLOSED_REQ_LEN_EXCEED:
			err_desc = "request length exceeded";
			break;
		case XCB_CONN_CLOSED_PARSE_ERR:
			err_desc = "display string parse error";
			break;
		case XCB_CONN_CLOSED_INVALID_SCREEN:
			err_desc = "invalid screen";
			break;
		default:
			err_desc = "unknown error";
			break;
		}
		wlr_log(WLR_ERROR,
			"XWayland: xcb_connect failed with code %d (%s) — "
			"X11 apps will not work", err, err_desc);

		/* Vendor-specific XCB error tips */
		if (discrete_gpu_idx >= 0) {
			switch (detected_gpus[discrete_gpu_idx].vendor) {
			case GPU_VENDOR_NVIDIA:
				wlr_log(WLR_ERROR,
					"NVIDIA: XCB connection failure may indicate "
					"missing nvidia-drm.modeset=1 or broken EGL/GBM setup");
				break;
			case GPU_VENDOR_AMD:
				wlr_log(WLR_ERROR,
					"AMD: XCB connection failure — check that mesa "
					"and xwayland packages are installed correctly");
				break;
			case GPU_VENDOR_INTEL:
				wlr_log(WLR_ERROR,
					"Intel: XCB connection failure — check that mesa "
					"and xwayland packages are installed correctly");
				break;
			default:
				break;
			}
		}
		return;
	}

	/* GLX extension probe — check if OpenGL over X11 will work */
	{
		xcb_query_extension_cookie_t glx_cookie =
			xcb_query_extension(xc, 3, "GLX");
		xcb_query_extension_reply_t *glx_reply =
			xcb_query_extension_reply(xc, glx_cookie, NULL);
		if (glx_reply) {
			if (glx_reply->present) {
				wlr_log(WLR_INFO,
					"XWayland: GLX extension is available — "
					"OpenGL X11 apps (blender, freecad, etc.) should work");
			} else {
				wlr_log(WLR_ERROR,
					"XWayland: GLX extension NOT available — "
					"OpenGL X11 apps (blender, freecad, etc.) will fail. "
					"Install GPU GL/EGL libraries (mesa, nvidia-libs)");
			}
			free(glx_reply);
		}
	}

	/* Intern Steam atoms on this temporary connection so we can read
	 * STEAM_GAME / STEAM_OVERLAY / STEAM_BIGPICTURE later via the
	 * wlr_xwayland XWM connection.  The atom IDs are server-global,
	 * so they're valid on any connection to the same X server. */
	{
		xcb_intern_atom_cookie_t sg = xcb_intern_atom(xc, 0, 10, "STEAM_GAME");
		xcb_intern_atom_cookie_t so = xcb_intern_atom(xc, 0, 13, "STEAM_OVERLAY");
		xcb_intern_atom_cookie_t sb = xcb_intern_atom(xc, 0, 16, "STEAM_BIGPICTURE");
		xcb_intern_atom_reply_t *r;

		if ((r = xcb_intern_atom_reply(xc, sg, NULL))) {
			atom_steam_game = r->atom;
			free(r);
		}
		if ((r = xcb_intern_atom_reply(xc, so, NULL))) {
			atom_steam_overlay = r->atom;
			free(r);
		}
		if ((r = xcb_intern_atom_reply(xc, sb, NULL))) {
			atom_steam_bigpicture = r->atom;
			free(r);
		}
		wlr_log(WLR_INFO,
			"XWayland: interned Steam atoms — "
			"STEAM_GAME=%u STEAM_OVERLAY=%u STEAM_BIGPICTURE=%u",
			atom_steam_game, atom_steam_overlay, atom_steam_bigpicture);
	}

	xcb_disconnect(xc);

	/* assign the one and only seat */
	wlr_xwayland_set_seat(xwayland, seat);

	/* Set the default XWayland cursor to match the rest of dwl. */
	wlr_xcursor_manager_load(cursor_mgr, 1);
	if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1)))
		wlr_xwayland_set_cursor(xwayland,
				wlr_xcursor_image_get_buffer(xcursor->images[0]),
				xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);

	/* Log successful ready with vendor info */
	{
		const char *gpu_info = "no discrete GPU";
		if (discrete_gpu_idx >= 0)
			gpu_info = detected_gpus[discrete_gpu_idx].driver;
		wlr_log(WLR_INFO,
			"XWayland ready: XCB verified, GPU=%s, DISPLAY=%s",
			gpu_info, xwayland->display_name);
	}
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
	load_monitors_conf(); /* Load monitor layout from dedicated config first */
	load_config();
	init_keybindings();

	/* NOTE: Do NOT call set_dgpu_env() here in the compositor process.
	 * It sets DRI_PRIME, __GLX_VENDOR_LIBRARY_NAME, etc. which are
	 * inherited by Xwayland and all child processes. This forces
	 * GPU-rendered apps (Alacritty, Kitty, etc.) to render on the
	 * discrete GPU while the compositor renders on the integrated GPU,
	 * causing cross-GPU DMA-BUF import failures (invisible windows).
	 *
	 * Instead, set_dgpu_env() is called per-process in fork paths:
	 * spawn(), launcher.c, htpc.c — only for games/apps that need dGPU. */

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
	setup_monitors_conf_watch();
	setup_monitor_overlay_watch();
	gamepad_setup();
	bt_controller_setup();
	pc_gaming_cache_watch_setup();

	/* Set GE-Proton as default Steam compatibility tool */
	steam_set_ge_proton_default();

	/* HTPC mode (nixlytile_mode == 2) is entered from updatemons()
	 * once the first monitor is ready and selmon is set */

	/* Defer all cache updates — let the compositor settle first.
	 * Phase 0=git, 1=file, 2=gaming; each fires 30 min apart. */
	cache_update_phase = 0;
	if (cache_update_timer)
		wl_event_source_timer_update(cache_update_timer, 120000); /* 2 minutes */
	/* Generate git and file caches immediately so search works right away */
	git_cache_update_start();
	file_cache_update_start();
	/* Generate nixpkgs cache now if missing, then schedule weekly updates */
	nixpkgs_cache_update_start();
	schedule_nixpkgs_cache_timer();
	run(startup_cmd);
	cleanup();
	return EXIT_SUCCESS;

usage:
	die("Usage: %s [-v] [-d] [-s startup command]", argv[0]);
}
