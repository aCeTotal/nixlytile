/*
 * Nixly Media Server
 * Lossless streaming server for movies and TV shows with TMDB metadata
 *
 * - Serves media files without transcoding (full quality)
 * - SQLite database with TMDB metadata
 * - HTTP server with range request support for seeking
 * - Real-time file watching with inotify
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#include "database.h"
#include "scanner.h"
#include "config.h"
#include "tmdb.h"
#include "watcher.h"
#include "transcoder.h"

/* No connection limit - kernel handles backlog */
#define BUFFER_SIZE 262144  /* 256KB for efficient streaming */
#define STREAM_CHUNK_SIZE 1048576  /* 1MB sendfile chunks */
#define MAX_PATH 4096
#define MAX_HEADER 8192
#define DISCOVERY_PORT 8081
#define DISCOVERY_MAGIC "NIXLY_DISCOVER"
#define DISCOVERY_RESPONSE "NIXLY_SERVER"

static int server_fd = -1;
static int discovery_fd = -1;
static volatile int running = 1;
static volatile int transcoder_restart_pending = 0;

/* Connection limiting based on bandwidth */
#define STREAM_BITRATE_MBPS 70  /* Assumed bitrate per stream for lossless 4K */
static volatile int active_streams = 0;
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;


static int get_max_streams(void) {
    if (server_config.upload_mbps <= 0) return 1000;  /* Effectively unlimited */
    return server_config.upload_mbps / STREAM_BITRATE_MBPS;
}

/* Periodic sync thread - checks for changes every 5 minutes as backup to inotify */
static void *sync_thread(void *arg) {
    (void)arg;

    while (running) {
        /* Sleep for 5 minutes, but check transcoder restart flag every second */
        for (int i = 0; i < 300 && running; i++) {
            sleep(1);

            /* Check if transcoder needs restart (new files arrived while running) */
            if (transcoder_restart_pending &&
                transcoder_get_state() == TRANSCODE_IDLE) {
                transcoder_restart_pending = 0;
                printf("Restarting transcoder for newly detected files...\n");
                transcoder_start();
            }
        }

        if (!running) break;

        printf("Periodic sync: Checking for changes...\n");

        /* Remove entries for deleted files */
        int removed = database_cleanup_missing();
        if (removed > 0) {
            printf("Periodic sync: Removed %d missing entries\n", removed);
        }

        /* Scan for new files in nixly_ready_media directories */
        int before = database_get_count();
        for (int i = 0; i < server_config.converted_path_count; i++) {
            char ready_path[MAX_PATH];
            snprintf(ready_path, sizeof(ready_path), "%s/nixly_ready_media",
                     server_config.converted_paths[i]);
            scanner_scan_directory(ready_path);
        }
        int after = database_get_count();

        if (after != before) {
            printf("Periodic sync: Media count changed %d -> %d\n", before, after);
        }

        /* Fetch any missing TMDB metadata */
        scanner_fetch_missing_tmdb();

        /* Refresh show status (next episode dates, ended status) */
        scanner_refresh_show_status();

        /* Start transcoder if idle (will pick up any unconverted files) */
        if (transcoder_get_state() == TRANSCODE_IDLE) {
            transcoder_start();
        }
    }

    return NULL;
}

/* Discovery thread - responds to broadcast queries */
static void *discovery_thread(void *arg) {
    (void)arg;
    struct sockaddr_in addr, client_addr;
    socklen_t addr_len;
    char buf[128];
    char response[128];

    discovery_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (discovery_fd < 0) {
        perror("discovery socket");
        return NULL;
    }

    int reuse = 1;
    setsockopt(discovery_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DISCOVERY_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(discovery_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("discovery bind");
        close(discovery_fd);
        discovery_fd = -1;
        return NULL;
    }

    printf("Discovery: Listening on UDP port %d\n", DISCOVERY_PORT);

    while (running) {
        addr_len = sizeof(client_addr);
        ssize_t len = recvfrom(discovery_fd, buf, sizeof(buf) - 1, 0,
                               (struct sockaddr *)&client_addr, &addr_len);
        if (len > 0) {
            buf[len] = '\0';
            if (strncmp(buf, DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC)) == 0) {
                /* Respond with our port */
                snprintf(response, sizeof(response), "%s:%d", DISCOVERY_RESPONSE, server_config.port);

                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
                printf("Discovery: Request from %s, responding with port %d\n",
                       client_ip, server_config.port);

                sendto(discovery_fd, response, strlen(response), 0,
                       (struct sockaddr *)&client_addr, addr_len);
            }
        }
    }

    close(discovery_fd);
    discovery_fd = -1;
    return NULL;
}

typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
    int is_local;  /* 1 if client is on local network */
} ClientConnection;

/* Check if client IP is on local network (private IP ranges) */
static int is_local_client(struct sockaddr_in *addr) {
    uint32_t ip = ntohl(addr->sin_addr.s_addr);

    /* 127.0.0.0/8 - localhost */
    if ((ip >> 24) == 127) return 1;

    /* 10.0.0.0/8 - private */
    if ((ip >> 24) == 10) return 1;

    /* 172.16.0.0/12 - private */
    if ((ip >> 20) == (172 << 4 | 1)) return 1;

    /* 192.168.0.0/16 - private */
    if ((ip >> 16) == (192 << 8 | 168)) return 1;

    return 0;
}

/* File change callback - keeps database in sync and triggers transcoding */
static void on_file_change(const char *filepath, int is_delete, WatchType type) {
    if (is_delete) {
        /* Remove from database */
        if (database_delete_by_path(filepath) == 0) {
            printf("DB: Removed %s\n", filepath);
        }
    } else {
        /* Check if it's a media file that needs processing */
        if (scanner_is_media_file(filepath)) {
            /* Check if this file is in a nixly_ready_media directory.
             * Files here are ready for serving - always add to DB and scrape,
             * regardless of whether they came from the transcoder or were
             * placed manually. */
            int is_ready_media = (strstr(filepath, "/nixly_ready_media/") != NULL);

            if (is_ready_media) {
                /* Ready media file - add to DB and fetch TMDB metadata */
                int id = scanner_scan_file(filepath, 1);
                if (id > 0) {
                    printf("DB: Added ready media %s (id=%d)\n", filepath, id);
                }
            } else {
                /* New source file detected - trigger transcoder if idle */
                printf("New media file detected: %s\n", filepath);
                if (transcoder_get_state() == TRANSCODE_IDLE) {
                    printf("Starting transcoder for new files...\n");
                    transcoder_start();
                } else {
                    /* Mark for restart when current run finishes */
                    transcoder_restart_pending = 1;
                    printf("Transcoder running, will restart when done\n");
                }
            }
        }
    }
}

/* HTTP response helpers */
static void send_response(int fd, int status, const char *status_text,
                          const char *content_type, const char *body, size_t body_len) {
    char header[MAX_HEADER];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, body_len);

    write(fd, header, header_len);
    if (body && body_len > 0) {
        write(fd, body, body_len);
    }
}

static void send_error(int fd, int status, const char *message) {
    char body[512];
    int body_len = snprintf(body, sizeof(body),
        "{\"error\": \"%s\"}", message);
    send_response(fd, status, message, "application/json", body, body_len);
}

/* Get MIME type for file extension */
static const char *get_mime_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";

    if (strcasecmp(ext, ".mp4") == 0) return "video/mp4";
    if (strcasecmp(ext, ".mkv") == 0) return "video/x-matroska";
    if (strcasecmp(ext, ".avi") == 0) return "video/x-msvideo";
    if (strcasecmp(ext, ".mov") == 0) return "video/quicktime";
    if (strcasecmp(ext, ".webm") == 0) return "video/webm";
    if (strcasecmp(ext, ".m4v") == 0) return "video/x-m4v";
    if (strcasecmp(ext, ".ts") == 0) return "video/mp2t";
    if (strcasecmp(ext, ".json") == 0) return "application/json";
    if (strcasecmp(ext, ".html") == 0) return "text/html";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".png") == 0) return "image/png";

    return "application/octet-stream";
}

/* Serve a local file (for cached images) */
static void serve_file(int fd, const char *filepath) {
    struct stat st;
    if (stat(filepath, &st) != 0) {
        send_error(fd, 404, "File not found");
        return;
    }

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 500, "Cannot open file");
        return;
    }

    const char *mime = get_mime_type(filepath);
    char header[MAX_HEADER];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: max-age=86400\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, st.st_size);

    write(fd, header, header_len);

    char buffer[BUFFER_SIZE];
    ssize_t bytes;
    while ((bytes = read(file_fd, buffer, sizeof(buffer))) > 0) {
        write(fd, buffer, bytes);
    }

    close(file_fd);
}

/* Stream file with range request support (for seeking in video)
 * Uses sendfile() for zero-copy kernel-to-socket transfer - most efficient
 * for lossless streaming of large media files */
static void stream_file(int fd, const char *filepath, const char *range_header) {
    struct stat st;
    if (stat(filepath, &st) != 0) {
        send_error(fd, 404, "File not found");
        return;
    }

    int file_fd = open(filepath, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 500, "Cannot open file");
        return;
    }

    off_t start = 0;
    off_t end = st.st_size - 1;
    int partial = 0;

    /* Parse Range header: bytes=start-end */
    if (range_header) {
        if (sscanf(range_header, "bytes=%ld-%ld", &start, &end) >= 1) {
            partial = 1;
            if (end <= 0 || end >= st.st_size) {
                end = st.st_size - 1;
            }
        }
    }

    off_t content_length = end - start + 1;
    const char *mime = get_mime_type(filepath);

    char header[MAX_HEADER];
    int header_len;

    if (partial) {
        header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 206 Partial Content\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Content-Range: bytes %ld-%ld/%ld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            mime, content_length, start, end, st.st_size);
    } else {
        header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            mime, st.st_size);
    }

    write(fd, header, header_len);

    /* Stream using sendfile() - zero-copy kernel-to-socket transfer
     * This is the most efficient way to stream large files:
     * - No user-space buffer copies
     * - Kernel handles DMA directly from disk to network
     * - Supports lossless transfer of any codec (video, audio, subtitles) */
    off_t offset = start;
    off_t remaining = content_length;

    while (remaining > 0 && running) {
        /* Send in chunks to allow checking 'running' flag and handle partial sends */
        size_t chunk = (remaining > STREAM_CHUNK_SIZE) ? STREAM_CHUNK_SIZE : remaining;
        ssize_t sent = sendfile(fd, file_fd, &offset, chunk);

        if (sent <= 0) {
            if (sent < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;  /* Retry on temporary errors */
            }
            break;  /* Client disconnected or error */
        }

        remaining -= sent;
    }

    close(file_fd);
}

/* URL-decode a percent-encoded string (e.g. %20 → space, + → space) */
static void url_decode(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_size; i++) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

/* Handle API requests */
static void handle_api(int fd, const char *path) {
    if (strcmp(path, "/api/status") == 0) {
        /* Status with server identity, rating, locality, and stream capacity
         * Priority: local servers get 1000 + rating, remote get just rating
         * Clients use priority to select best source for duplicate content */
        int max = get_max_streams();
        int rating = config_get_server_rating();
        int is_local = config_get_client_local();
        int priority = config_get_server_priority();
        char json[512];
        int len = snprintf(json, sizeof(json),
            "{\"status\":\"ok\","
            "\"server_id\":\"%s\","
            "\"server_name\":\"%s\","
            "\"rating\":%d,"
            "\"is_local\":%s,"
            "\"priority\":%d,"
            "\"upload_mbps\":%d,"
            "\"active_streams\":%d,"
            "\"max_streams\":%d}",
            server_config.server_id,
            server_config.server_name,
            rating,
            is_local ? "true" : "false",
            priority,
            server_config.upload_mbps,
            active_streams, max);
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else if (strcmp(path, "/api/library") == 0) {
        /* Return entire media library as JSON */
        char *json = database_get_library_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Database error");
        }
    }
    else if (strcmp(path, "/api/movies") == 0) {
        char *json = database_get_movies_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Database error");
        }
    }
    else if (strcmp(path, "/api/tvshows") == 0) {
        char *json = database_get_tvshows_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Database error");
        }
    }
    else if (strncmp(path, "/api/show/", 10) == 0) {
        /* /api/show/{show_name}/seasons or /api/show/{show_name}/episodes/{season} */
        char show_name[256];
        char *show_start = (char *)path + 10;
        char *seasons_pos = strstr(show_start, "/seasons");
        char *episodes_pos = strstr(show_start, "/episodes/");

        if (seasons_pos) {
            /* Extract show name (URL decoded) */
            size_t name_len = seasons_pos - show_start;
            if (name_len >= sizeof(show_name)) name_len = sizeof(show_name) - 1;
            strncpy(show_name, show_start, name_len);
            show_name[name_len] = '\0';

            /* Simple URL decode for spaces */
            for (char *p = show_name; *p; p++) {
                if (*p == '+') *p = ' ';
                else if (*p == '%' && p[1] == '2' && p[2] == '0') {
                    *p = ' ';
                    memmove(p + 1, p + 3, strlen(p + 3) + 1);
                }
            }

            char *json = database_get_show_seasons_json(show_name);
            if (json) {
                send_response(fd, 200, "OK", "application/json", json, strlen(json));
                free(json);
            } else {
                send_error(fd, 500, "Database error");
            }
        }
        else if (episodes_pos) {
            /* Extract show name */
            size_t name_len = episodes_pos - show_start;
            if (name_len >= sizeof(show_name)) name_len = sizeof(show_name) - 1;
            strncpy(show_name, show_start, name_len);
            show_name[name_len] = '\0';

            /* Simple URL decode */
            for (char *p = show_name; *p; p++) {
                if (*p == '+') *p = ' ';
                else if (*p == '%' && p[1] == '2' && p[2] == '0') {
                    *p = ' ';
                    memmove(p + 1, p + 3, strlen(p + 3) + 1);
                }
            }

            int season = atoi(episodes_pos + 10);
            char *json = database_get_show_episodes_json(show_name, season);
            if (json) {
                send_response(fd, 200, "OK", "application/json", json, strlen(json));
                free(json);
            } else {
                send_error(fd, 500, "Database error");
            }
        }
        else {
            send_error(fd, 400, "Invalid show endpoint");
        }
    }
    else if (strcmp(path, "/api/scan") == 0) {
        /* Trigger library rescan of nixly_ready_media directories */
        printf("API: Starting rescan...\n");
        int before = database_get_count();
        int total_found = 0;
        for (int i = 0; i < server_config.converted_path_count; i++) {
            char ready_path[MAX_PATH];
            snprintf(ready_path, sizeof(ready_path), "%s/nixly_ready_media",
                     server_config.converted_paths[i]);
            int found = scanner_scan_directory(ready_path);
            printf("API: Scanned %s -> %d new files\n", ready_path, found);
            if (found > 0) total_found += found;
        }
        int after = database_get_count();
        char json[512];
        int len = snprintf(json, sizeof(json),
            "{\"status\":\"scan complete\",\"paths_scanned\":%d,"
            "\"new_files\":%d,\"total_before\":%d,\"total_after\":%d}",
            server_config.converted_path_count, total_found, before, after);
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else if (strcmp(path, "/api/paths") == 0) {
        /* Diagnostic: show configured paths and what exists */
        size_t buf_size = 8192;
        size_t buf_used = 0;
        char *json = malloc(buf_size);
        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "{\"converted_path_count\":%d,\"converted_paths\":[",
            server_config.converted_path_count);
        for (int i = 0; i < server_config.converted_path_count; i++) {
            char ready_path[MAX_PATH];
            snprintf(ready_path, sizeof(ready_path), "%s/nixly_ready_media",
                     server_config.converted_paths[i]);
            struct stat st;
            int dir_exists = (stat(ready_path, &st) == 0 && S_ISDIR(st.st_mode));
            /* Count files in ready_path */
            int file_count = 0;
            if (dir_exists) {
                file_count = scanner_scan_directory(ready_path);
                if (file_count < 0) file_count = 0;
            }
            if (i > 0) json[buf_used++] = ',';
            buf_used += snprintf(json + buf_used, buf_size - buf_used,
                "{\"path\":\"%s\",\"ready_media_path\":\"%s\","
                "\"ready_media_exists\":%s,\"files_found\":%d}",
                server_config.converted_paths[i], ready_path,
                dir_exists ? "true" : "false", file_count);
        }
        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "],\"unprocessed_path_count\":%d,\"unprocessed_paths\":[",
            server_config.unprocessed_path_count);
        for (int i = 0; i < server_config.unprocessed_path_count; i++) {
            struct stat st;
            int exists = (stat(server_config.unprocessed_paths[i], &st) == 0);
            if (i > 0) json[buf_used++] = ',';
            buf_used += snprintf(json + buf_used, buf_size - buf_used,
                "{\"path\":\"%s\",\"exists\":%s}",
                server_config.unprocessed_paths[i], exists ? "true" : "false");
        }
        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "],\"db_path\":\"%s\",\"total_media\":%d}",
            server_config.db_path, database_get_count());
        send_response(fd, 200, "OK", "application/json", json, buf_used);
        free(json);
    }
    else if (strcmp(path, "/api/tmdb/refresh") == 0) {
        /* Fetch missing TMDB metadata */
        printf("API: Fetching missing TMDB metadata...\n");
        scanner_fetch_missing_tmdb();
        send_response(fd, 200, "OK", "application/json",
                     "{\"status\": \"tmdb refresh complete\"}", 35);
    }
    else if (strcmp(path, "/api/transcode/status") == 0) {
        const char *state_str = "idle";
        TranscodeState ts = transcoder_get_state();
        if (ts == TRANSCODE_RUNNING) state_str = "running";
        else if (ts == TRANSCODE_STOPPED) state_str = "stopped";

        char json[512];
        int len = snprintf(json, sizeof(json),
            "{\"state\":\"%s\",\"current\":\"%s\",\"completed\":%d,\"total\":%d,\"percent\":%.1f}",
            state_str,
            transcoder_get_current_file(),
            transcoder_get_completed_jobs(),
            transcoder_get_total_jobs(),
            transcoder_get_progress());
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else if (strcmp(path, "/api/transcode/start") == 0) {
        if (transcoder_get_state() == TRANSCODE_RUNNING) {
            send_response(fd, 200, "OK", "application/json",
                         "{\"status\":\"already running\"}", 28);
        } else {
            transcoder_start();
            send_response(fd, 200, "OK", "application/json",
                         "{\"status\":\"started\"}", 20);
        }
    }
    else if (strcmp(path, "/api/transcode/stop") == 0) {
        transcoder_stop();
        send_response(fd, 200, "OK", "application/json",
                     "{\"status\":\"stopped\"}", 20);
    }
    else if (strcmp(path, "/api/transcode/queue") == 0) {
        char *json = transcoder_get_queue_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Queue error");
        }
    }
    else if (strncmp(path, "/api/transcode/skip?title=", 26) == 0) {
        char title[256];
        url_decode(path + 26, title, sizeof(title));
        transcoder_skip_title(title);
        send_response(fd, 200, "OK", "application/json",
                     "{\"status\":\"skipped\"}", 20);
    }
    else if (strncmp(path, "/api/transcode/unskip?title=", 28) == 0) {
        char title[256];
        url_decode(path + 28, title, sizeof(title));
        transcoder_unskip_title(title);
        send_response(fd, 200, "OK", "application/json",
                     "{\"status\":\"unskipped\"}", 22);
    }
    else if (strncmp(path, "/api/transcode/prioritize?title=", 32) == 0) {
        char title[256];
        url_decode(path + 32, title, sizeof(title));
        transcoder_prioritize_title(title);
        send_response(fd, 200, "OK", "application/json",
                     "{\"status\":\"prioritized\"}", 24);
    }
    else if (strncmp(path, "/api/media/", 11) == 0) {
        /* Get media info by ID */
        int id = atoi(path + 11);
        char *json = database_get_media_json(id);
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 404, "Media not found");
        }
    }
    /* Retro gaming API endpoints */
    else if (strcmp(path, "/api/roms") == 0) {
        char *json = database_get_roms_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Database error");
        }
    }
    else if (strncmp(path, "/api/roms/console/", 18) == 0) {
        int console = atoi(path + 18);
        if (console >= 0 && console < CONSOLE_COUNT) {
            char *json = database_get_roms_by_console_json((ConsoleType)console);
            if (json) {
                send_response(fd, 200, "OK", "application/json", json, strlen(json));
                free(json);
            } else {
                send_error(fd, 500, "Database error");
            }
        } else {
            send_error(fd, 400, "Invalid console ID");
        }
    }
    else if (strncmp(path, "/api/rom/", 9) == 0) {
        int id = atoi(path + 9);
        char *json = database_get_rom_json(id);
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 404, "ROM not found");
        }
    }
    else if (strcmp(path, "/api/consoles") == 0) {
        char json[1024];
        int len = snprintf(json, sizeof(json),
            "[{\"id\":0,\"name\":\"NES\",\"count\":%d},"
            "{\"id\":1,\"name\":\"SNES\",\"count\":%d},"
            "{\"id\":2,\"name\":\"Nintendo 64\",\"count\":%d},"
            "{\"id\":3,\"name\":\"GameCube\",\"count\":%d},"
            "{\"id\":4,\"name\":\"Wii\",\"count\":%d},"
            "{\"id\":5,\"name\":\"Switch\",\"count\":%d}]",
            database_get_rom_count_by_console(CONSOLE_NES),
            database_get_rom_count_by_console(CONSOLE_SNES),
            database_get_rom_count_by_console(CONSOLE_N64),
            database_get_rom_count_by_console(CONSOLE_GAMECUBE),
            database_get_rom_count_by_console(CONSOLE_WII),
            database_get_rom_count_by_console(CONSOLE_SWITCH));
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else if (strcmp(path, "/api/counts") == 0) {
        char json[512];
        int len = snprintf(json, sizeof(json),
            "{\"movies\":%d,\"tvshows\":%d,\"roms\":%d,\"total\":%d}",
            0, 0, database_get_rom_count(), database_get_count());
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else {
        send_error(fd, 404, "API endpoint not found");
    }
}

/* Parse HTTP request and handle it */
static void handle_request(int fd, const char *request) {
    char method[16], path[MAX_PATH], version[16];

    if (sscanf(request, "%15s %4095s %15s", method, path, version) != 3) {
        send_error(fd, 400, "Bad Request");
        return;
    }

    /* Only support GET requests */
    if (strcmp(method, "GET") != 0) {
        send_error(fd, 405, "Method Not Allowed");
        return;
    }

    /* Extract Range header if present */
    char *range_header = NULL;
    char *range_start = strstr(request, "Range:");
    if (range_start) {
        range_start += 6;
        while (*range_start == ' ') range_start++;
        char *range_end = strstr(range_start, "\r\n");
        if (range_end) {
            size_t len = range_end - range_start;
            range_header = malloc(len + 1);
            strncpy(range_header, range_start, len);
            range_header[len] = '\0';
        }
    }

    /* Route request */
    if (strncmp(path, "/api/", 5) == 0) {
        handle_api(fd, path);
    }
    else if (strncmp(path, "/stream/", 8) == 0) {
        /* Stream media file by ID */
        int id = atoi(path + 8);
        char *filepath = database_get_filepath(id);
        if (filepath) {
            stream_file(fd, filepath, range_header);
            free(filepath);
        } else {
            send_error(fd, 404, "Media not found");
        }
    }
    else if (strncmp(path, "/image/", 7) == 0) {
        /* Serve cached image */
        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s",
                 server_config.cache_dir, path + 7);
        serve_file(fd, filepath);
    }
    else if (strcmp(path, "/status") == 0) {
        const char *html =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>Nixly Transcoder</title>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box}\n"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;\n"
"  background:#0d1117;color:#c9d1d9;min-height:100vh;padding:20px}\n"
"h1{color:#58a6ff;margin-bottom:8px;font-size:1.5em}\n"
".status-bar{background:#161b22;border:1px solid #30363d;border-radius:8px;\n"
"  padding:16px;margin-bottom:20px}\n"
".status-bar .state{font-size:1.1em;font-weight:600}\n"
".state.running{color:#3fb950}.state.idle{color:#8b949e}.state.stopped{color:#f85149}\n"
".progress-wrap{background:#21262d;border-radius:4px;height:24px;margin:10px 0;\n"
"  overflow:hidden;position:relative}\n"
".progress-bar{background:linear-gradient(90deg,#1f6feb,#58a6ff);height:100%;\n"
"  transition:width .5s ease;border-radius:4px}\n"
".progress-text{position:absolute;top:0;left:0;right:0;text-align:center;\n"
"  line-height:24px;font-size:12px;color:#fff;font-weight:600}\n"
".current{color:#8b949e;font-size:0.9em;margin-top:4px}\n"
".controls{margin:12px 0;display:flex;gap:8px;flex-wrap:wrap}\n"
".btn{padding:8px 16px;border:1px solid #30363d;border-radius:6px;\n"
"  background:#21262d;color:#c9d1d9;cursor:pointer;font-size:13px;\n"
"  transition:background .15s}\n"
".btn:hover{background:#30363d}\n"
".btn.green{border-color:#238636;color:#3fb950}\n"
".btn.green:hover{background:#238636;color:#fff}\n"
".btn.red{border-color:#da3633;color:#f85149}\n"
".btn.red:hover{background:#da3633;color:#fff}\n"
".btn.blue{border-color:#1f6feb;color:#58a6ff}\n"
".btn.blue:hover{background:#1f6feb;color:#fff}\n"
".queue{list-style:none}\n"
".queue li{display:flex;align-items:center;gap:10px;padding:10px 12px;\n"
"  background:#161b22;border:1px solid #30363d;border-radius:6px;\n"
"  margin-bottom:6px;transition:opacity .2s}\n"
".queue li.skipped{opacity:.4;text-decoration:line-through}\n"
".queue li.active{border-color:#1f6feb;background:#161b22}\n"
".queue li .info{flex:1;min-width:0}\n"
".queue li .title{font-weight:600;color:#c9d1d9;white-space:nowrap;\n"
"  overflow:hidden;text-overflow:ellipsis}\n"
".queue li .meta{font-size:0.8em;color:#8b949e}\n"
".queue li .actions{display:flex;gap:6px;flex-shrink:0}\n"
".queue li .actions .btn{padding:4px 10px;font-size:12px}\n"
".tag{display:inline-block;padding:2px 6px;border-radius:3px;font-size:11px;\n"
"  font-weight:600;margin-right:4px}\n"
".tag.movie{background:#1f3d1f;color:#3fb950}\n"
".tag.tv{background:#1f2d4f;color:#58a6ff}\n"
".counter{color:#8b949e;font-size:0.9em;margin-bottom:10px}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<h1>Nixly Transcoder</h1>\n"
"\n"
"<div class=\"status-bar\">\n"
"  <div><span class=\"state\" id=\"state\">Loading...</span></div>\n"
"  <div class=\"progress-wrap\"><div class=\"progress-bar\" id=\"pbar\"></div>\n"
"    <div class=\"progress-text\" id=\"ptxt\"></div></div>\n"
"  <div class=\"current\" id=\"current\"></div>\n"
"  <div class=\"controls\">\n"
"    <button class=\"btn green\" onclick=\"apiCall('/api/transcode/start')\">Start</button>\n"
"    <button class=\"btn red\" onclick=\"apiCall('/api/transcode/stop')\">Stop</button>\n"
"  </div>\n"
"</div>\n"
"\n"
"<div class=\"counter\" id=\"counter\"></div>\n"
"<ul class=\"queue\" id=\"queue\"></ul>\n"
"\n"
"<script>\n"
"function apiCall(url){fetch(url).then(()=>setTimeout(refresh,300))}\n"
"function esc(s){var d=document.createElement('div');d.textContent=s;\n"
"  return d.innerHTML.replace(/\"/g,'&quot;')}\n"
"\n"
"document.getElementById('queue').addEventListener('click',function(e){\n"
"  var btn=e.target.closest('button[data-action]');\n"
"  if(!btn)return;\n"
"  var t=btn.getAttribute('data-title');\n"
"  var a=btn.getAttribute('data-action');\n"
"  apiCall('/api/transcode/'+a+'?title='+encodeURIComponent(t));\n"
"});\n"
"\n"
"function refresh(){\n"
"  fetch('/api/transcode/status').then(function(r){return r.json()}).then(function(d){\n"
"    var s=document.getElementById('state');\n"
"    s.textContent=d.state.charAt(0).toUpperCase()+d.state.slice(1);\n"
"    s.className='state '+d.state;\n"
"    document.getElementById('pbar').style.width=d.percent+'%';\n"
"    document.getElementById('ptxt').textContent=\n"
"      d.state==='running'?d.percent.toFixed(1)+'%':'';\n"
"    document.getElementById('current').textContent=\n"
"      d.current?'Encoding: '+d.current:'';\n"
"  });\n"
"  fetch('/api/transcode/queue').then(function(r){return r.json()}).then(function(jobs){\n"
"    var ul=document.getElementById('queue');\n"
"    document.getElementById('counter').textContent=jobs.length+' files in queue';\n"
"    ul.innerHTML='';\n"
"    jobs.forEach(function(j){\n"
"      var li=document.createElement('li');\n"
"      if(j.skipped)li.className='skipped';\n"
"      if(j.current)li.className='active';\n"
"      var type=j.type===2?'tv':'movie';\n"
"      var tag='<span class=\"tag '+type+'\">'+(j.type===2?'TV':'Movie')+'</span>';\n"
"      var meta=j.type===2?'S'+String(j.season).padStart(2,'0')+'E'+\n"
"        String(j.episode).padStart(2,'0'):'';if(j.year)meta+=' ('+j.year+')';\n"
"      var t=esc(j.title);\n"
"      var skipBtn=j.skipped?\n"
"        '<button class=\"btn\" data-action=\"unskip\" data-title=\"'+t+'\">Unskip</button>':\n"
"        '<button class=\"btn red\" data-action=\"skip\" data-title=\"'+t+'\">Skip</button>';\n"
"      li.innerHTML='<div class=\"info\"><div class=\"title\">'+tag+' '+t+\n"
"        '</div><div class=\"meta\">'+meta+'</div></div>'+\n"
"        '<div class=\"actions\">'+skipBtn+\n"
"        '<button class=\"btn blue\" data-action=\"prioritize\" data-title=\"'+t+\n"
"        '\">Prioritize!</button></div>';\n"
"      ul.appendChild(li);\n"
"    });\n"
"  });\n"
"}\n"
"refresh();setInterval(refresh,3000);\n"
"</script>\n"
"</body></html>\n";
        send_response(fd, 200, "OK", "text/html", html, strlen(html));
    }
    else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        const char *html =
            "<!DOCTYPE html><html><head><title>Nixly Media Server</title></head>"
            "<body><h1>Nixly Media Server</h1>"
            "<p>API Endpoints:</p>"
            "<ul>"
            "<li><a href='/api/library'>/api/library</a> - Full media library</li>"
            "<li><a href='/api/movies'>/api/movies</a> - Movies list</li>"
            "<li><a href='/api/tvshows'>/api/tvshows</a> - TV Shows list</li>"
            "<li>/api/media/{id} - Media details</li>"
            "<li>/api/scan - Rescan library</li>"
            "<li>/api/tmdb/refresh - Fetch missing TMDB data</li>"
            "<li>/stream/{id} - Stream media file</li>"
            "<li>/image/{filename} - Cached poster/backdrop</li>"
            "<li><a href='/status'>/status</a> - Transcoder status &amp; queue</li>"
            "</ul></body></html>";
        send_response(fd, 200, "OK", "text/html", html, strlen(html));
    }
    else {
        send_error(fd, 404, "Not Found");
    }

    if (range_header) free(range_header);
}

/* Optimize socket for high-throughput streaming */
static void optimize_socket(int fd) {
    /* TCP_NODELAY: disable Nagle's algorithm for lower latency */
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Large send buffer for streaming (4MB) */
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    /* TCP_CORK: batch small writes for efficiency (disabled for streaming) */
    int cork = 0;
    setsockopt(fd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
}

/* Client handler thread */
static void *client_handler(void *arg) {
    ClientConnection *conn = (ClientConnection *)arg;
    char buffer[MAX_HEADER];
    int is_stream = 0;

    /* Set thread-local flag for local client detection */
    config_set_client_local(is_local_client(&conn->client_addr));

    /* Optimize socket for streaming before handling request */
    optimize_socket(conn->client_fd);

    ssize_t bytes = recv(conn->client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';

        /* Check if this is a stream request (counts against bandwidth limit) */
        if (strstr(buffer, "/stream/") != NULL) {
            int max = get_max_streams();
            pthread_mutex_lock(&stream_lock);
            if (active_streams >= max) {
                pthread_mutex_unlock(&stream_lock);
                send_error(conn->client_fd, 503,
                    "Server busy - max concurrent streams reached");
                close(conn->client_fd);
                free(conn);
                return NULL;
            }
            active_streams++;
            is_stream = 1;
            pthread_mutex_unlock(&stream_lock);
        }

        handle_request(conn->client_fd, buffer);
    }

    /* Decrement stream count if this was a stream */
    if (is_stream) {
        pthread_mutex_lock(&stream_lock);
        active_streams--;
        pthread_mutex_unlock(&stream_lock);
    }

    close(conn->client_fd);
    free(conn);
    return NULL;
}

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
    if (server_fd >= 0) {
        close(server_fd);
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c <config>  Config file path (default: ~/.config/nixly-server/config.conf)\n");
    fprintf(stderr, "  -p <port>    Override port (default: %d)\n", server_config.port);
    fprintf(stderr, "  -h           Show this help\n");
}

int main(int argc, char *argv[]) {
    const char *config_path = "~/.config/nixly-server/config.conf";
    int port_override = 0;
    int opt;

    while ((opt = getopt(argc, argv, "c:p:h")) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            case 'p':
                port_override = atoi(optarg);
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    /* Load configuration */
    config_load(config_path);

    if (port_override > 0) {
        server_config.port = port_override;
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Create cache directory */
    char cache_expanded[MAX_PATH];
    const char *home = getenv("HOME");
    if (server_config.cache_dir[0] == '~' && home) {
        snprintf(cache_expanded, sizeof(cache_expanded), "%s%s",
                 home, server_config.cache_dir + 1);
        strcpy(server_config.cache_dir, cache_expanded);
    }
    mkdir(server_config.cache_dir, 0755);

    /* Expand db_path and create parent directory */
    if (server_config.db_path[0] == '~' && home) {
        char db_expanded[MAX_PATH];
        snprintf(db_expanded, sizeof(db_expanded), "%s%s",
                 home, server_config.db_path + 1);
        strcpy(server_config.db_path, db_expanded);
    }
    {
        char db_dir[MAX_PATH];
        strncpy(db_dir, server_config.db_path, sizeof(db_dir) - 1);
        db_dir[sizeof(db_dir) - 1] = '\0';
        char *slash = strrchr(db_dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir(db_dir, 0755);
        }
    }

    /* Initialize database */
    if (database_init(server_config.db_path) != 0) {
        fprintf(stderr, "Failed to initialize database\n");
        return 1;
    }

    /* Initialize TMDB */
    if (server_config.tmdb_api_key[0]) {
        if (tmdb_init(server_config.tmdb_api_key, server_config.tmdb_language) == 0) {
            printf("TMDB: Initialized with API key\n");
        }
    }

    /* Clean up entries for files that no longer exist */
    printf("Checking for removed files...\n");
    int removed = database_cleanup_missing();
    if (removed > 0) {
        printf("Removed %d entries for missing files\n", removed);
    }

    /* Initial scan of nixly_ready_media directories */
    printf("Scanning converted media directories...\n");
    for (int i = 0; i < server_config.converted_path_count; i++) {
        char ready_path[MAX_PATH];
        snprintf(ready_path, sizeof(ready_path), "%s/nixly_ready_media",
                 server_config.converted_paths[i]);
        printf("  Ready media: %s\n", ready_path);
        scanner_scan_directory(ready_path);
    }
    printf("Scan complete. Found %d media files.\n", database_get_count());

    /* Fetch TMDB metadata for any entries missing it */
    scanner_fetch_missing_tmdb();

    /* Refresh show status (next episode dates, ended status) */
    scanner_refresh_show_status();

    /* Initialize file watcher */
    if (watcher_init() == 0) {
        watcher_set_callback(on_file_change);

        /* Watch unprocessed paths for new source files -> trigger transcoder */
        for (int i = 0; i < server_config.unprocessed_path_count; i++) {
            watcher_add_path(server_config.unprocessed_paths[i], WATCH_TYPE_MEDIA);
        }

        /* Watch nixly_ready_media in converted paths -> scan to DB */
        for (int i = 0; i < server_config.converted_path_count; i++) {
            char ready_path[MAX_PATH];
            snprintf(ready_path, sizeof(ready_path), "%s/nixly_ready_media",
                     server_config.converted_paths[i]);
            /* Create the directory if it doesn't exist yet */
            mkdir(ready_path, 0755);
            watcher_add_path(ready_path, WATCH_TYPE_MEDIA);
        }

        /* ROMs directories */
        for (int i = 0; i < server_config.roms_path_count; i++) {
            watcher_add_path(server_config.roms_paths[i], WATCH_TYPE_ROMS);
        }

        watcher_start();
    }

    /* Initialize and start transcoder */
    transcoder_init();
    transcoder_start();

    /* Create server socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(server_config.port),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    /* SOMAXCONN = kernel max (typically 4096+), allows unlimited concurrent streams */
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        return 1;
    }

    /* Start discovery thread */
    pthread_t discovery_tid;
    if (pthread_create(&discovery_tid, NULL, discovery_thread, NULL) != 0) {
        fprintf(stderr, "Warning: Failed to start discovery thread\n");
    } else {
        pthread_detach(discovery_tid);
    }

    /* Start periodic sync thread (backup to inotify) */
    pthread_t sync_tid;
    if (pthread_create(&sync_tid, NULL, sync_thread, NULL) != 0) {
        fprintf(stderr, "Warning: Failed to start sync thread\n");
    } else {
        pthread_detach(sync_tid);
    }

    int max_streams = get_max_streams();
    int rating = config_get_server_rating();
    printf("\n");
    printf("========================================\n");
    printf("  Nixly Media Server running\n");
    printf("  http://0.0.0.0:%d\n", server_config.port);
    printf("  Discovery: UDP port %d\n", DISCOVERY_PORT);
    printf("========================================\n");
    printf("  Server: %s\n", server_config.server_name);
    printf("  ID: %s\n", server_config.server_id);
    printf("  Rating: %d/10 (%d Mbps)\n", rating, server_config.upload_mbps);
    printf("  Max streams: %d\n", max_streams);
    printf("========================================\n");
    printf("  Media files: %d\n", database_get_count());
    printf("  Watching %d directories\n", watcher_get_count());
    printf("========================================\n\n");

    /* Accept connections */
    while (running) {
        ClientConnection *conn = malloc(sizeof(ClientConnection));
        socklen_t addr_len = sizeof(conn->client_addr);

        conn->client_fd = accept(server_fd, (struct sockaddr *)&conn->client_addr, &addr_len);
        if (conn->client_fd < 0) {
            free(conn);
            if (running) perror("accept");
            continue;
        }

        pthread_t thread;
        if (pthread_create(&thread, NULL, client_handler, conn) != 0) {
            close(conn->client_fd);
            free(conn);
            continue;
        }
        pthread_detach(thread);
    }

    /* Cleanup */
    transcoder_cleanup();
    if (discovery_fd >= 0) {
        close(discovery_fd);
        discovery_fd = -1;
    }
    watcher_cleanup();
    tmdb_cleanup();
    database_close();
    printf("\nServer stopped.\n");
    return 0;
}
