/* media.c - Auto-extracted from nixlytile.c */
#include "nixlytile.h"
#include "client.h"

void
add_media_server(const char *url, int is_local, int is_configured)
{
	/* Check for duplicates */
	for (int i = 0; i < media_server_count; i++) {
		if (strcmp(media_servers[i].url, url) == 0)
			return;
	}

	if (media_server_count >= MAX_MEDIA_SERVERS)
		return;

	MediaServer *srv = &media_servers[media_server_count];
	strncpy(srv->url, url, sizeof(srv->url) - 1);
	srv->server_id[0] = '\0';
	srv->server_name[0] = '\0';
	srv->priority = is_local ? 1000 : 0;  /* Will be updated from /api/status */
	srv->is_local = is_local;
	srv->is_configured = is_configured;
	media_server_count++;

	wlr_log(WLR_INFO, "Added media server: %s (local=%d, configured=%d)",
		url, is_local, is_configured);
}

int
discover_nixly_server(void)
{
	int sock;
	struct sockaddr_in broadcast_addr, recv_addr;
	socklen_t addr_len;
	char recv_buf[256];
	ssize_t recv_len;
	struct timeval tv;
	int broadcast_enable = 1;
	int discovered_count = 0;

	/* Create UDP socket */
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		wlr_log(WLR_DEBUG, "Discovery: failed to create socket");
		return 0;
	}

	/* Enable broadcast */
	if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
		wlr_log(WLR_DEBUG, "Discovery: failed to enable broadcast");
		close(sock);
		return 0;
	}

	/* Set receive timeout (200ms per response, allows multiple servers) */
	tv.tv_sec = 0;
	tv.tv_usec = 200000;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* Broadcast discovery message to local network */
	memset(&broadcast_addr, 0, sizeof(broadcast_addr));
	broadcast_addr.sin_family = AF_INET;
	broadcast_addr.sin_port = htons(MEDIA_DISCOVERY_PORT);
	broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	if (sendto(sock, MEDIA_DISCOVERY_MAGIC, strlen(MEDIA_DISCOVERY_MAGIC), 0,
		   (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr)) < 0) {
		wlr_log(WLR_DEBUG, "Discovery: broadcast send failed");
		close(sock);
		return 0;
	}

	/* Collect responses from multiple servers (wait up to 1 second total) */
	for (int attempts = 0; attempts < 5 && discovered_count < MAX_MEDIA_SERVERS; attempts++) {
		addr_len = sizeof(recv_addr);
		recv_len = recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0,
				    (struct sockaddr *)&recv_addr, &addr_len);

		if (recv_len <= 0)
			break;  /* Timeout, no more servers */

		recv_buf[recv_len] = '\0';
		if (strncmp(recv_buf, MEDIA_DISCOVERY_RESPONSE, strlen(MEDIA_DISCOVERY_RESPONSE)) == 0) {
			/* Extract port if provided, otherwise use default */
			int port = MEDIA_SERVER_PORT;
			char *port_str = strchr(recv_buf, ':');
			if (port_str) {
				port = atoi(port_str + 1);
				if (port <= 0 || port > 65535)
					port = MEDIA_SERVER_PORT;
			}

			char ip_str[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &recv_addr.sin_addr, ip_str, sizeof(ip_str));

			char url[256];
			snprintf(url, sizeof(url), "http://%s:%d", ip_str, port);

			/* Add as local server (auto-discovered) */
			add_media_server(url, 1, 0);
			discovered_count++;

			/* Also set legacy discovered_server_url for compatibility */
			if (!server_discovered) {
				strncpy(discovered_server_url, url, sizeof(discovered_server_url) - 1);
				server_discovered = 1;
			}
		}
	}

	close(sock);
	return discovered_count;
}

int
try_localhost_server(void)
{
	char cmd[256];
	FILE *fp;

	snprintf(cmd, sizeof(cmd), "curl -s --connect-timeout 1 http://localhost:%d/api/status 2>/dev/null", MEDIA_SERVER_PORT);
	fp = popen(cmd, "r");
	if (fp) {
		char buf[64];
		if (fgets(buf, sizeof(buf), fp) && strstr(buf, "ok")) {
			pclose(fp);
			char url[256];
			snprintf(url, sizeof(url), "http://localhost:%d", MEDIA_SERVER_PORT);
			add_media_server(url, 1, 0);  /* Local, not configured */
			snprintf(discovered_server_url, sizeof(discovered_server_url), "%s", url);
			wlr_log(WLR_INFO, "Found nixly-server on localhost");
			server_discovered = 1;
			return 1;
		}
		pclose(fp);
	}
	return 0;
}

void
run_local_discovery(void)
{
	uint64_t now = monotonic_msec();

	/* Don't retry too often (every 30 seconds) */
	if (now - last_discovery_attempt_ms < 30000)
		return;
	last_discovery_attempt_ms = now;

	/* Try localhost first */
	try_localhost_server();

	/* Then broadcast discovery for other local servers */
	discover_nixly_server();
}

const char *
get_media_server_url(void)
{
	/* Always try to discover local servers */
	run_local_discovery();

	if (media_server_count == 0) {
		/* Fallback to localhost even if not responding */
		return "http://localhost:8080";
	}

	/* Find server with highest priority (local servers have priority 1000+) */
	int best_idx = 0;
	for (int i = 1; i < media_server_count; i++) {
		if (media_servers[i].priority > media_servers[best_idx].priority)
			best_idx = i;
	}

	return media_servers[best_idx].url;
}

int
get_all_media_servers(MediaServer **servers)
{
	*servers = media_servers;
	return media_server_count;
}

int
calculate_buffer_time(int64_t file_size, int duration, int bandwidth_mbps)
{
	if (duration <= 0 || file_size <= 0 || bandwidth_mbps <= 0)
		return 0;

	/* Calculate file bitrate in Mbps */
	double file_bitrate_mbps = ((double)file_size * 8.0) / ((double)duration * 1000000.0);

	/* If client bandwidth >= file bitrate, can stream directly */
	if (bandwidth_mbps >= file_bitrate_mbps)
		return 0;

	/* Need to buffer. Calculate how much time to wait.
	 * We need to download enough so that by the time the video ends,
	 * we've downloaded the whole file.
	 *
	 * Let T = total duration, B = buffer time, D = download speed, P = playback speed (bitrate)
	 * During buffer: download B * D bytes
	 * During playback: download T * D bytes while playing T * P bytes
	 * Total downloaded: (B + T) * D >= T * P (file size)
	 * B >= T * (P - D) / D = T * (P/D - 1)
	 *
	 * In Mbps terms: B >= T * (file_bitrate / bandwidth - 1)
	 */
	double ratio = file_bitrate_mbps / (double)bandwidth_mbps;
	int buffer_seconds = (int)(duration * (ratio - 1.0)) + 30;  /* +30s safety margin */

	if (buffer_seconds < 30)
		buffer_seconds = 30;

	return buffer_seconds;
}

double
load_resume_position(int media_id)
{
	int i;
	for (i = 0; i < resume_cache_count; i++) {
		if (resume_cache[i].media_id == media_id)
			return resume_cache[i].position;
	}
	return 0.0;
}

void
save_resume_position(int media_id, double position)
{
	int i;
	/* Update existing entry */
	for (i = 0; i < resume_cache_count; i++) {
		if (resume_cache[i].media_id == media_id) {
			resume_cache[i].position = position;
			return;
		}
	}
	/* Add new entry */
	if (resume_cache_count < 256) {
		resume_cache[resume_cache_count].media_id = media_id;
		resume_cache[resume_cache_count].position = position;
		resume_cache_count++;
	}
}

void
launch_integrated_player_with_resume(const char *url, double resume_pos)
{
	if (!url || !selmon)
		return;

	/* Copy URL to playback_url for tracking */
	strncpy(playback_url, url, sizeof(playback_url) - 1);
	playback_url[sizeof(playback_url) - 1] = '\0';

	/* Hide media views first */
	media_view_hide_all();

	/* Create video player if it doesn't exist */
	if (!active_videoplayer) {
		active_videoplayer = videoplayer_create(selmon);
		if (!active_videoplayer) {
			wlr_log(WLR_ERROR, "Failed to create video player");
			return;
		}

		/* Initialize scene on block layer (topmost) so player is always above HTPC UI */
		if (videoplayer_init_scene(active_videoplayer, layers[LyrBlock]) < 0) {
			wlr_log(WLR_ERROR, "Failed to initialize video player scene");
			videoplayer_destroy(active_videoplayer);
			active_videoplayer = NULL;
			return;
		}
	}

	/* Open the file/URL */
	if (videoplayer_open(active_videoplayer, url) < 0) {
		wlr_log(WLR_ERROR, "Failed to open video: %s - %s",
			url, active_videoplayer->error_msg);
		return;
	}

	/* Setup display mode based on monitor capabilities */
	float display_hz = selmon->wlr_output->current_mode ?
		selmon->wlr_output->current_mode->refresh / 1000.0f : 60.0f;
	videoplayer_setup_display_mode(active_videoplayer, display_hz, selmon->vrr_capable);

	/* Set fullscreen size to cover entire monitor */
	videoplayer_set_fullscreen_size(active_videoplayer, selmon->m.width, selmon->m.height);

	/* Position at monitor origin */
	videoplayer_set_position(active_videoplayer, selmon->m.x, selmon->m.y);

	/* Make visible */
	videoplayer_set_visible(active_videoplayer, 1);

	/* Seek to resume position if provided */
	if (resume_pos > 0.0) {
		int64_t resume_us = (int64_t)(resume_pos * 1000000.0);
		videoplayer_seek(active_videoplayer, resume_us);
	}

	/* Start playback */
	videoplayer_play(active_videoplayer);

	playback_state = PLAYBACK_PLAYING;

	wlr_log(WLR_INFO, "Started integrated video player: %s (resume at %.1fs)", url, resume_pos);
}

void
launch_integrated_player(const char *filepath)
{
	launch_integrated_player_with_resume(filepath, 0.0);
}

void
stop_integrated_player(void)
{
	if (active_videoplayer && active_videoplayer->state != VP_STATE_IDLE) {
		videoplayer_stop(active_videoplayer);
		videoplayer_set_visible(active_videoplayer, 0);
		playback_state = PLAYBACK_IDLE;
		wlr_log(WLR_INFO, "Stopped integrated video player");
	}
}

void
media_start_playback(MediaItem *item, int is_movie)
{
	double resume_pos;
	char cmd[768];
	FILE *fp;

	if (!item) return;

	/* Build stream URL */
	snprintf(playback_url, sizeof(playback_url), "%s/stream/%d",
		 item->server_url[0] ? item->server_url : get_media_server_url(), item->id);

	playback_is_movie = is_movie;
	playback_duration = item->duration;
	playback_media_id = item->id;

	/* Check for resume position */
	resume_pos = load_resume_position(item->id);

	/* Fetch file size from server to calculate bitrate */
	snprintf(cmd, sizeof(cmd),
		 "curl -sI '%s' 2>/dev/null | grep -i content-length | awk '{print $2}' | tr -d '\\r'",
		 playback_url);
	fp = popen(cmd, "r");
	if (fp) {
		char buf[64];
		if (fgets(buf, sizeof(buf), fp)) {
			playback_file_size = atoll(buf);
		}
		pclose(fp);
	}

	/* Calculate buffer time */
	playback_buffer_seconds = calculate_buffer_time(playback_file_size, playback_duration, client_download_mbps);

	if (playback_buffer_seconds > 0) {
		/* Need to buffer first */
		int minutes = (playback_buffer_seconds + 59) / 60;
		const char *media_type = is_movie ? "movie" : "episode";

		snprintf(playback_message, sizeof(playback_message),
			"Due to your internet connection being only %d Mbps,\n"
			"parts of this %s need to be downloaded before playback can begin.\n\n"
			"We recommend upgrading your internet connection\n"
			"for uninterrupted direct playback.\n\n"
			"Estimated wait time: approximately %d minute%s.",
			client_download_mbps,
			media_type,
			minutes,
			minutes == 1 ? "" : "s");

		playback_state = PLAYBACK_BUFFERING;
		playback_buffer_progress = 0;
		playback_start_time = monotonic_msec();

		wlr_log(WLR_INFO, "Media playback: buffering %d seconds before starting", playback_buffer_seconds);
	} else {
		/* Can play directly - use integrated player */
		wlr_log(WLR_INFO, "Media playback: starting direct playback of %s (resume at %.1fs)",
			item->title, resume_pos);
		launch_integrated_player_with_resume(playback_url, resume_pos);
	}
}

void
media_check_buffering(void)
{
	if (playback_state != PLAYBACK_BUFFERING)
		return;

	uint64_t elapsed_ms = monotonic_msec() - playback_start_time;
	int elapsed_sec = elapsed_ms / 1000;

	playback_buffer_progress = (elapsed_sec * 100) / playback_buffer_seconds;
	if (playback_buffer_progress > 100)
		playback_buffer_progress = 100;

	/* Update remaining time in message */
	int remaining_sec = playback_buffer_seconds - elapsed_sec;
	if (remaining_sec < 0) remaining_sec = 0;
	int minutes = (remaining_sec + 59) / 60;

	const char *media_type = playback_is_movie ? "movie" : "episode";
	snprintf(playback_message, sizeof(playback_message),
		"Due to your internet connection being only %d Mbps,\n"
		"parts of this %s need to be downloaded before playback can begin.\n\n"
		"We recommend upgrading your internet connection\n"
		"for uninterrupted direct playback.\n\n"
		"Estimated wait time: approximately %d minute%s.",
		client_download_mbps,
		media_type,
		minutes,
		minutes == 1 ? "" : "s");

	if (elapsed_sec >= playback_buffer_seconds) {
		/* Buffer complete - start playback with integrated player */
		double resume_pos = load_resume_position(playback_media_id);
		wlr_log(WLR_INFO, "Media playback: buffer complete, starting playback");
		launch_integrated_player_with_resume(playback_url, resume_pos);
	}
}

void
media_cancel_buffering(void)
{
	if (playback_state == PLAYBACK_BUFFERING) {
		playback_state = PLAYBACK_IDLE;
		playback_message[0] = '\0';
		wlr_log(WLR_INFO, "Media playback: buffering cancelled");
	}
}

void
media_view_free_items(MediaGridView *view)
{
	MediaItem *item, *next;
	if (!view) return;

	item = view->items;
	while (item) {
		next = item->next;
		if (item->poster_buf)
			wlr_buffer_drop(item->poster_buf);
		free(item);
		item = next;
	}
	view->items = NULL;
	view->item_count = 0;
}

int
json_extract_string(const char *json, const char *key, char *out, size_t out_size)
{
	char search[128];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *pos = strstr(json, search);
	if (!pos) return 0;

	pos = strchr(pos, ':');
	if (!pos) return 0;
	pos++;
	while (*pos == ' ') pos++;

	if (*pos == '"') {
		pos++;
		/* Find end quote, handling escaped quotes */
		const char *src = pos;
		char *dst = out;
		char *dst_end = out + out_size - 1;

		while (*src && *src != '"' && dst < dst_end) {
			if (*src == '\\' && src[1]) {
				src++;
				switch (*src) {
				case '"': *dst++ = '"'; break;
				case '\\': *dst++ = '\\'; break;
				case 'n': *dst++ = '\n'; break;
				case 'r': *dst++ = '\r'; break;
				case 't': *dst++ = '\t'; break;
				default: *dst++ = *src; break;
				}
				src++;
			} else {
				*dst++ = *src++;
			}
		}
		*dst = '\0';
		return 1;
	} else if (strncmp(pos, "null", 4) == 0) {
		out[0] = '\0';
		return 1;
	}
	return 0;
}

int
json_extract_int(const char *json, const char *key)
{
	char search[128];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *pos = strstr(json, search);
	if (!pos) return 0;
	pos = strchr(pos, ':');
	if (!pos) return 0;
	return atoi(pos + 1);
}

float
json_extract_float(const char *json, const char *key)
{
	char search[128];
	snprintf(search, sizeof(search), "\"%s\":", key);
	const char *pos = strstr(json, search);
	if (!pos) return 0.0f;
	pos = strchr(pos, ':');
	if (!pos) return 0.0f;
	return atof(pos + 1);
}

int
media_view_poll_timer_cb(void *data)
{
	Monitor *m;
	(void)data;

	if (!htpc_mode_active) {
		/* Re-arm timer even when inactive */
		if (media_view_poll_timer)
			wl_event_source_timer_update(media_view_poll_timer, 3000);
		return 0;
	}

	wl_list_for_each(m, &mons, link) {
		/* Update movies view if visible */
		if (m->movies_view.visible) {
			int saved_idx = m->movies_view.selected_idx;
			int saved_scroll = m->movies_view.scroll_offset;
			if (media_view_refresh(m, MEDIA_VIEW_MOVIES)) {
				/* Data changed - restore navigation and re-render */
				if (saved_idx < m->movies_view.item_count)
					m->movies_view.selected_idx = saved_idx;
				else if (m->movies_view.item_count > 0)
					m->movies_view.selected_idx = m->movies_view.item_count - 1;
				m->movies_view.scroll_offset = saved_scroll;
				media_view_render(m, MEDIA_VIEW_MOVIES);
			}
		}

		/* Update tvshows view if visible */
		if (m->tvshows_view.visible) {
			int saved_idx = m->tvshows_view.selected_idx;
			int saved_scroll = m->tvshows_view.scroll_offset;
			if (media_view_refresh(m, MEDIA_VIEW_TVSHOWS)) {
				/* Data changed - restore navigation and re-render */
				if (saved_idx < m->tvshows_view.item_count)
					m->tvshows_view.selected_idx = saved_idx;
				else if (m->tvshows_view.item_count > 0)
					m->tvshows_view.selected_idx = m->tvshows_view.item_count - 1;
				m->tvshows_view.scroll_offset = saved_scroll;
				media_view_render(m, MEDIA_VIEW_TVSHOWS);
			}
		}
	}

	/* Check buffering progress */
	if (playback_state == PLAYBACK_BUFFERING) {
		int old_progress = playback_buffer_progress;
		PlaybackState old_state = playback_state;
		Monitor *bm;

		media_check_buffering();

		/* Re-render if progress changed or state changed */
		if (playback_buffer_progress != old_progress || playback_state != old_state) {
			wl_list_for_each(bm, &mons, link) {
				if (bm->movies_view.visible && bm->movies_view.in_detail_view)
					media_view_render_detail(bm, MEDIA_VIEW_MOVIES);
				if (bm->tvshows_view.visible && bm->tvshows_view.in_detail_view)
					media_view_render_detail(bm, MEDIA_VIEW_TVSHOWS);
			}
		}
	}

	/* Re-arm timer for next poll (1s during buffering, 3s otherwise) */
	{
		int interval = (playback_state == PLAYBACK_BUFFERING) ? 1000 : 3000;
		if (media_view_poll_timer)
			wl_event_source_timer_update(media_view_poll_timer, interval);
	}

	return 0;
}

int
media_view_refresh(Monitor *m, MediaViewType type)
{
	MediaGridView *view;
	FILE *fp;
	char cmd[512];
	char buffer[1024 * 1024];  /* 1MB buffer for JSON */
	size_t bytes_read;
	const char *endpoint;
	uint32_t new_hash = 5381;

	if (!m) return 0;
	view = media_get_view(m, type);
	if (!view) return 0;

	view->needs_refresh = 0;
	view->last_refresh_ms = monotonic_msec();

	/* Run discovery to find local servers */
	run_local_discovery();

	endpoint = (type == MEDIA_VIEW_MOVIES) ? "/api/movies" : "/api/tvshows";

	/* Temporary storage for all items from all servers */
	#define MAX_TEMP_ITEMS 2000
	MediaItem **temp_items = calloc(MAX_TEMP_ITEMS, sizeof(MediaItem *));
	int total_items = 0;

	/* Fetch from all configured/discovered servers */
	MediaServer *servers;
	int server_count = get_all_media_servers(&servers);

	if (server_count == 0) {
		/* Fallback to localhost */
		snprintf(cmd, sizeof(cmd), "curl -s 'http://localhost:8080%s' 2>/dev/null", endpoint);
		fp = popen(cmd, "r");
		if (fp) {
			bytes_read = fread(buffer, 1, sizeof(buffer) - 1, fp);
			pclose(fp);
			if (bytes_read > 0) {
				buffer[bytes_read] = '\0';
				for (size_t i = 0; i < bytes_read; i++)
					new_hash = ((new_hash << 5) + new_hash) + (unsigned char)buffer[i];
				total_items = parse_media_json(buffer, "http://localhost:8080", temp_items, MAX_TEMP_ITEMS);
			}
		}
	} else {
		for (int s = 0; s < server_count && total_items < MAX_TEMP_ITEMS; s++) {
			snprintf(cmd, sizeof(cmd), "curl -s --connect-timeout 2 '%s%s' 2>/dev/null",
				 servers[s].url, endpoint);
			fp = popen(cmd, "r");
			if (!fp) continue;

			bytes_read = fread(buffer, 1, sizeof(buffer) - 1, fp);
			pclose(fp);
			if (bytes_read == 0) continue;
			buffer[bytes_read] = '\0';

			/* Update hash with this server's data */
			for (size_t i = 0; i < bytes_read; i++)
				new_hash = ((new_hash << 5) + new_hash) + (unsigned char)buffer[i];

			int parsed = parse_media_json(buffer, servers[s].url,
						      &temp_items[total_items], MAX_TEMP_ITEMS - total_items);
			total_items += parsed;
			wlr_log(WLR_DEBUG, "Fetched %d items from %s", parsed, servers[s].url);
		}
	}

	/* Skip update if data hasn't changed */
	if (view->items && new_hash == view->last_data_hash) {
		for (int i = 0; i < total_items; i++)
			free(temp_items[i]);
		free(temp_items);
		return 0;
	}
	view->last_data_hash = new_hash;

	/* Data changed - free old items */
	media_view_free_items(view);

	/* Deduplicate by tmdb_id: keep only the item with highest server_priority
	 * For items without tmdb_id (0), keep all of them */
	MediaItem *last = NULL;
	int count = 0;

	for (int i = 0; i < total_items; i++) {
		MediaItem *item = temp_items[i];
		if (!item) continue;

		int dominated = 0;

		/* Check if another item with same tmdb_id has higher priority */
		if (item->tmdb_id > 0) {
			for (int j = 0; j < total_items; j++) {
				if (i == j || !temp_items[j]) continue;
				MediaItem *other = temp_items[j];
				if (other->tmdb_id == item->tmdb_id &&
				    other->type == item->type &&
				    other->season == item->season &&
				    other->episode == item->episode) {
					/* Same content - check priority */
					if (other->server_priority > item->server_priority) {
						dominated = 1;
						break;
					}
					/* If same priority, keep first one (lower index) */
					if (other->server_priority == item->server_priority && j < i) {
						dominated = 1;
						break;
					}
				}
			}
		}

		if (dominated) {
			/* This item is dominated by a better source - discard */
			free(item);
			temp_items[i] = NULL;
		} else {
			/* Keep this item - add to linked list */
			if (!view->items) {
				view->items = item;
			} else {
				last->next = item;
			}
			last = item;
			item->next = NULL;
			count++;
		}
	}

	free(temp_items);
	view->item_count = count;

	wlr_log(WLR_INFO, "Media view: loaded %d items for %s (after dedup from %d servers)",
		count, type == MEDIA_VIEW_MOVIES ? "Movies" : "TV-shows", server_count);
	return 1;  /* Data changed */
}

void
media_view_load_poster(MediaItem *item, int target_w, int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GdkPixbuf *scaled = NULL;
	GdkPixbuf *cropped = NULL;
	GError *gerr = NULL;
	int orig_w, orig_h, scaled_w, scaled_h, crop_x, crop_y;
	double scale_w, scale_h, scale;
	guchar *pixels;
	int nchan, stride;
	uint8_t *argb = NULL;
	size_t bufsize;
	struct wlr_buffer *buf = NULL;

	if (!item || item->poster_loaded)
		return;

	item->poster_loaded = 1;

	/* Check if poster path exists */
	if (item->poster_path[0] == '\0' || strcmp(item->poster_path, "null") == 0)
		return;

	/* Check if it's a local file */
	if (item->poster_path[0] != '/')
		return;

	pixbuf = gdk_pixbuf_new_from_file(item->poster_path, &gerr);
	if (!pixbuf) {
		if (gerr) {
			wlr_log(WLR_DEBUG, "Failed to load poster %s: %s",
				item->poster_path, gerr->message);
			g_error_free(gerr);
		}
		return;
	}

	orig_w = gdk_pixbuf_get_width(pixbuf);
	orig_h = gdk_pixbuf_get_height(pixbuf);
	if (orig_w <= 0 || orig_h <= 0 || target_w <= 0 || target_h <= 0) {
		g_object_unref(pixbuf);
		return;
	}

	/* Scale-to-fill: scale so the image covers the entire target area */
	scale_w = (double)target_w / (double)orig_w;
	scale_h = (double)target_h / (double)orig_h;
	scale = (scale_w > scale_h) ? scale_w : scale_h;  /* Use larger scale to cover */

	scaled_w = (int)lround(orig_w * scale);
	scaled_h = (int)lround(orig_h * scale);
	if (scaled_w < target_w) scaled_w = target_w;
	if (scaled_h < target_h) scaled_h = target_h;

	scaled = gdk_pixbuf_scale_simple(pixbuf, scaled_w, scaled_h, GDK_INTERP_BILINEAR);
	g_object_unref(pixbuf);
	if (!scaled)
		return;

	/* Crop to center */
	crop_x = (scaled_w - target_w) / 2;
	crop_y = (scaled_h - target_h) / 2;
	if (crop_x < 0) crop_x = 0;
	if (crop_y < 0) crop_y = 0;

	cropped = gdk_pixbuf_new_subpixbuf(scaled, crop_x, crop_y, target_w, target_h);
	if (!cropped) {
		g_object_unref(scaled);
		return;
	}

	/* Convert to ARGB32 for wlr_buffer */
	nchan = gdk_pixbuf_get_n_channels(cropped);
	stride = gdk_pixbuf_get_rowstride(cropped);
	pixels = gdk_pixbuf_get_pixels(cropped);

	bufsize = (size_t)target_w * (size_t)target_h * 4;
	argb = malloc(bufsize);
	if (!argb) {
		g_object_unref(cropped);
		g_object_unref(scaled);
		return;
	}

	for (int y = 0; y < target_h; y++) {
		guchar *row = pixels + y * stride;
		for (int x = 0; x < target_w; x++) {
			uint8_t r = row[x * nchan + 0];
			uint8_t g = row[x * nchan + 1];
			uint8_t b = row[x * nchan + 2];
			uint8_t a = (nchan >= 4) ? row[x * nchan + 3] : 255;
			size_t off = ((size_t)y * (size_t)target_w + (size_t)x) * 4;
			/* ARGB32 format */
			argb[off + 0] = b;
			argb[off + 1] = g;
			argb[off + 2] = r;
			argb[off + 3] = a;
		}
	}

	buf = statusbar_buffer_from_argb32_raw((uint32_t *)argb, target_w, target_h);
	free(argb);
	g_object_unref(cropped);
	g_object_unref(scaled);

	if (buf) {
		item->poster_buf = buf;
		item->poster_w = target_w;
		item->poster_h = target_h;
	}
}

void
media_view_load_backdrop(MediaItem *item, int target_w, int target_h)
{
	GdkPixbuf *pixbuf = NULL;
	GdkPixbuf *scaled = NULL;
	GdkPixbuf *cropped = NULL;
	GError *gerr = NULL;
	int orig_w, orig_h, scaled_w, scaled_h, crop_x, crop_y;
	double scale_w, scale_h, scale;
	guchar *pixels;
	int nchan, stride;
	uint8_t *argb = NULL;
	size_t bufsize;
	struct wlr_buffer *buf = NULL;

	if (!item || item->backdrop_loaded)
		return;

	item->backdrop_loaded = 1;

	if (item->backdrop_path[0] == '\0')
		return;

	pixbuf = gdk_pixbuf_new_from_file(item->backdrop_path, &gerr);
	if (!pixbuf) {
		if (gerr) g_error_free(gerr);
		return;
	}

	orig_w = gdk_pixbuf_get_width(pixbuf);
	orig_h = gdk_pixbuf_get_height(pixbuf);

	scale_w = (double)target_w / orig_w;
	scale_h = (double)target_h / orig_h;
	scale = (scale_w > scale_h) ? scale_w : scale_h;

	scaled_w = (int)(orig_w * scale);
	scaled_h = (int)(orig_h * scale);

	scaled = gdk_pixbuf_scale_simple(pixbuf, scaled_w, scaled_h, GDK_INTERP_BILINEAR);
	g_object_unref(pixbuf);

	if (!scaled)
		return;

	crop_x = (scaled_w - target_w) / 2;
	crop_y = (scaled_h - target_h) / 2;

	cropped = gdk_pixbuf_new_subpixbuf(scaled, crop_x, crop_y, target_w, target_h);
	if (!cropped) {
		g_object_unref(scaled);
		return;
	}

	pixels = gdk_pixbuf_get_pixels(cropped);
	nchan = gdk_pixbuf_get_n_channels(cropped);
	stride = gdk_pixbuf_get_rowstride(cropped);

	bufsize = target_w * target_h * 4;
	argb = malloc(bufsize);
	if (!argb) {
		g_object_unref(cropped);
		g_object_unref(scaled);
		return;
	}

	for (int y = 0; y < target_h; y++) {
		guchar *row = pixels + y * stride;
		for (int x = 0; x < target_w; x++) {
			guchar r = row[x * nchan + 0];
			guchar g = row[x * nchan + 1];
			guchar b = row[x * nchan + 2];
			guchar a = (nchan == 4) ? row[x * nchan + 3] : 255;

			size_t off = (y * target_w + x) * 4;
			argb[off + 0] = b;
			argb[off + 1] = g;
			argb[off + 2] = r;
			argb[off + 3] = a;
		}
	}

	buf = statusbar_buffer_from_argb32_raw((uint32_t *)argb, target_w, target_h);
	free(argb);
	g_object_unref(cropped);
	g_object_unref(scaled);

	if (buf) {
		item->backdrop_buf = buf;
		item->backdrop_w = target_w;
		item->backdrop_h = target_h;
	}
}

void
media_view_free_seasons(MediaGridView *view)
{
	MediaSeason *s = view->seasons;
	while (s) {
		MediaSeason *next = s->next;
		free(s);
		s = next;
	}
	view->seasons = NULL;
	view->season_count = 0;
}

void
media_view_free_episodes(MediaGridView *view)
{
	MediaItem *item = view->episodes;
	while (item) {
		MediaItem *next = item->next;
		if (item->poster_buf)
			wlr_buffer_drop(item->poster_buf);
		if (item->backdrop_buf)
			wlr_buffer_drop(item->backdrop_buf);
		free(item);
		item = next;
	}
	view->episodes = NULL;
	view->episode_count = 0;
}

void
media_render_buffering_overlay(Monitor *m, MediaViewType type)
{
	MediaGridView *view;
	struct wlr_scene_node *node, *tmp;
	float bg_color[4] = {0.02f, 0.02f, 0.05f, 0.98f};
	float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float warning_color[4] = {1.0f, 0.8f, 0.2f, 1.0f};
	float progress_bg[4] = {0.15f, 0.15f, 0.2f, 1.0f};
	float progress_fill[4] = {0.2f, 0.6f, 1.0f, 1.0f};
	StatusModule mod = {0};
	struct wlr_scene_tree *text_tree;
	int center_x, center_y, line_y, line_height;
	char *msg;
	char line[256];

	if (!m || !statusfont.font)
		return;

	view = media_get_view(m, type);
	if (!view || !view->visible || !view->tree)
		return;

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &view->tree->children, link) {
		wlr_scene_node_destroy(node);
	}
	view->grid = NULL;
	view->detail_panel = NULL;

	/* Dark background */
	drawrect(view->tree, 0, 0, view->width, view->height, bg_color);

	/* Center the message */
	center_x = view->width / 2;
	center_y = view->height / 2 - 100;

	/* Warning icon area */
	{
		float icon_bg[4] = {0.15f, 0.12f, 0.05f, 1.0f};
		drawrect(view->tree, center_x - 40, center_y - 80, 80, 80, icon_bg);
	}

	/* Warning symbol */
	text_tree = wlr_scene_tree_create(view->tree);
	if (text_tree) {
		wlr_scene_node_set_position(&text_tree->node, center_x - 20, center_y - 60);
		mod.tree = text_tree;
		tray_render_label(&mod, "!", 0, 40, warning_color);
	}

	/* Render message lines */
	msg = playback_message;
	line_y = center_y + 20;
	line_height = 32;

	while (*msg) {
		/* Extract line until \n */
		char *nl = strchr(msg, '\n');
		size_t len = nl ? (size_t)(nl - msg) : strlen(msg);
		int text_w;

		if (len >= sizeof(line)) len = sizeof(line) - 1;
		strncpy(line, msg, len);
		line[len] = '\0';

		/* Center text */
		text_w = (int)len * 10;  /* Approximate */
		text_tree = wlr_scene_tree_create(view->tree);
		if (text_tree) {
			wlr_scene_node_set_position(&text_tree->node, center_x - text_w / 2, line_y);
			mod.tree = text_tree;
			tray_render_label(&mod, line, 0, 24, text_color);
		}
		line_y += line_height;

		if (nl)
			msg = nl + 1;
		else
			break;
	}

	/* Progress bar */
	{
		int bar_w = 400;
		int bar_h = 20;
		int bar_x = center_x - bar_w / 2;
		int bar_y = line_y + 40;
		int fill_w;
		char pct[32];

		drawrect(view->tree, bar_x, bar_y, bar_w, bar_h, progress_bg);
		fill_w = (bar_w * playback_buffer_progress) / 100;
		if (fill_w > 0)
			drawrect(view->tree, bar_x, bar_y, fill_w, bar_h, progress_fill);

		/* Progress percentage */
		snprintf(pct, sizeof(pct), "%d%%", playback_buffer_progress);
		text_tree = wlr_scene_tree_create(view->tree);
		if (text_tree) {
			wlr_scene_node_set_position(&text_tree->node, center_x - 20, bar_y + bar_h + 20);
			mod.tree = text_tree;
			tray_render_label(&mod, pct, 0, 24, text_color);
		}

		/* Cancel hint */
		{
			float hint_color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
			text_tree = wlr_scene_tree_create(view->tree);
			if (text_tree) {
				wlr_scene_node_set_position(&text_tree->node, center_x - 140, view->height - 60);
				mod.tree = text_tree;
				tray_render_label(&mod, "Press B or Escape to cancel", 0, 24, hint_color);
			}
		}
	}
}

void
media_view_render_detail(Monitor *m, MediaViewType type)
{
	MediaGridView *view;
	struct wlr_scene_node *node, *tmp;
	float bg_color[4] = {0.05f, 0.05f, 0.08f, 0.98f};
	float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float dim_text[4] = {0.7f, 0.7f, 0.7f, 1.0f};
	float accent_color[4] = {0.2f, 0.5f, 1.0f, 1.0f};
	float panel_bg[4] = {0.08f, 0.08f, 0.12f, 0.95f};
	float selected_bg[4] = {0.15f, 0.35f, 0.65f, 0.9f};
	float hover_bg[4] = {0.12f, 0.12f, 0.18f, 0.9f};

	if (!m || !statusfont.font)
		return;

	view = media_get_view(m, type);
	if (!view || !view->visible || !view->tree || !view->in_detail_view)
		return;

	/* If buffering, show buffering overlay instead */
	if (playback_state == PLAYBACK_BUFFERING) {
		media_render_buffering_overlay(m, type);
		return;
	}

	MediaItem *item = view->detail_item;
	if (!item) return;

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &view->tree->children, link) {
		wlr_scene_node_destroy(node);
	}
	view->grid = NULL;
	view->detail_panel = NULL;

	/* Background */
	drawrect(view->tree, 0, 0, view->width, view->height, bg_color);

	/* Load and display backdrop if available */
	int backdrop_h = view->height * 60 / 100;  /* 60% of screen height */
	if (!item->backdrop_loaded && item->backdrop_path[0]) {
		media_view_load_backdrop(item, view->width, backdrop_h);
	}
	if (item->backdrop_buf) {
		struct wlr_scene_buffer *backdrop = wlr_scene_buffer_create(view->tree, item->backdrop_buf);
		if (backdrop) {
			wlr_scene_node_set_position(&backdrop->node, 0, 0);
		}
		/* Gradient overlay on backdrop */
		float gradient[4] = {0.05f, 0.05f, 0.08f, 0.7f};
		drawrect(view->tree, 650, backdrop_h - 180, view->width - 650, 180, gradient);
	}

	/* Left side: poster and info */
	int poster_w = 280;
	int poster_h = 420;
	int info_x = 60;
	int info_y = (backdrop_h > 0 ? backdrop_h - poster_h / 2 : 60) + 30;

	/* Poster */
	if (!item->poster_loaded) {
		media_view_load_poster(item, poster_w, poster_h);
	}
	if (item->poster_buf) {
		/* Poster shadow/border */
		float poster_shadow[4] = {0.0f, 0.0f, 0.0f, 0.5f};
		drawrect(view->tree, info_x - 4, info_y - 4, poster_w + 8, poster_h + 8, poster_shadow);

		struct wlr_scene_buffer *poster = wlr_scene_buffer_create(view->tree, item->poster_buf);
		if (poster) {
			wlr_scene_node_set_position(&poster->node, info_x, info_y);
		}
	}

	/* Details box - starts at top-right corner of thumbnail (with gap for shadow) */
	int details_box_x = info_x + poster_w + 72;  /* 72px gap after poster shadow */
	int details_box_y = info_y;  /* Aligned with poster top */
	int details_box_w = 300;
	int details_box_h = 380;
	float details_box_bg[4] = {0.08f, 0.08f, 0.12f, 0.95f};  /* Match panel_bg */
	drawrect(view->tree, details_box_x, details_box_y, details_box_w, details_box_h, details_box_bg);

	/* Title and metadata inside details box */
	int text_x = details_box_x + 15;  /* Inside details box with padding */
	int text_y = details_box_y + 20;
	int text_max_w = 200;  /* Max width before wrapping */
	int chars_per_line = text_max_w / 9;  /* ~9px per char at size 16 */

	/* Bold label color (brighter white) */
	float label_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

	/* Title (bold/large) */
	struct wlr_scene_tree *title_tree = wlr_scene_tree_create(view->tree);
	if (title_tree) {
		wlr_scene_node_set_position(&title_tree->node, text_x, text_y);
		StatusModule mod = {0};
		mod.tree = title_tree;
		tray_render_label(&mod, item->title, 0, 28, text_color);
	}
	text_y += 50;

	/* Score */
	if (item->rating > 0) {
		/* Bold label */
		struct wlr_scene_tree *label_tree = wlr_scene_tree_create(view->tree);
		if (label_tree) {
			wlr_scene_node_set_position(&label_tree->node, text_x, text_y);
			StatusModule mod = {0};
			mod.tree = label_tree;
			tray_render_label(&mod, "Score:", 0, 16, label_color);
		}
		/* Value */
		char score_val[32];
		snprintf(score_val, sizeof(score_val), "%.1f", item->rating);
		struct wlr_scene_tree *val_tree = wlr_scene_tree_create(view->tree);
		if (val_tree) {
			wlr_scene_node_set_position(&val_tree->node, text_x + 160, text_y);
			StatusModule mod = {0};
			mod.tree = val_tree;
			tray_render_label(&mod, score_val, 0, 16, dim_text);
		}
		text_y += 35;
	}

	/* Total duration */
	{
		/* For TV shows: use TMDB episode_runtime * total_episodes if available */
		int total_duration_sec = item->duration;
		if (type == MEDIA_VIEW_TVSHOWS && item->tmdb_episode_runtime > 0 && item->tmdb_total_episodes > 0) {
			total_duration_sec = item->tmdb_episode_runtime * item->tmdb_total_episodes * 60;
		}
		if (total_duration_sec > 0) {
			/* Bold label */
			struct wlr_scene_tree *label_tree = wlr_scene_tree_create(view->tree);
			if (label_tree) {
				wlr_scene_node_set_position(&label_tree->node, text_x, text_y);
				StatusModule mod = {0};
				mod.tree = label_tree;
				tray_render_label(&mod, "Total duration:", 0, 16, label_color);
			}
			/* Value */
			char duration_val[32];
			int hours = total_duration_sec / 3600;
			int mins = (total_duration_sec % 3600) / 60;
			if (hours > 0)
				snprintf(duration_val, sizeof(duration_val), "%dh %dm", hours, mins);
			else
				snprintf(duration_val, sizeof(duration_val), "%dm", mins);
			struct wlr_scene_tree *val_tree = wlr_scene_tree_create(view->tree);
			if (val_tree) {
				wlr_scene_node_set_position(&val_tree->node, text_x + 160, text_y);
				StatusModule mod = {0};
				mod.tree = val_tree;
				tray_render_label(&mod, duration_val, 0, 16, dim_text);
			}
			text_y += 35;
		}
	}

	/* Sjanger - with text wrapping */
	if (item->genres[0]) {
		/* Bold label */
		struct wlr_scene_tree *label_tree = wlr_scene_tree_create(view->tree);
		if (label_tree) {
			wlr_scene_node_set_position(&label_tree->node, text_x, text_y);
			StatusModule mod = {0};
			mod.tree = label_tree;
			tray_render_label(&mod, "Sjanger:", 0, 16, label_color);
		}
		text_y += 22;
		/* Value with word wrap at 250px */
		char wrapped_genre[512];
		int src_idx = 0, dst_idx = 0, line_len = 0;
		while (item->genres[src_idx] && dst_idx < (int)sizeof(wrapped_genre) - 1) {
			char c = item->genres[src_idx++];
			if (line_len >= chars_per_line && (c == ' ' || c == ',')) {
				wrapped_genre[dst_idx++] = '\n';
				line_len = 0;
				if (c == ' ') continue;
			}
			wrapped_genre[dst_idx++] = c;
			line_len++;
		}
		wrapped_genre[dst_idx] = '\0';
		/* Render wrapped lines */
		char *line = wrapped_genre;
		while (line && *line) {
			char *nl = strchr(line, '\n');
			char line_buf[256];
			if (nl) {
				int len = nl - line;
				if (len > 255) len = 255;
				strncpy(line_buf, line, len);
				line_buf[len] = '\0';
				line = nl + 1;
			} else {
				strncpy(line_buf, line, 255);
				line_buf[255] = '\0';
				line = NULL;
			}
			struct wlr_scene_tree *val_tree = wlr_scene_tree_create(view->tree);
			if (val_tree) {
				wlr_scene_node_set_position(&val_tree->node, text_x, text_y);
				StatusModule mod = {0};
				mod.tree = val_tree;
				tray_render_label(&mod, line_buf, 0, 16, dim_text);
			}
			text_y += 20;
		}
		text_y += 15;
	}

	/* Release date */
	if (item->year > 0) {
		/* Bold label */
		struct wlr_scene_tree *label_tree = wlr_scene_tree_create(view->tree);
		if (label_tree) {
			wlr_scene_node_set_position(&label_tree->node, text_x, text_y);
			StatusModule mod = {0};
			mod.tree = label_tree;
			tray_render_label(&mod, "Release date:", 0, 16, label_color);
		}
		/* Value */
		char year_val[16];
		snprintf(year_val, sizeof(year_val), "%d", item->year);
		struct wlr_scene_tree *val_tree = wlr_scene_tree_create(view->tree);
		if (val_tree) {
			wlr_scene_node_set_position(&val_tree->node, text_x + 160, text_y);
			StatusModule mod = {0};
			mod.tree = val_tree;
			tray_render_label(&mod, year_val, 0, 16, dim_text);
		}
		text_y += 35;
	}

	/* Seasons and Episodes (for TV shows only) */
	if (type == MEDIA_VIEW_TVSHOWS && (item->tmdb_total_seasons > 0 || view->season_count > 0)) {
		/* Use TMDB totals if available, otherwise fall back to local counts */
		int display_seasons = item->tmdb_total_seasons > 0 ? item->tmdb_total_seasons : view->season_count;
		int display_episodes = item->tmdb_total_episodes;
		if (display_episodes <= 0) {
			display_episodes = 0;
			MediaSeason *s;
			for (s = view->seasons; s; s = s->next) {
				display_episodes += s->episode_count;
			}
		}

		/* Seasons - bold label */
		struct wlr_scene_tree *slabel_tree = wlr_scene_tree_create(view->tree);
		if (slabel_tree) {
			wlr_scene_node_set_position(&slabel_tree->node, text_x, text_y);
			StatusModule mod = {0};
			mod.tree = slabel_tree;
			tray_render_label(&mod, "Seasons:", 0, 16, label_color);
		}
		char seasons_val[16];
		snprintf(seasons_val, sizeof(seasons_val), "%d", display_seasons);
		struct wlr_scene_tree *sval_tree = wlr_scene_tree_create(view->tree);
		if (sval_tree) {
			wlr_scene_node_set_position(&sval_tree->node, text_x + 160, text_y);
			StatusModule mod = {0};
			mod.tree = sval_tree;
			tray_render_label(&mod, seasons_val, 0, 16, dim_text);
		}
		text_y += 35;

		/* Episodes - bold label */
		struct wlr_scene_tree *elabel_tree = wlr_scene_tree_create(view->tree);
		if (elabel_tree) {
			wlr_scene_node_set_position(&elabel_tree->node, text_x, text_y);
			StatusModule mod = {0};
			mod.tree = elabel_tree;
			tray_render_label(&mod, "Episodes:", 0, 16, label_color);
		}
		char episodes_val[16];
		snprintf(episodes_val, sizeof(episodes_val), "%d", display_episodes);
		struct wlr_scene_tree *eval_tree = wlr_scene_tree_create(view->tree);
		if (eval_tree) {
			wlr_scene_node_set_position(&eval_tree->node, text_x + 160, text_y);
			StatusModule mod = {0};
			mod.tree = eval_tree;
			tray_render_label(&mod, episodes_val, 0, 16, dim_text);
		}
		text_y += 35;
	}

	/* Next episode / Ended status (TV shows only) */
	if (type == MEDIA_VIEW_TVSHOWS && item->tmdb_status[0]) {
		struct wlr_scene_tree *nlabel_tree = wlr_scene_tree_create(view->tree);
		if (nlabel_tree) {
			wlr_scene_node_set_position(&nlabel_tree->node, text_x, text_y);
			StatusModule mod = {0};
			mod.tree = nlabel_tree;
			tray_render_label(&mod, "Next episode:", 0, 16, label_color);
		}
		char next_val[32];
		if (strcmp(item->tmdb_status, "Ended") == 0 ||
		    strcmp(item->tmdb_status, "Canceled") == 0) {
			snprintf(next_val, sizeof(next_val), "Ended");
		} else if (item->tmdb_next_episode[0]) {
			snprintf(next_val, sizeof(next_val), "%s", item->tmdb_next_episode);
		} else {
			snprintf(next_val, sizeof(next_val), "TBA");
		}
		struct wlr_scene_tree *nval_tree = wlr_scene_tree_create(view->tree);
		if (nval_tree) {
			wlr_scene_node_set_position(&nval_tree->node, text_x + 160, text_y);
			StatusModule mod = {0};
			mod.tree = nval_tree;
			tray_render_label(&mod, next_val, 0, 16, dim_text);
		}
		text_y += 35;
	}

	/* Overview bar - horizontal bar to the right of details_box (transparent, no background) */
	if (item->overview[0]) {
		int overview_bar_x = details_box_x + details_box_w + 85;
		int overview_bar_y = details_box_y;
		int overview_bar_w = view->width - overview_bar_x - 40;
		int overview_bar_h = 80;

		/* Word wrap overview text for horizontal bar */
		char wrapped[4096];
		int wrap_width = 800 / 9;  /* Max 800px line width for font size 16 */
		int src_idx = 0, dst_idx = 0, line_len = 0;
		int max_lines = 12;  /* Allow more lines to show full overview */
		int current_line = 0;

		while (item->overview[src_idx] && dst_idx < (int)sizeof(wrapped) - 1 && current_line < max_lines) {
			char c = item->overview[src_idx++];
			if (c == '\n' || (line_len >= wrap_width && c == ' ')) {
				wrapped[dst_idx++] = '\n';
				line_len = 0;
				current_line++;
			} else {
				wrapped[dst_idx++] = c;
				line_len++;
			}
		}
		wrapped[dst_idx] = '\0';

		/* Render overview lines in horizontal bar */
		int ov_text_x = overview_bar_x - 35;
		int ov_text_y = overview_bar_y + 12;
		char *line = wrapped;
		int lines_rendered = 0;
		while (*line && lines_rendered < max_lines) {
			char *newline = strchr(line, '\n');
			if (newline) *newline = '\0';

			struct wlr_scene_tree *line_tree = wlr_scene_tree_create(view->tree);
			if (line_tree) {
				wlr_scene_node_set_position(&line_tree->node, ov_text_x, ov_text_y);
				StatusModule mod = {0};
				mod.tree = line_tree;
				tray_render_label(&mod, line, 0, 16, dim_text);
			}
			ov_text_y += 20;
			lines_rendered++;

			if (newline) {
				line = newline + 1;
			} else {
				break;
			}
		}
	}

	/* Play button (for movies) or focus indicator */
	if (type == MEDIA_VIEW_MOVIES || (type == MEDIA_VIEW_TVSHOWS && view->detail_focus == DETAIL_FOCUS_INFO)) {
		int btn_y = info_y + poster_h + 30;
		int btn_w = 160;
		int btn_h = 50;

		float *btn_bg = (view->detail_focus == DETAIL_FOCUS_INFO) ? selected_bg : hover_bg;
		drawrect(view->tree, info_x, btn_y, btn_w, btn_h, btn_bg);

		struct wlr_scene_tree *btn_tree = wlr_scene_tree_create(view->tree);
		if (btn_tree) {
			wlr_scene_node_set_position(&btn_tree->node, info_x + 50, btn_y + 15);
			StatusModule mod = {0};
			mod.tree = btn_tree;
			tray_render_label(&mod, "▶ Play", 0, 20, text_color);
		}
	}

	/* For TV shows: show seasons and episodes columns - placed on right side */
	if (type == MEDIA_VIEW_TVSHOWS) {
		/* Position columns on the right side */
		int panel_y = info_y + poster_h - 190;  /* 150px higher */
		int panel_h = view->height - panel_y - 30;
		int col_gap = 15;
		int season_col_w = 550;
		int episode_col_w = 550;
		int episode_col_x = view->width - 40 - episode_col_w;
		int panel_x = episode_col_x - col_gap - season_col_w;
		int row_h = 40;
		int ep_row_h = 55;
		int header_h = 45;

		/* Seasons column with clipping area */
		drawrect(view->tree, panel_x, panel_y, season_col_w, panel_h, panel_bg);

		/* Seasons header */
		struct wlr_scene_tree *sh_tree = wlr_scene_tree_create(view->tree);
		if (sh_tree) {
			wlr_scene_node_set_position(&sh_tree->node, panel_x + 12, panel_y + 12);
			StatusModule mod = {0};
			mod.tree = sh_tree;
			tray_render_label(&mod, "Seasons", 0, 16, accent_color);
		}

		/* Season list with scroll */
		int content_y = panel_y + header_h;
		int content_h = panel_h - header_h - 5;
		int visible_rows = content_h / row_h;

		/* Ensure selected season is visible */
		if (view->selected_season_idx < view->season_scroll_offset) {
			view->season_scroll_offset = view->selected_season_idx;
		} else if (view->selected_season_idx >= view->season_scroll_offset + visible_rows) {
			view->season_scroll_offset = view->selected_season_idx - visible_rows + 1;
		}
		if (view->season_scroll_offset < 0) view->season_scroll_offset = 0;

		MediaSeason *season = view->seasons;
		int season_idx = 0;
		/* Skip to scroll offset */
		while (season && season_idx < view->season_scroll_offset) {
			season = season->next;
			season_idx++;
		}

		int season_y = content_y;
		while (season && season_y + row_h <= panel_y + panel_h) {
			int is_selected = (season_idx == view->selected_season_idx);
			int is_focused = (view->detail_focus == DETAIL_FOCUS_SEASONS);

			float *row_bg = (is_selected && is_focused) ? selected_bg :
			                (is_selected ? hover_bg : NULL);

			if (row_bg) {
				drawrect(view->tree, panel_x + 4, season_y, season_col_w - 8, row_h - 4, row_bg);
			}

			char season_str[64];
			snprintf(season_str, sizeof(season_str), "Season %d", season->season);

			struct wlr_scene_tree *s_tree = wlr_scene_tree_create(view->tree);
			if (s_tree) {
				wlr_scene_node_set_position(&s_tree->node, panel_x + 12, season_y + 10);
				StatusModule mod = {0};
				mod.tree = s_tree;
				float *color = is_selected ? text_color : dim_text;
				tray_render_label(&mod, season_str, 0, 14, color);
			}

			season_y += row_h;
			season_idx++;
			season = season->next;
		}

		/* Scroll indicator for seasons if needed */
		int total_seasons = 0;
		for (MediaSeason *s = view->seasons; s; s = s->next) total_seasons++;
		if (total_seasons > visible_rows) {
			int scroll_track_h = content_h - 10;
			int scroll_thumb_h = (visible_rows * scroll_track_h) / total_seasons;
			if (scroll_thumb_h < 20) scroll_thumb_h = 20;
			int scroll_thumb_y = content_y + 5 + (view->season_scroll_offset * (scroll_track_h - scroll_thumb_h)) / (total_seasons - visible_rows);
			float scroll_color[4] = {0.3f, 0.3f, 0.4f, 0.6f};
			drawrect(view->tree, panel_x + season_col_w - 6, scroll_thumb_y, 4, scroll_thumb_h, scroll_color);
		}

		/* Episodes column */
		drawrect(view->tree, episode_col_x, panel_y, episode_col_w, panel_h, panel_bg);

		/* Episodes header */
		struct wlr_scene_tree *eh_tree = wlr_scene_tree_create(view->tree);
		if (eh_tree) {
			char ep_header[64];
			snprintf(ep_header, sizeof(ep_header), "Season %d", view->selected_season);
			wlr_scene_node_set_position(&eh_tree->node, episode_col_x + 12, panel_y + 12);
			StatusModule mod = {0};
			mod.tree = eh_tree;
			tray_render_label(&mod, ep_header, 0, 16, accent_color);
		}

		/* Episode list with scroll */
		int ep_content_y = panel_y + header_h;
		int ep_content_h = panel_h - header_h - 5;
		int ep_visible_rows = ep_content_h / ep_row_h;

		/* Ensure selected episode is visible */
		if (view->selected_episode_idx < view->episode_scroll_offset) {
			view->episode_scroll_offset = view->selected_episode_idx;
		} else if (view->selected_episode_idx >= view->episode_scroll_offset + ep_visible_rows) {
			view->episode_scroll_offset = view->selected_episode_idx - ep_visible_rows + 1;
		}
		if (view->episode_scroll_offset < 0) view->episode_scroll_offset = 0;

		MediaItem *ep = view->episodes;
		int ep_idx = 0;
		/* Skip to scroll offset */
		while (ep && ep_idx < view->episode_scroll_offset) {
			ep = ep->next;
			ep_idx++;
		}

		int ep_y = ep_content_y;
		while (ep && ep_y + ep_row_h <= panel_y + panel_h) {
			int is_selected = (ep_idx == view->selected_episode_idx);
			int is_focused = (view->detail_focus == DETAIL_FOCUS_EPISODES);

			float *row_bg = (is_selected && is_focused) ? selected_bg :
			                (is_selected ? hover_bg : NULL);

			if (row_bg) {
				drawrect(view->tree, episode_col_x + 4, ep_y, episode_col_w - 12, ep_row_h - 4, row_bg);
			}

			/* Episode number */
			char ep_str[300];
			snprintf(ep_str, sizeof(ep_str), "Episode %d", ep->episode);

			struct wlr_scene_tree *ep_tree = wlr_scene_tree_create(view->tree);
			if (ep_tree) {
				wlr_scene_node_set_position(&ep_tree->node, episode_col_x + 12, ep_y + 18);
				StatusModule mod = {0};
				mod.tree = ep_tree;
				float *color = is_selected ? text_color : dim_text;
				tray_render_label(&mod, ep_str, 0, 14, color);
			}

			ep_y += ep_row_h;
			ep_idx++;
			ep = ep->next;
		}

		/* Scroll indicator for episodes if needed */
		if (view->episode_count > ep_visible_rows) {
			int ep_scroll_track_h = ep_content_h - 10;
			int ep_scroll_thumb_h = (ep_visible_rows * ep_scroll_track_h) / view->episode_count;
			if (ep_scroll_thumb_h < 20) ep_scroll_thumb_h = 20;
			int ep_scroll_thumb_y = ep_content_y + 5;
			if (view->episode_count > ep_visible_rows) {
				ep_scroll_thumb_y += (view->episode_scroll_offset * (ep_scroll_track_h - ep_scroll_thumb_h)) / (view->episode_count - ep_visible_rows);
			}
			float scroll_color[4] = {0.3f, 0.3f, 0.4f, 0.6f};
			drawrect(view->tree, episode_col_x + episode_col_w - 8, ep_scroll_thumb_y, 4, ep_scroll_thumb_h, scroll_color);
		}
	}

	/* Navigation hint at bottom */
	struct wlr_scene_tree *hint_tree = wlr_scene_tree_create(view->tree);
	if (hint_tree) {
		const char *hint = type == MEDIA_VIEW_MOVIES ?
		                   "A/Enter: Play   B/Esc: Back" :
		                   "←→: Switch columns   ↑↓: Navigate   A/Enter: Play   B/Esc: Back";
		wlr_scene_node_set_position(&hint_tree->node, 60, view->height - 40);
		StatusModule mod = {0};
		mod.tree = hint_tree;
		float hint_color[4] = {0.5f, 0.5f, 0.5f, 1.0f};
		tray_render_label(&mod, hint, 0, 14, hint_color);
	}
}

void
media_view_enter_detail(Monitor *m, MediaViewType type)
{
	MediaGridView *view = media_get_view(m, type);
	if (!view || !view->items) return;

	/* Find selected item */
	MediaItem *item = view->items;
	for (int i = 0; i < view->selected_idx && item; i++) {
		item = item->next;
	}
	if (!item) return;

	view->in_detail_view = 1;
	view->detail_item = item;
	view->detail_focus = DETAIL_FOCUS_INFO;

	/* For TV shows, load seasons */
	if (type == MEDIA_VIEW_TVSHOWS) {
		const char *show_name = item->title[0] ? item->title : item->show_name;
		media_view_fetch_seasons(view, show_name);

		/* Select first season and load its episodes */
		view->selected_season_idx = 0;
		view->season_scroll_offset = 0;
		if (view->seasons) {
			view->selected_season = view->seasons->season;
			media_view_fetch_episodes(view, show_name, view->selected_season);
		}
		view->selected_episode_idx = 0;
		view->episode_scroll_offset = 0;
		view->detail_focus = DETAIL_FOCUS_SEASONS;
	}

	media_view_render_detail(m, type);
}

void
media_view_exit_detail(Monitor *m, MediaViewType type)
{
	MediaGridView *view = media_get_view(m, type);
	if (!view) return;

	view->in_detail_view = 0;
	view->detail_item = NULL;

	/* Free TV show data */
	media_view_free_seasons(view);
	media_view_free_episodes(view);

	media_view_render(m, type);
}

void
media_view_ensure_visible(Monitor *m, MediaViewType type)
{
	MediaGridView *view = media_get_view(m, type);
	if (!view) return;

	int padding = MEDIA_GRID_PADDING;
	int gap = MEDIA_GRID_GAP;
	int grid_width = view->width - 2 * padding;
	int cols = 5;
	int tile_w = (grid_width - ((cols - 1) * gap)) / cols;
	int tile_h = (tile_w * 3) / 2;  /* 2:3 poster ratio */

	int row = view->selected_idx / cols;
	int y_start = row * (tile_h + gap);
	int y_end = y_start + tile_h;
	int visible_h = view->height - 2 * padding;

	if (y_start < view->scroll_offset) {
		view->scroll_offset = y_start;
	} else if (y_end > view->scroll_offset + visible_h) {
		view->scroll_offset = y_end - visible_h;
	}

	if (view->scroll_offset < 0)
		view->scroll_offset = 0;
}

void
media_view_render(Monitor *m, MediaViewType type)
{
	MediaGridView *view;
	struct wlr_scene_node *node, *tmp;
	int padding = MEDIA_GRID_PADDING;
	int gap = MEDIA_GRID_GAP;
	float tile_color[4] = {0.12f, 0.12f, 0.15f, 0.95f};
	float text_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	float name_bg[4] = {0.0f, 0.0f, 0.0f, 0.75f};
	float dim_color[4] = {0.05f, 0.05f, 0.08f, 0.98f};

	if (!m || !statusfont.font)
		return;

	view = media_get_view(m, type);
	if (!view || !view->visible || !view->tree)
		return;

	/* Refresh if needed */
	if (view->needs_refresh || !view->items) {
		media_view_refresh(m, type);
	}

	/* Clear previous content */
	wl_list_for_each_safe(node, tmp, &view->tree->children, link) {
		wlr_scene_node_destroy(node);
	}
	view->grid = NULL;
	view->detail_panel = NULL;

	/* Background dim */
	drawrect(view->tree, 0, 0, view->width, view->height, dim_color);

	/* Calculate grid dimensions */
	int grid_width = view->width - 2 * padding;
	int grid_height = view->height - 2 * padding;

	/* Fixed 5 columns */
	view->cols = 5;
	int tile_w = (grid_width - ((view->cols - 1) * gap)) / view->cols;
	int tile_h = (tile_w * 3) / 2;  /* 2:3 poster ratio */
	int name_bar_h = 32;

	/* Ensure selected is visible */
	media_view_ensure_visible(m, type);

	/* Create grid container */
	view->grid = wlr_scene_tree_create(view->tree);
	if (!view->grid) return;
	wlr_scene_node_set_position(&view->grid->node, padding, padding);

	/* Draw tiles */
	MediaItem *item = view->items;
	int idx = 0;
	int visible_start = view->scroll_offset / (tile_h + gap);
	int visible_rows = (grid_height + gap) / (tile_h + gap) + 2;

	while (item) {
		int row = idx / view->cols;
		int col = idx % view->cols;

		/* Only render visible tiles */
		if (row >= visible_start && row < visible_start + visible_rows) {
			int tx = col * (tile_w + gap);
			int ty = row * (tile_h + gap) - view->scroll_offset;

			if (ty + tile_h > 0 && ty < grid_height) {
				int is_selected = (idx == view->selected_idx);
				int glow_size = is_selected ? 6 : 0;

				struct wlr_scene_tree *tile = wlr_scene_tree_create(view->grid);
				if (!tile) {
					item = item->next;
					idx++;
					continue;
				}

				if (is_selected) {
					wlr_scene_node_set_position(&tile->node, tx - glow_size, ty - glow_size);

					/* Draw glow effect */
					float glow1[4] = {0.2f, 0.5f, 1.0f, 0.2f};
					float glow2[4] = {0.3f, 0.6f, 1.0f, 0.35f};
					drawrect(tile, 0, 0, tile_w + glow_size * 2, tile_h + glow_size * 2, glow1);
					drawrect(tile, 2, 2, tile_w + glow_size * 2 - 4, tile_h + glow_size * 2 - 4, glow2);
					drawrect(tile, glow_size, glow_size, tile_w, tile_h, tile_color);
				} else {
					wlr_scene_node_set_position(&tile->node, tx, ty);
					drawrect(tile, 0, 0, tile_w, tile_h, tile_color);
				}

				int img_offset = is_selected ? glow_size : 0;

				/* Load and display poster */
				if (!item->poster_loaded) {
					media_view_load_poster(item, tile_w, tile_h - name_bar_h);
				}
				if (item->poster_buf) {
					struct wlr_scene_buffer *img = wlr_scene_buffer_create(tile, item->poster_buf);
					if (img) {
						wlr_scene_node_set_position(&img->node, img_offset, img_offset);
					}
				}

				/* Title bar at bottom */
				drawrect(tile, img_offset, img_offset + tile_h - name_bar_h - glow_size,
				         tile_w, name_bar_h, name_bg);

				/* Title text */
				struct wlr_scene_tree *text_tree = wlr_scene_tree_create(tile);
				if (text_tree) {
					char display_title[128];
					if (type == MEDIA_VIEW_TVSHOWS) {
						/* TV-shows view: show title with year in parentheses */
						const char *title = item->title[0] ? item->title :
						                    (item->show_name[0] ? item->show_name : "Unknown");
						if (item->year > 0)
							snprintf(display_title, sizeof(display_title), "%s (%d)", title, item->year);
						else
							snprintf(display_title, sizeof(display_title), "%s", title);
					} else {
						/* Movies view: show movie title with year */
						if (item->year > 0)
							snprintf(display_title, sizeof(display_title), "%s (%d)", item->title, item->year);
						else
							snprintf(display_title, sizeof(display_title), "%s", item->title);
					}

					/* Truncate if too long */
					int max_chars = tile_w / 8;
					if ((int)strlen(display_title) > max_chars && max_chars > 3) {
						display_title[max_chars - 3] = '.';
						display_title[max_chars - 2] = '.';
						display_title[max_chars - 1] = '.';
						display_title[max_chars] = '\0';
					}

					wlr_scene_node_set_position(&text_tree->node,
						img_offset + 6, img_offset + tile_h - name_bar_h + 6 - glow_size);
					StatusModule mod = {0};
					mod.tree = text_tree;
					tray_render_label(&mod, display_title, 0, 18, text_color);
				}

				/* Rating badge for selected */
				if (is_selected && item->rating > 0) {
					char rating_str[16];
					snprintf(rating_str, sizeof(rating_str), "%.1f", item->rating);
					float rating_bg[4] = {0.1f, 0.1f, 0.1f, 0.85f};
					float rating_fg[4] = {1.0f, 0.8f, 0.2f, 1.0f};

					struct wlr_scene_tree *badge = wlr_scene_tree_create(tile);
					if (badge) {
						int badge_w = 40, badge_h = 22;
						wlr_scene_node_set_position(&badge->node,
							img_offset + tile_w - badge_w - 4, img_offset + 4);
						drawrect(badge, 0, 0, badge_w, badge_h, rating_bg);
						StatusModule mod = {0};
						mod.tree = badge;
						tray_render_label(&mod, rating_str, 6, 16, rating_fg);
					}
				}
			}
		}

		item = item->next;
		idx++;
	}

	/* Show count in corner */
	char count_str[64];
	snprintf(count_str, sizeof(count_str), "%d %s",
	         view->item_count,
	         type == MEDIA_VIEW_MOVIES ? "Movies" : "Episodes");
	struct wlr_scene_tree *count_tree = wlr_scene_tree_create(view->tree);
	if (count_tree) {
		wlr_scene_node_set_position(&count_tree->node, padding, view->height - 30);
		StatusModule mod = {0};
		mod.tree = count_tree;
		float count_color[4] = {0.7f, 0.7f, 0.7f, 1.0f};
		tray_render_label(&mod, count_str, 0, 18, count_color);
	}
}

void
media_view_show(Monitor *m, MediaViewType type)
{
	MediaGridView *view;
	float dim_color[4] = {0.05f, 0.05f, 0.08f, 0.98f};

	if (!m) return;

	view = media_get_view(m, type);
	if (!view) return;

	/* Hide the other media view first to ensure clean state */
	if (type == MEDIA_VIEW_MOVIES)
		media_view_hide(m, MEDIA_VIEW_TVSHOWS);
	else
		media_view_hide(m, MEDIA_VIEW_MOVIES);

	if (view->visible) return;

	view->visible = 1;
	/* Set view tag: Movies = Tag 2 (bit 1), TV-shows = Tag 1 (bit 0) */
	view->view_tag = (type == MEDIA_VIEW_MOVIES) ? (1 << 1) : (1 << 0);
	view->width = m->m.width;
	view->height = m->m.height;
	view->view_type = type;
	view->selected_idx = 0;
	view->scroll_offset = 0;
	view->needs_refresh = 1;

	strcpy(view->server_url, get_media_server_url());

	/* Create scene tree on LyrBlock layer (same as other HTPC overlays) */
	if (!view->tree) {
		view->tree = wlr_scene_tree_create(layers[LyrBlock]);
	}
	if (view->tree) {
		wlr_scene_node_set_position(&view->tree->node, m->m.x, m->m.y);
		wlr_scene_node_set_enabled(&view->tree->node, true);
		wlr_scene_node_raise_to_top(&view->tree->node);
	}

	/* Hide mouse cursor in media views */
	wlr_cursor_set_surface(cursor, NULL, 0, 0);

	media_view_render(m, type);
}

void
media_view_hide(Monitor *m, MediaViewType type)
{
	MediaGridView *view;

	if (!m) return;

	view = media_get_view(m, type);
	if (!view || !view->visible) return;

	view->visible = 0;

	if (view->tree) {
		wlr_scene_node_set_enabled(&view->tree->node, false);
	}
}

void
media_view_hide_all(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		media_view_hide(m, MEDIA_VIEW_MOVIES);
		media_view_hide(m, MEDIA_VIEW_TVSHOWS);
	}
}

Monitor *
media_view_visible_monitor(void)
{
	Monitor *m;
	wl_list_for_each(m, &mons, link) {
		if (m->movies_view.visible || m->tvshows_view.visible)
			return m;
	}
	return NULL;
}

void
media_view_scroll(Monitor *m, MediaViewType type, int delta)
{
	MediaGridView *view = media_get_view(m, type);
	if (!view) return;

	view->scroll_offset += delta;
	if (view->scroll_offset < 0)
		view->scroll_offset = 0;

	media_view_render(m, type);
}

int
handle_playback_osd_input(int button)
{
	int handled = 0;

	/* Any input shows OSD */
	osd_visible = 1;
	osd_show_time = monotonic_msec();

	switch (button) {
	case BTN_SOUTH:  /* A button - pause/play or select menu item */
		if (osd_menu_open == OSD_MENU_NONE) {
			if (active_videoplayer)
				videoplayer_toggle_pause(active_videoplayer);
		} else if (osd_menu_open == OSD_MENU_SOUND) {
			if (active_videoplayer && osd_menu_selection < audio_track_count)
				videoplayer_set_audio_track(active_videoplayer, osd_menu_selection);
			osd_menu_open = OSD_MENU_NONE;
		} else if (osd_menu_open == OSD_MENU_SUBTITLES) {
			if (active_videoplayer) {
				if (osd_menu_selection == 0)
					videoplayer_set_subtitle_track(active_videoplayer, -1);  /* Off */
				else if (osd_menu_selection <= subtitle_track_count)
					videoplayer_set_subtitle_track(active_videoplayer, osd_menu_selection - 1);
			}
			osd_menu_open = OSD_MENU_NONE;
		}
		handled = 1;
		break;

	case BTN_EAST:   /* B button - close menu or stop playback */
		if (osd_menu_open != OSD_MENU_NONE) {
			osd_menu_open = OSD_MENU_NONE;
		} else {
			stop_integrated_player();
			hide_playback_osd();
			return 1;  /* Don't render OSD after stopping */
		}
		handled = 1;
		break;

	case BTN_DPAD_UP:
		if (osd_menu_open != OSD_MENU_NONE && osd_menu_selection > 0)
			osd_menu_selection--;
		handled = 1;
		break;

	case BTN_DPAD_DOWN:
		if (osd_menu_open == OSD_MENU_SOUND && osd_menu_selection < audio_track_count - 1)
			osd_menu_selection++;
		else if (osd_menu_open == OSD_MENU_SUBTITLES && osd_menu_selection < subtitle_track_count)
			osd_menu_selection++;
		handled = 1;
		break;

	case BTN_DPAD_LEFT:
		if (osd_menu_open == OSD_MENU_SUBTITLES) {
			osd_menu_open = OSD_MENU_SOUND;
			osd_menu_selection = 0;
		} else if (osd_menu_open == OSD_MENU_NONE) {
			/* Could add seek left here */
		}
		handled = 1;
		break;

	case BTN_DPAD_RIGHT:
		if (osd_menu_open == OSD_MENU_NONE) {
			osd_menu_open = OSD_MENU_SOUND;
			osd_menu_selection = 0;
		} else if (osd_menu_open == OSD_MENU_SOUND) {
			osd_menu_open = OSD_MENU_SUBTITLES;
			osd_menu_selection = 0;
		}
		handled = 1;
		break;
	}

	/* Re-render OSD after input */
	if (handled)
		render_playback_osd();

	return handled;
}

int
media_view_handle_detail_button(Monitor *m, MediaViewType type, int button)
{
	MediaGridView *view = media_get_view(m, type);
	if (!view) return 0;

	int needs_render = 0;

	/* If playback is active, handle playback controls */
	if (playback_state == PLAYBACK_PLAYING) {
		return handle_playback_osd_input(button);
	}

	switch (button) {
	case BTN_EAST:   /* B button - go back to grid or cancel buffering */
		if (playback_state == PLAYBACK_BUFFERING) {
			media_cancel_buffering();
			media_view_render_detail(m, type);
			return 1;
		}
		media_view_exit_detail(m, type);
		return 1;

	case BTN_SOUTH:  /* A button - play selected */
		if (type == MEDIA_VIEW_MOVIES) {
			/* Play movie */
			if (view->detail_item) {
				wlr_log(WLR_INFO, "Play movie: %s", view->detail_item->title);
				media_start_playback(view->detail_item, 1);
				needs_render = 1;
			}
		} else if (type == MEDIA_VIEW_TVSHOWS) {
			if (view->detail_focus == DETAIL_FOCUS_EPISODES && view->episodes) {
				/* Play selected episode */
				MediaItem *ep = view->episodes;
				for (int i = 0; i < view->selected_episode_idx && ep; i++)
					ep = ep->next;
				if (ep) {
					wlr_log(WLR_INFO, "Play episode: S%02dE%02d %s",
					        ep->season, ep->episode, ep->episode_title);
					media_start_playback(ep, 0);
					needs_render = 1;
				}
			} else if (view->detail_focus == DETAIL_FOCUS_SEASONS) {
				/* Move to episodes column */
				view->detail_focus = DETAIL_FOCUS_EPISODES;
				view->selected_episode_idx = 0;
				needs_render = 1;
			}
		}
		break;

	case BTN_DPAD_UP:
		if (type == MEDIA_VIEW_TVSHOWS) {
			if (view->detail_focus == DETAIL_FOCUS_SEASONS) {
				if (view->selected_season_idx > 0) {
					view->selected_season_idx--;
					/* Load episodes for new season */
					MediaSeason *s = view->seasons;
					for (int i = 0; i < view->selected_season_idx && s; i++)
						s = s->next;
					if (s) {
						view->selected_season = s->season;
						const char *show = view->detail_item->title[0] ?
						                   view->detail_item->title : view->detail_item->show_name;
						media_view_fetch_episodes(view, show, s->season);
						view->selected_episode_idx = 0;
						view->episode_scroll_offset = 0;
					}
					needs_render = 1;
				}
			} else if (view->detail_focus == DETAIL_FOCUS_EPISODES) {
				if (view->selected_episode_idx > 0) {
					view->selected_episode_idx--;
					needs_render = 1;
				}
			}
		}
		break;

	case BTN_DPAD_DOWN:
		if (type == MEDIA_VIEW_TVSHOWS) {
			if (view->detail_focus == DETAIL_FOCUS_SEASONS) {
				if (view->selected_season_idx < view->season_count - 1) {
					view->selected_season_idx++;
					/* Load episodes for new season */
					MediaSeason *s = view->seasons;
					for (int i = 0; i < view->selected_season_idx && s; i++)
						s = s->next;
					if (s) {
						view->selected_season = s->season;
						const char *show = view->detail_item->title[0] ?
						                   view->detail_item->title : view->detail_item->show_name;
						media_view_fetch_episodes(view, show, s->season);
						view->selected_episode_idx = 0;
						view->episode_scroll_offset = 0;
					}
					needs_render = 1;
				}
			} else if (view->detail_focus == DETAIL_FOCUS_EPISODES) {
				if (view->selected_episode_idx < view->episode_count - 1) {
					view->selected_episode_idx++;
					needs_render = 1;
				}
			}
		}
		break;

	case BTN_DPAD_LEFT:
		if (type == MEDIA_VIEW_TVSHOWS) {
			if (view->detail_focus == DETAIL_FOCUS_EPISODES) {
				view->detail_focus = DETAIL_FOCUS_SEASONS;
				needs_render = 1;
			}
		}
		break;

	case BTN_DPAD_RIGHT:
		if (type == MEDIA_VIEW_TVSHOWS) {
			if (view->detail_focus == DETAIL_FOCUS_SEASONS && view->episode_count > 0) {
				view->detail_focus = DETAIL_FOCUS_EPISODES;
				needs_render = 1;
			}
		}
		break;

	case BTN_MODE:   /* Guide button */
		return 0;

	default:
		return 0;
	}

	if (needs_render) {
		media_view_render_detail(m, type);
	}

	return 1;
}

int
media_view_handle_button(Monitor *m, MediaViewType type, int button, int value)
{
	MediaGridView *view;

	if (!m) return 0;

	view = media_get_view(m, type);
	if (!view) return 0;

	/* Only handle input if view is visible AND on current tag */
	if (!htpc_view_is_active(m, view->view_tag, view->visible)) return 0;

	/* Only handle button press */
	if (value != 1) return 0;

	/* Handle detail view separately */
	if (view->in_detail_view) {
		return media_view_handle_detail_button(m, type, button);
	}

	int old_idx = view->selected_idx;
	int cols = view->cols > 0 ? view->cols : 5;

	switch (button) {
	case BTN_DPAD_UP:
		if (view->selected_idx >= cols)
			view->selected_idx -= cols;
		break;
	case BTN_DPAD_DOWN:
		if (view->selected_idx + cols < view->item_count)
			view->selected_idx += cols;
		break;
	case BTN_DPAD_LEFT:
		if (view->selected_idx > 0)
			view->selected_idx--;
		break;
	case BTN_DPAD_RIGHT:
		if (view->selected_idx < view->item_count - 1)
			view->selected_idx++;
		break;
	case BTN_EAST:   /* B button - do nothing in grid view (main views can't be closed) */
		return 1;
	case BTN_MODE:   /* Guide button - let main handler show menu overlay */
		return 0;
	case BTN_SOUTH:  /* A button - enter detail view */
		media_view_enter_detail(m, type);
		return 1;
	case BTN_TL:     /* LB - scroll up */
		view->scroll_offset -= 200;
		if (view->scroll_offset < 0) view->scroll_offset = 0;
		break;
	case BTN_TR:     /* RB - scroll down */
		view->scroll_offset += 200;
		break;
	default:
		return 0;
	}

	if (view->selected_idx != old_idx || button == BTN_TL || button == BTN_TR) {
		media_view_render(m, type);
	}

	return 1;
}

int
handle_playback_key(xkb_keysym_t sym)
{
	int handled = 0;

	/* Any input shows OSD */
	osd_visible = 1;
	osd_show_time = monotonic_msec();

	switch (sym) {
	case XKB_KEY_space:
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (osd_menu_open == OSD_MENU_NONE) {
			if (active_videoplayer)
				videoplayer_toggle_pause(active_videoplayer);
		} else if (osd_menu_open == OSD_MENU_SOUND) {
			if (active_videoplayer && osd_menu_selection < audio_track_count)
				videoplayer_set_audio_track(active_videoplayer, osd_menu_selection);
			osd_menu_open = OSD_MENU_NONE;
		} else if (osd_menu_open == OSD_MENU_SUBTITLES) {
			if (active_videoplayer) {
				if (osd_menu_selection == 0)
					videoplayer_set_subtitle_track(active_videoplayer, -1);  /* Off */
				else if (osd_menu_selection <= subtitle_track_count)
					videoplayer_set_subtitle_track(active_videoplayer, osd_menu_selection - 1);
			}
			osd_menu_open = OSD_MENU_NONE;
		}
		handled = 1;
		break;

	case XKB_KEY_Escape:
	case XKB_KEY_q:
		if (osd_menu_open != OSD_MENU_NONE) {
			osd_menu_open = OSD_MENU_NONE;
		} else {
			stop_integrated_player();
			hide_playback_osd();
			return 1;  /* Don't render OSD after stopping */
		}
		handled = 1;
		break;

	case XKB_KEY_Up:
	case XKB_KEY_k:
		if (osd_menu_open != OSD_MENU_NONE && osd_menu_selection > 0)
			osd_menu_selection--;
		handled = 1;
		break;

	case XKB_KEY_Down:
	case XKB_KEY_j:
		if (osd_menu_open == OSD_MENU_SOUND && osd_menu_selection < audio_track_count - 1)
			osd_menu_selection++;
		else if (osd_menu_open == OSD_MENU_SUBTITLES && osd_menu_selection < subtitle_track_count)
			osd_menu_selection++;
		handled = 1;
		break;

	case XKB_KEY_Left:
	case XKB_KEY_h:
		if (osd_menu_open == OSD_MENU_SUBTITLES) {
			osd_menu_open = OSD_MENU_SOUND;
			osd_menu_selection = 0;
		}
		handled = 1;
		break;

	case XKB_KEY_Right:
	case XKB_KEY_l:
		if (osd_menu_open == OSD_MENU_NONE) {
			osd_menu_open = OSD_MENU_SOUND;
			osd_menu_selection = 0;
		} else if (osd_menu_open == OSD_MENU_SOUND) {
			osd_menu_open = OSD_MENU_SUBTITLES;
			osd_menu_selection = 0;
		}
		handled = 1;
		break;

	case XKB_KEY_a:  /* Audio shortcut */
		osd_menu_open = OSD_MENU_SOUND;
		osd_menu_selection = 0;
		handled = 1;
		break;

	case XKB_KEY_s:  /* Subtitle shortcut */
		osd_menu_open = OSD_MENU_SUBTITLES;
		osd_menu_selection = 0;
		handled = 1;
		break;
	}

	/* Re-render OSD after input */
	if (handled)
		render_playback_osd();

	return handled;
}

int
media_view_handle_detail_key(Monitor *m, MediaViewType type, xkb_keysym_t sym)
{
	MediaGridView *view = media_get_view(m, type);
	if (!view) return 0;

	int needs_render = 0;

	/* If playback is active, handle playback controls */
	if (playback_state == PLAYBACK_PLAYING) {
		return handle_playback_key(sym);
	}

	switch (sym) {
	case XKB_KEY_Escape:
		if (playback_state == PLAYBACK_BUFFERING) {
			media_cancel_buffering();
			media_view_render_detail(m, type);
			return 1;
		}
		media_view_exit_detail(m, type);
		return 1;

	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (type == MEDIA_VIEW_MOVIES) {
			/* Play movie */
			if (view->detail_item) {
				wlr_log(WLR_INFO, "Play movie: %s", view->detail_item->title);
				media_start_playback(view->detail_item, 1);
				needs_render = 1;
			}
		} else if (type == MEDIA_VIEW_TVSHOWS) {
			if (view->detail_focus == DETAIL_FOCUS_EPISODES && view->episodes) {
				MediaItem *ep = view->episodes;
				for (int i = 0; i < view->selected_episode_idx && ep; i++)
					ep = ep->next;
				if (ep) {
					wlr_log(WLR_INFO, "Play episode: S%02dE%02d %s",
					        ep->season, ep->episode, ep->episode_title);
					media_start_playback(ep, 0);
					needs_render = 1;
				}
			} else if (view->detail_focus == DETAIL_FOCUS_SEASONS) {
				view->detail_focus = DETAIL_FOCUS_EPISODES;
				view->selected_episode_idx = 0;
				needs_render = 1;
			}
		}
		break;

	case XKB_KEY_Up:
	case XKB_KEY_k:
		if (type == MEDIA_VIEW_TVSHOWS) {
			if (view->detail_focus == DETAIL_FOCUS_SEASONS) {
				if (view->selected_season_idx > 0) {
					view->selected_season_idx--;
					MediaSeason *s = view->seasons;
					for (int i = 0; i < view->selected_season_idx && s; i++)
						s = s->next;
					if (s) {
						view->selected_season = s->season;
						const char *show = view->detail_item->title[0] ?
						                   view->detail_item->title : view->detail_item->show_name;
						media_view_fetch_episodes(view, show, s->season);
						view->selected_episode_idx = 0;
						view->episode_scroll_offset = 0;
					}
					needs_render = 1;
				}
			} else if (view->detail_focus == DETAIL_FOCUS_EPISODES) {
				if (view->selected_episode_idx > 0) {
					view->selected_episode_idx--;
					needs_render = 1;
				}
			}
		}
		break;

	case XKB_KEY_Down:
	case XKB_KEY_j:
		if (type == MEDIA_VIEW_TVSHOWS) {
			if (view->detail_focus == DETAIL_FOCUS_SEASONS) {
				if (view->selected_season_idx < view->season_count - 1) {
					view->selected_season_idx++;
					MediaSeason *s = view->seasons;
					for (int i = 0; i < view->selected_season_idx && s; i++)
						s = s->next;
					if (s) {
						view->selected_season = s->season;
						const char *show = view->detail_item->title[0] ?
						                   view->detail_item->title : view->detail_item->show_name;
						media_view_fetch_episodes(view, show, s->season);
						view->selected_episode_idx = 0;
						view->episode_scroll_offset = 0;
					}
					needs_render = 1;
				}
			} else if (view->detail_focus == DETAIL_FOCUS_EPISODES) {
				if (view->selected_episode_idx < view->episode_count - 1) {
					view->selected_episode_idx++;
					needs_render = 1;
				}
			}
		}
		break;

	case XKB_KEY_Left:
	case XKB_KEY_h:
		if (type == MEDIA_VIEW_TVSHOWS) {
			if (view->detail_focus == DETAIL_FOCUS_EPISODES) {
				view->detail_focus = DETAIL_FOCUS_SEASONS;
				needs_render = 1;
			}
		}
		break;

	case XKB_KEY_Right:
	case XKB_KEY_l:
		if (type == MEDIA_VIEW_TVSHOWS) {
			if (view->detail_focus == DETAIL_FOCUS_SEASONS && view->episode_count > 0) {
				view->detail_focus = DETAIL_FOCUS_EPISODES;
				needs_render = 1;
			}
		}
		break;

	default:
		return 0;
	}

	if (needs_render) {
		media_view_render_detail(m, type);
	}

	return 1;
}

int
media_view_handle_key(Monitor *m, MediaViewType type, xkb_keysym_t sym)
{
	MediaGridView *view;

	if (!m) return 0;

	view = media_get_view(m, type);
	if (!view) return 0;

	/* Only handle input if view is visible AND on current tag */
	if (!htpc_view_is_active(m, view->view_tag, view->visible)) return 0;

	/* Handle detail view separately */
	if (view->in_detail_view) {
		return media_view_handle_detail_key(m, type, sym);
	}

	int old_idx = view->selected_idx;
	int cols = view->cols > 0 ? view->cols : 5;

	switch (sym) {
	case XKB_KEY_Escape:
		/* Do nothing in grid view - main views can't be closed */
		return 1;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		/* Enter detail view */
		media_view_enter_detail(m, type);
		return 1;
	case XKB_KEY_Up:
	case XKB_KEY_k:
		if (view->selected_idx >= cols)
			view->selected_idx -= cols;
		break;
	case XKB_KEY_Down:
	case XKB_KEY_j:
		if (view->selected_idx + cols < view->item_count)
			view->selected_idx += cols;
		break;
	case XKB_KEY_Left:
	case XKB_KEY_h:
		if (view->selected_idx > 0)
			view->selected_idx--;
		break;
	case XKB_KEY_Right:
	case XKB_KEY_l:
		if (view->selected_idx < view->item_count - 1)
			view->selected_idx++;
		break;
	case XKB_KEY_Page_Up:
		view->scroll_offset -= 400;
		if (view->scroll_offset < 0) view->scroll_offset = 0;
		media_view_render(m, type);
		return 1;
	case XKB_KEY_Page_Down:
		view->scroll_offset += 400;
		media_view_render(m, type);
		return 1;
	default:
		return 0;
	}

	if (view->selected_idx != old_idx) {
		media_view_render(m, type);
	}

	return 1;
}

