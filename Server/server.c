/*
 * Nixly Media Server
 * Lossless streaming server for movies and TV shows
 *
 * - Serves media files without transcoding (full quality)
 * - SQLite database for media library
 * - HTTP server with range request support for seeking
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

#define DEFAULT_PORT 8080
#define MAX_CONNECTIONS 100
#define BUFFER_SIZE 65536
#define MAX_PATH 4096
#define MAX_HEADER 8192

static int server_fd = -1;
static volatile int running = 1;

/* Media library paths */
static char *media_paths[64];
static int media_path_count = 0;

typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
} ClientConnection;

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

    return "application/octet-stream";
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
    if (strcmp(path, "/api/library") == 0) {
        /* Return entire media library as JSON */
        char *json = database_get_library_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Database error");
        }
    }
    else if (strncmp(path, "/api/movies", 11) == 0) {
        char *json = database_get_movies_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Database error");
        }
    }
    else if (strncmp(path, "/api/tvshows", 12) == 0) {
        char *json = database_get_tvshows_json();
        if (json) {
            send_response(fd, 200, "OK", "application/json", json, strlen(json));
            free(json);
        } else {
            send_error(fd, 500, "Database error");
        }
    }
    else if (strncmp(path, "/api/scan", 9) == 0) {
        /* Trigger library rescan */
        for (int i = 0; i < media_path_count; i++) {
            scanner_scan_directory(media_paths[i]);
        }
        send_response(fd, 200, "OK", "application/json", "{\"status\": \"scan started\"}", 26);
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
            "<li>/stream/{id} - Stream media file</li>"
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
    fprintf(stderr, "Usage: %s [options] <media_path> [media_path2 ...]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -p <port>    Port to listen on (default: %d)\n", DEFAULT_PORT);
    fprintf(stderr, "  -d <db>      Database file path (default: nixly.db)\n");
    fprintf(stderr, "  -h           Show this help\n");
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    const char *db_path = "nixly.db";
    int opt;

    while ((opt = getopt(argc, argv, "p:d:h")) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'd':
                db_path = optarg;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }

    /* Collect media paths */
    for (int i = optind; i < argc && media_path_count < 64; i++) {
        media_paths[media_path_count++] = argv[i];
    }

    if (media_path_count == 0) {
        fprintf(stderr, "Error: At least one media path required\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Initialize database */
    if (database_init(db_path) != 0) {
        fprintf(stderr, "Failed to initialize database\n");
        return 1;
    }

    /* Initial scan of media directories */
    printf("Scanning media directories...\n");
    for (int i = 0; i < media_path_count; i++) {
        printf("  Scanning: %s\n", media_paths[i]);
        scanner_scan_directory(media_paths[i]);
    }
    printf("Scan complete. Found %d media files.\n", database_get_count());

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
        .sin_port = htons(port),
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

    printf("Nixly Media Server running on http://0.0.0.0:%d\n", port);
    printf("Serving %d media paths\n", media_path_count);

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

    database_close();
    printf("\nServer stopped.\n");
    return 0;
}
