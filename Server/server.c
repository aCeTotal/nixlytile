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
#include <dirent.h>
#include <errno.h>
#include <signal.h>

#include "database.h"
#include "scanner.h"
#include "config.h"
#include "tmdb.h"
#include <sys/syscall.h>
#include "watcher.h"
#include "downloads.h"

/* No connection limit - kernel handles backlog */
#define BUFFER_SIZE 1048576  /* 1MB for efficient streaming */
#define STREAM_CHUNK_SIZE 4194304  /* 4MB sendfile chunks — fewer syscalls for large UHD files */
#define MAX_PATH 4096
#define MAX_HEADER 8192
#define DISCOVERY_PORT 8081
#define DISCOVERY_MAGIC "NIXLY_DISCOVER"
#define DISCOVERY_RESPONSE "NIXLY_SERVER"

static int server_fd = -1;
static int discovery_fd = -1;
static volatile int running = 1;
static volatile int startup_scanning = 0;  /* 1 while initial scan/TMDB fetch is running */

/* Connection limiting based on bandwidth */
#define STREAM_BITRATE_MBPS 70  /* Assumed bitrate per stream for lossless 4K */
static volatile int active_streams = 0;
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;


static int get_max_streams(void) {
    if (server_config.upload_mbps <= 0) return 1000;  /* Effectively unlimited */
    return server_config.upload_mbps / STREAM_BITRATE_MBPS;
}

/* Forward declaration - defined after startup_scan_thread */
static void on_file_change(const char *filepath, int is_delete, WatchType type);

/* Background thread for /api/tmdb/rescan — non-blocking endpoint */
static void *tmdb_rescan_thread(void *arg) {
    (void)arg;
    scanner_scrape_session_begin(&tmdb_progress);
    scanner_rescan_all_tmdb();
    scanner_scrape_session_end(&tmdb_progress);
    printf("API: Background TMDB rescan complete\n");
    return NULL;
}

static void *scrape_tmdb_thread(void *arg) {
    (void)arg;
    printf("Scrape: TMDB thread started\n");
    scanner_fetch_missing_tmdb();
    scanner_refresh_show_status();
    printf("Scrape: TMDB thread done\n");
    return NULL;
}

/* Initial startup scan thread - runs heavy I/O and network work in background
 * so the HTTP server can accept connections immediately */
static void *startup_scan_thread(void *arg) {
    (void)arg;
    startup_scanning = 1;

    /* Clean up entries for files that no longer exist */
    printf("Startup scan: Checking for removed files...\n");
    int removed = database_cleanup_missing();
    if (removed > 0) {
        printf("Startup scan: Removed %d entries for missing files\n", removed);
    }

    /* Initial scan of nixly_ready_media directories */
    printf("Startup scan: Scanning converted media directories...\n");
    for (int i = 0; i < server_config.converted_path_count && running; i++) {
        char ready_path[MAX_PATH];
        snprintf(ready_path, sizeof(ready_path), "%s/nixly_ready_media",
                 server_config.converted_paths[i]);
        printf("  Ready media: %s\n", ready_path);
        scanner_scan_directory(ready_path);
    }
    printf("Startup scan: Found %d media files.\n", database_get_count());

    /* Initialize file watcher early — so unprocessed and ready media changes
     * are detected even while scraping is still running */
    if (running && watcher_init() == 0) {
        watcher_set_callback(on_file_change);

        for (int i = 0; i < server_config.unprocessed_path_count; i++) {
            watcher_add_path(server_config.unprocessed_paths[i], WATCH_TYPE_DOWNLOADS);
        }

        for (int i = 0; i < server_config.converted_path_count; i++) {
            char ready_path[MAX_PATH];
            snprintf(ready_path, sizeof(ready_path), "%s/nixly_ready_media",
                     server_config.converted_paths[i]);
            mkdir(ready_path, 0755);
            watcher_add_path(ready_path, WATCH_TYPE_MEDIA);
        }

        watcher_start();
    }

    /* Start unprocessed file monitor — scrape, rename, move */
    if (running && downloads_init() == 0) {
        downloads_process_pending();
        downloads_start();
    }

    scanner_scrape_session_begin(&tmdb_progress);

    {
        pthread_t tmdb_thread;
        int have_tmdb = 0;

        if (running) {
            have_tmdb = (pthread_create(&tmdb_thread, NULL, scrape_tmdb_thread, NULL) == 0);
        }

        if (!have_tmdb && running) {
            scanner_fetch_missing_tmdb();
            scanner_refresh_show_status();
        }

        if (have_tmdb) pthread_join(tmdb_thread, NULL);
    }

    scanner_scrape_session_end(&tmdb_progress);

    startup_scanning = 0;
    printf("Startup scan: Complete. Server fully ready.\n");
    printf("  Media files: %d\n", database_get_count());
    printf("  Watching %d directories\n", watcher_get_count());
    return NULL;
}

/* Periodic sync thread - checks for changes every 5 minutes as backup to inotify */
static void *sync_thread(void *arg) {
    (void)arg;

    while (running) {
        for (int i = 0; i < 300 && running; i++) sleep(1);
        if (!running) break;

        printf("Periodic sync: Checking for changes...\n");

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

        scanner_scrape_session_begin(&tmdb_progress);

        {
            pthread_t tmdb_t;
            int have_tmdb = (pthread_create(&tmdb_t, NULL, scrape_tmdb_thread, NULL) == 0);
            if (!have_tmdb) {
                scanner_fetch_missing_tmdb();
                scanner_refresh_show_status();
            }
            if (have_tmdb) pthread_join(tmdb_t, NULL);
        }

        scanner_scrape_session_end(&tmdb_progress);

        /* Process pending unprocessed files (backup to inotify thread) */
        downloads_process_pending();
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
    } else if (type == WATCH_TYPE_DOWNLOADS) {
        /* Unprocessed file — downloads.c handles classify+move */
        downloads_notify_file(filepath);
    } else {
        /* Ready media under nixly_ready_media/ — add to DB, scrape TMDB */
        if (scanner_is_media_file(filepath) &&
            strstr(filepath, "/nixly_ready_media/") != NULL) {
            int id = scanner_scan_file(filepath, 1);
            if (id > 0) {
                printf("DB: Added ready media %s (id=%d)\n", filepath, id);
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

    /* Aggressive sequential readahead — tells kernel to prefetch ~2MB+ ahead
     * instead of default ~128KB.  Huge win for streaming large files from
     * busy HDDs where seek latency dominates. */
    posix_fadvise(file_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    /* Elevate this thread's I/O scheduling priority to best-effort class,
     * highest priority (0).  Transcoding threads run at IOPRIO_CLASS_IDLE,
     * so direct streaming always wins disk access. */
    syscall(SYS_ioprio_set, 1 /*IOPRIO_WHO_PROCESS*/, 0 /*self*/,
            (2 << 13) | 0 /*IOPRIO_CLASS_BE, prio 0*/);

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
            "\"max_streams\":%d,"
            "\"scanning\":%s}",
            server_config.server_id,
            server_config.server_name,
            rating,
            is_local ? "true" : "false",
            priority,
            server_config.upload_mbps,
            active_streams, max,
            startup_scanning ? "true" : "false");
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
    else if (strcmp(path, "/api/tmdb/rescan") == 0) {
        /* Re-fetch TMDB metadata for ALL entries — run in background thread */
        printf("API: Full TMDB rescan starting (background)...\n");
        pthread_t rescan_thread;
        if (pthread_create(&rescan_thread, NULL, tmdb_rescan_thread, NULL) == 0) {
            pthread_detach(rescan_thread);
            send_response(fd, 200, "OK", "application/json",
                         "{\"status\":\"started\"}", 20);
        } else {
            send_response(fd, 500, "Internal Server Error", "application/json",
                         "{\"status\":\"failed to start rescan\"}", 35);
        }
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
    else if (strcmp(path, "/api/scrape/status") == 0) {
        int t_active, t_total, t_processed, t_success;
        const char *t_operation;
        char t_item[256];
        time_t t_start;
        scanner_get_progress(&tmdb_progress, &t_active, &t_operation, t_item,
            sizeof(t_item), &t_total, &t_processed, &t_success, &t_start);

        char t_esc[512];
        char *d = t_esc;
        for (const char *s = t_item; *s && d < t_esc + sizeof(t_esc) - 6; s++) {
            if (*s == '"') { *d++ = '\\'; *d++ = '"'; }
            else if (*s == '\\') { *d++ = '\\'; *d++ = '\\'; }
            else *d++ = *s;
        }
        *d = '\0';

        int t_elapsed = t_active ? (int)(time(NULL) - t_start) : 0;
        float t_percent = (t_total > 0) ? (100.0f * t_processed / t_total) : 0;
        if (t_active && t_percent >= 100.0f) t_percent = 99.0f;
        int t_failed = t_processed - t_success; if (t_failed < 0) t_failed = 0;

        int media_count = database_get_count();

        MediaEntry *tmdb_entries = NULL;
        int tmdb_pending = 0;
        if (database_get_entries_without_tmdb(&tmdb_entries, &tmdb_pending) == 0) {
            for (int i = 0; i < tmdb_pending; i++)
                database_free_entry(&tmdb_entries[i]);
            free(tmdb_entries);
        }

        char json[2048];
        int len = snprintf(json, sizeof(json),
            "{\"tmdb\":{\"active\":%s,\"operation\":\"%s\",\"current_item\":\"%s\","
            "\"total\":%d,\"processed\":%d,\"success\":%d,\"failed\":%d,"
            "\"percent\":%.1f,\"elapsed\":%d},"
            "\"media_count\":%d,\"tmdb_pending\":%d}",
            t_active ? "true" : "false", t_operation, t_esc,
            t_total, t_processed, t_success, t_failed, t_percent, t_elapsed,
            media_count, tmdb_pending);
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else if (strcmp(path, "/api/counts") == 0) {
        char json[256];
        int len = snprintf(json, sizeof(json),
            "{\"total\":%d}", database_get_count());
        send_response(fd, 200, "OK", "application/json", json, len);
    }
    else if (strcmp(path, "/api/debug/ls") == 0) {
        /* Diagnostic: recursively list files in nixly_ready_media without probing */
        size_t buf_size = 65536;
        size_t buf_used = 0;
        char *json = malloc(buf_size);
        buf_used += snprintf(json + buf_used, buf_size - buf_used, "{\"paths\":[");

        for (int i = 0; i < server_config.converted_path_count; i++) {
            char ready_path[MAX_PATH];
            snprintf(ready_path, sizeof(ready_path), "%s/nixly_ready_media",
                     server_config.converted_paths[i]);

            /* Recursive listing using a simple stack */
            char dirs[64][MAX_PATH];
            int dir_count = 0;
            strncpy(dirs[0], ready_path, MAX_PATH - 1);
            dir_count = 1;
            int first_file = 1;

            while (dir_count > 0) {
                dir_count--;
                char *cur_dir = dirs[dir_count];
                DIR *d = opendir(cur_dir);
                if (!d) {
                    if (buf_used < buf_size - 256) {
                        if (!first_file) json[buf_used++] = ',';
                        first_file = 0;
                        buf_used += snprintf(json + buf_used, buf_size - buf_used,
                            "{\"error\":\"opendir failed\",\"path\":\"%s\",\"errno\":%d,\"strerror\":\"%s\"}",
                            cur_dir, errno, strerror(errno));
                    }
                    continue;
                }
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    if (ent->d_name[0] == '.') continue;
                    char fullpath[MAX_PATH];
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", cur_dir, ent->d_name);
                    struct stat st;
                    int stat_ok = (stat(fullpath, &st) == 0);
                    if (stat_ok && S_ISDIR(st.st_mode) && dir_count < 63) {
                        strncpy(dirs[dir_count], fullpath, MAX_PATH - 1);
                        dir_count++;
                    } else if (buf_used < buf_size - 512) {
                        if (!first_file) json[buf_used++] = ',';
                        first_file = 0;
                        int is_media = scanner_is_media_file(fullpath);
                        buf_used += snprintf(json + buf_used, buf_size - buf_used,
                            "{\"path\":\"%s\",\"is_dir\":%s,\"is_media\":%s,\"size\":%lld,\"stat_ok\":%s}",
                            fullpath,
                            (stat_ok && S_ISDIR(st.st_mode)) ? "true" : "false",
                            is_media ? "true" : "false",
                            stat_ok ? (long long)st.st_size : 0LL,
                            stat_ok ? "true" : "false");
                    }
                }
                closedir(d);
            }
        }

        buf_used += snprintf(json + buf_used, buf_size - buf_used,
            "],\"converted_path_count\":%d,\"db_count\":%d}",
            server_config.converted_path_count, database_get_count());
        send_response(fd, 200, "OK", "application/json", json, buf_used);
        free(json);
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
        /* Serve cached image — URL-decode filename (e.g. %20 → space) */
        char decoded[MAX_PATH];
        url_decode(path + 7, decoded, sizeof(decoded));
        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s",
                 server_config.cache_dir, decoded);
        serve_file(fd, filepath);
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
            "<li>/api/tmdb/rescan - Re-fetch ALL TMDB data</li>"
            "<li>/api/scrape/status - Current scrape progress</li>"
            "<li>/stream/{id} - Stream media file</li>"
            "<li>/image/{filename} - Cached poster/backdrop</li>"
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

    /* Create server socket FIRST - accept connections while scanning in background */
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

    /* Start initial scan in background - server responds immediately with
     * whatever is already in the database while scan populates new entries */
    pthread_t startup_tid;
    if (pthread_create(&startup_tid, NULL, startup_scan_thread, NULL) != 0) {
        fprintf(stderr, "Warning: Failed to start startup scan thread, running synchronously\n");
        startup_scan_thread(NULL);
    } else {
        pthread_detach(startup_tid);
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
    printf("  Scanning in background...\n");
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
    downloads_stop();
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
