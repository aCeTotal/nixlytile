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
#include <netinet/in.h>
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

#define MAX_CONNECTIONS 100
#define BUFFER_SIZE 65536
#define MAX_PATH 4096
#define MAX_HEADER 8192
#define DISCOVERY_PORT 8081
#define DISCOVERY_MAGIC "NIXLY_DISCOVER"
#define DISCOVERY_RESPONSE "NIXLY_SERVER"

static int server_fd = -1;
static int discovery_fd = -1;
static volatile int running = 1;
static volatile int transcoder_restart_pending = 0;

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

        /* Scan for new files */
        int before = database_get_count();
        for (int i = 0; i < server_config.media_path_count; i++) {
            scanner_scan_directory(server_config.media_paths[i]);
        }
        for (int i = 0; i < server_config.movies_path_count; i++) {
            scanner_scan_directory(server_config.movies_paths[i]);
        }
        for (int i = 0; i < server_config.tvshows_path_count; i++) {
            scanner_scan_directory(server_config.tvshows_paths[i]);
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
} ClientConnection;

/* File change callback - keeps database in sync and triggers transcoding */
static void on_file_change(const char *filepath, int is_delete, WatchType type) {
    (void)type; /* We handle all types the same for now */

    if (is_delete) {
        /* Remove from database */
        if (database_delete_by_path(filepath) == 0) {
            printf("DB: Removed %s\n", filepath);
        }
    } else {
        /* Check if it's a media file that needs processing */
        if (scanner_is_media_file(filepath)) {
            /* Skip already converted files */
            const char *base = strrchr(filepath, '/');
            base = base ? base + 1 : filepath;
            if (strstr(base, ".x265.CRF14") != NULL) {
                /* This is an output file from transcoder, add to DB */
                int id = scanner_scan_file(filepath, 1);
                if (id > 0) {
                    printf("DB: Added converted file %s (id=%d)\n", filepath, id);
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

/* Stream file with range request support (for seeking in video) */
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
            "Connection: close\r\n"
            "\r\n",
            mime, content_length, start, end, st.st_size);
    } else {
        header_len = snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %ld\r\n"
            "Accept-Ranges: bytes\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n"
            "\r\n",
            mime, st.st_size);
    }

    write(fd, header, header_len);

    /* Seek to start position */
    if (start > 0) {
        lseek(file_fd, start, SEEK_SET);
    }

    /* Stream the file - no transcoding, pure lossless transfer */
    char buffer[BUFFER_SIZE];
    off_t remaining = content_length;

    while (remaining > 0 && running) {
        size_t to_read = (remaining < BUFFER_SIZE) ? remaining : BUFFER_SIZE;
        ssize_t bytes_read = read(file_fd, buffer, to_read);

        if (bytes_read <= 0) break;

        ssize_t bytes_written = write(fd, buffer, bytes_read);
        if (bytes_written <= 0) break;

        remaining -= bytes_read;
    }

    close(file_fd);
}

/* Handle API requests */
static void handle_api(int fd, const char *path) {
    if (strcmp(path, "/api/status") == 0) {
        /* Simple status check for discovery */
        send_response(fd, 200, "OK", "application/json",
                     "{\"status\": \"ok\", \"server\": \"nixly\"}", 35);
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
        /* Trigger library rescan */
        printf("API: Starting rescan...\n");
        for (int i = 0; i < server_config.media_path_count; i++) {
            scanner_scan_directory(server_config.media_paths[i]);
        }
        for (int i = 0; i < server_config.movies_path_count; i++) {
            scanner_scan_directory(server_config.movies_paths[i]);
        }
        for (int i = 0; i < server_config.tvshows_path_count; i++) {
            scanner_scan_directory(server_config.tvshows_paths[i]);
        }
        send_response(fd, 200, "OK", "application/json",
                     "{\"status\": \"scan complete\"}", 27);
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
    else if (strcmp(path, "/api/status") == 0) {
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
            "</ul></body></html>";
        send_response(fd, 200, "OK", "text/html", html, strlen(html));
    }
    else {
        send_error(fd, 404, "Not Found");
    }

    if (range_header) free(range_header);
}

/* Client handler thread */
static void *client_handler(void *arg) {
    ClientConnection *conn = (ClientConnection *)arg;
    char buffer[MAX_HEADER];

    ssize_t bytes = recv(conn->client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes > 0) {
        buffer[bytes] = '\0';
        handle_request(conn->client_fd, buffer);
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

    /* Expand output path */
    if (server_config.output_path[0] == '~' && home) {
        char out_expanded[MAX_PATH];
        snprintf(out_expanded, sizeof(out_expanded), "%s%s",
                 home, server_config.output_path + 1);
        strcpy(server_config.output_path, out_expanded);
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

    /* Initial scan of media directories */
    printf("Scanning media directories...\n");
    for (int i = 0; i < server_config.media_path_count; i++) {
        printf("  Media: %s\n", server_config.media_paths[i]);
        scanner_scan_directory(server_config.media_paths[i]);
    }
    for (int i = 0; i < server_config.movies_path_count; i++) {
        printf("  Movies: %s\n", server_config.movies_paths[i]);
        scanner_scan_directory(server_config.movies_paths[i]);
    }
    for (int i = 0; i < server_config.tvshows_path_count; i++) {
        printf("  TV Shows: %s\n", server_config.tvshows_paths[i]);
        scanner_scan_directory(server_config.tvshows_paths[i]);
    }
    printf("Scan complete. Found %d media files.\n", database_get_count());

    /* Fetch TMDB metadata for any entries missing it */
    scanner_fetch_missing_tmdb();

    /* Refresh show status (next episode dates, ended status) */
    scanner_refresh_show_status();

    /* Initialize file watcher */
    if (watcher_init() == 0) {
        watcher_set_callback(on_file_change);

        /* Generic media paths (watches everything) */
        for (int i = 0; i < server_config.media_path_count; i++) {
            watcher_add_path(server_config.media_paths[i], WATCH_TYPE_MEDIA);
        }

        /* Specific type paths (backwards compatibility) */
        for (int i = 0; i < server_config.movies_path_count; i++) {
            watcher_add_path(server_config.movies_paths[i], WATCH_TYPE_MOVIES);
        }
        for (int i = 0; i < server_config.tvshows_path_count; i++) {
            watcher_add_path(server_config.tvshows_paths[i], WATCH_TYPE_TVSHOWS);
        }
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

    if (listen(server_fd, MAX_CONNECTIONS) < 0) {
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

    printf("\n");
    printf("========================================\n");
    printf("  Nixly Media Server running\n");
    printf("  http://0.0.0.0:%d\n", server_config.port);
    printf("  Discovery: UDP port %d\n", DISCOVERY_PORT);
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
