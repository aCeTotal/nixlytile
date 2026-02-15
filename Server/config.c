/*
 * Nixly Media Server - Configuration Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <pwd.h>
#include <unistd.h>
#include <time.h>
#include "config.h"

/* Generate a unique server ID based on hostname and random component */
static void generate_server_id(char *buf, size_t len) {
    char hostname[64] = "nixly";
    gethostname(hostname, sizeof(hostname) - 1);
    hostname[sizeof(hostname) - 1] = '\0';

    /* Simple random suffix */
    srand(time(NULL) ^ getpid());
    unsigned int r = rand();

    snprintf(buf, len, "%s-%08x", hostname, r);
}

ServerConfig server_config;

static char *trim(char *str) {
    while (isspace(*str)) str++;
    if (*str == '\0') return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) *end-- = '\0';
    return str;
}

static char *expand_path(const char *path, char *out, size_t out_size) {
    if (path[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "/tmp";
        }
        snprintf(out, out_size, "%s%s", home, path + 1);
    } else {
        strncpy(out, path, out_size - 1);
        out[out_size - 1] = '\0';
    }
    return out;
}

void config_init_defaults(void) {
    memset(&server_config, 0, sizeof(server_config));

    server_config.port = 8080;
    strcpy(server_config.db_path, "~/.local/share/nixly-server/nixly.db");

    /* Default: 500 Mbps upload = ~7 simultaneous 70 Mbps streams */
    server_config.upload_mbps = 500;

    /* Server identity - generate unique ID, use hostname as name */
    generate_server_id(server_config.server_id, sizeof(server_config.server_id));
    gethostname(server_config.server_name, sizeof(server_config.server_name) - 1);
    server_config.server_name[sizeof(server_config.server_name) - 1] = '\0';

    /* TMDB API key - hardcoded */
    strcpy(server_config.tmdb_api_key, "d415e076cfcbbe11dd7366a6e2f63321");
    strcpy(server_config.tmdb_language, "en-US");
    strcpy(server_config.cache_dir, "~/.cache/nixly-server");

    /* Unprocessed/converted paths empty by default */
    server_config.unprocessed_path_count = 0;
    server_config.converted_path_count = 0;

    /* Keep source files by default */
    server_config.delete_after_conversion = 0;
}

int config_load(const char *path) {
    char expanded[MAX_PATH_LEN];
    expand_path(path, expanded, sizeof(expanded));

    FILE *f = fopen(expanded, "r");
    if (!f) {
        fprintf(stderr, "Config file not found: %s, using defaults\n", expanded);
        config_init_defaults();
        return -1;
    }

    config_init_defaults();

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);

        /* Skip comments and empty lines */
        if (*l == '#' || *l == '\0') continue;

        char *eq = strchr(l, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = trim(l);
        char *value = trim(eq + 1);

        /* Remove quotes from value */
        size_t vlen = strlen(value);
        if (vlen >= 2 && value[0] == '"' && value[vlen - 1] == '"') {
            value[vlen - 1] = '\0';
            value++;
        }

        if (strcmp(key, "port") == 0) {
            server_config.port = atoi(value);
        }
        else if (strcmp(key, "upload_mbps") == 0) {
            server_config.upload_mbps = atoi(value);
        }
        else if (strcmp(key, "server_id") == 0) {
            strncpy(server_config.server_id, value, sizeof(server_config.server_id) - 1);
        }
        else if (strcmp(key, "server_name") == 0) {
            strncpy(server_config.server_name, value, sizeof(server_config.server_name) - 1);
        }
        else if (strcmp(key, "db_path") == 0) {
            expand_path(value, server_config.db_path, sizeof(server_config.db_path));
        }
        else if (strcmp(key, "tmdb_api_key") == 0) {
            strncpy(server_config.tmdb_api_key, value, sizeof(server_config.tmdb_api_key) - 1);
        }
        else if (strcmp(key, "tmdb_language") == 0) {
            strncpy(server_config.tmdb_language, value, sizeof(server_config.tmdb_language) - 1);
        }
        else if (strcmp(key, "cache_dir") == 0) {
            expand_path(value, server_config.cache_dir, sizeof(server_config.cache_dir));
        }
        else if (strcmp(key, "unprocessed_path") == 0) {
            if (server_config.unprocessed_path_count < MAX_WATCH_PATHS) {
                expand_path(value, server_config.unprocessed_paths[server_config.unprocessed_path_count],
                           MAX_PATH_LEN);
                server_config.unprocessed_path_count++;
            }
        }
        else if (strcmp(key, "converted_path") == 0) {
            if (server_config.converted_path_count < MAX_WATCH_PATHS) {
                expand_path(value, server_config.converted_paths[server_config.converted_path_count],
                           MAX_PATH_LEN);
                server_config.converted_path_count++;
            }
        }
        else if (strcmp(key, "delete_after_conversion") == 0) {
            server_config.delete_after_conversion =
                (strcmp(value, "true") == 0 || strcmp(value, "1") == 0);
        }
        else if (strcmp(key, "roms_path") == 0) {
            if (server_config.roms_path_count < MAX_WATCH_PATHS) {
                expand_path(value, server_config.roms_paths[server_config.roms_path_count],
                           MAX_PATH_LEN);
                server_config.roms_path_count++;
            }
        }
    }

    fclose(f);
    printf("Config loaded from %s\n", expanded);
    return 0;
}

int config_save(const char *path) {
    char expanded[MAX_PATH_LEN];
    expand_path(path, expanded, sizeof(expanded));

    FILE *f = fopen(expanded, "w");
    if (!f) return -1;

    fprintf(f, "# Nixly Media Server Configuration\n\n");

    fprintf(f, "# Server port\n");
    fprintf(f, "port = %d\n\n", server_config.port);

    fprintf(f, "# Database path\n");
    fprintf(f, "db_path = %s\n\n", server_config.db_path);

    fprintf(f, "# Upload speed in Mbps (max streams = upload_mbps / 70)\n");
    fprintf(f, "# Server rating = upload_mbps / 100 (1-10 scale)\n");
    fprintf(f, "upload_mbps = %d\n\n", server_config.upload_mbps);

    fprintf(f, "# Server identity (for multi-server deduplication)\n");
    fprintf(f, "server_id = %s\n", server_config.server_id);
    fprintf(f, "server_name = %s\n\n", server_config.server_name);

    fprintf(f, "# TMDB API key (get from https://www.themoviedb.org/settings/api)\n");
    fprintf(f, "tmdb_api_key = %s\n\n", server_config.tmdb_api_key);

    fprintf(f, "# TMDB language (en-US, nb-NO, etc.)\n");
    fprintf(f, "tmdb_language = %s\n\n", server_config.tmdb_language);

    fprintf(f, "# Cache directory for thumbnails\n");
    fprintf(f, "cache_dir = %s\n\n", server_config.cache_dir);

    fprintf(f, "# Source directories with raw media to be transcoded (can have multiple)\n");
    for (int i = 0; i < server_config.unprocessed_path_count; i++) {
        fprintf(f, "unprocessed_path = %s\n", server_config.unprocessed_paths[i]);
    }

    fprintf(f, "\n# Destination disks for converted media (can have multiple)\n");
    fprintf(f, "# Transcoded files go to <path>/nixly_ready_media/TV|Movies/\n");
    for (int i = 0; i < server_config.converted_path_count; i++) {
        fprintf(f, "converted_path = %s\n", server_config.converted_paths[i]);
    }

    fprintf(f, "\n# Delete source files after successful conversion (true/false)\n");
    fprintf(f, "delete_after_conversion = %s\n", server_config.delete_after_conversion ? "true" : "false");

    fprintf(f, "\n# ROMs directories (can have multiple)\n");
    for (int i = 0; i < server_config.roms_path_count; i++) {
        fprintf(f, "roms_path = %s\n", server_config.roms_paths[i]);
    }

    fclose(f);
    return 0;
}

/* Calculate server rating based on upload speed
 * 1000 Mbps or more = 10, 900 = 9, 800 = 8, ... 100 = 1
 * Used by clients to prefer servers with better bandwidth */
int config_get_server_rating(void) {
    int rating = server_config.upload_mbps / 100;
    if (rating < 1) rating = 1;
    if (rating > 10) rating = 10;
    return rating;
}

/* Thread-local client locality flag */
static __thread int current_client_local = 0;

void config_set_client_local(int is_local) {
    current_client_local = is_local;
}

int config_get_client_local(void) {
    return current_client_local;
}

/* Get priority: local servers are always preferred (1000+) over remote */
int config_get_server_priority(void) {
    int rating = config_get_server_rating();
    return current_client_local ? (1000 + rating) : rating;
}
