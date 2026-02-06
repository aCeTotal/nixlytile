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
#include "config.h"

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
    strcpy(server_config.db_path, "nixly.db");

    /* TMDB API key - hardcoded */
    strcpy(server_config.tmdb_api_key, "d415e076cfcbbe11dd7366a6e2f63321");
    strcpy(server_config.tmdb_language, "en-US");
    strcpy(server_config.cache_dir, "~/.cache/nixly-server");

    /* Transcoder output directory (empty = Nixly_Media per source path) */
    server_config.output_path[0] = '\0';

    /* Default: watch all of /home/total/media/ */
    strcpy(server_config.media_paths[0], "/home/total/media");
    server_config.media_path_count = 1;

    /* Movies/tvshows paths empty by default (use media_paths instead) */
    server_config.movies_path_count = 0;
    server_config.tvshows_path_count = 0;
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
        else if (strcmp(key, "output_path") == 0) {
            expand_path(value, server_config.output_path, sizeof(server_config.output_path));
        }
        else if (strcmp(key, "movies_path") == 0) {
            if (server_config.movies_path_count < MAX_WATCH_PATHS) {
                expand_path(value, server_config.movies_paths[server_config.movies_path_count],
                           MAX_PATH_LEN);
                server_config.movies_path_count++;
            }
        }
        else if (strcmp(key, "tvshows_path") == 0) {
            if (server_config.tvshows_path_count < MAX_WATCH_PATHS) {
                expand_path(value, server_config.tvshows_paths[server_config.tvshows_path_count],
                           MAX_PATH_LEN);
                server_config.tvshows_path_count++;
            }
        }
        else if (strcmp(key, "roms_path") == 0) {
            if (server_config.roms_path_count < MAX_WATCH_PATHS) {
                expand_path(value, server_config.roms_paths[server_config.roms_path_count],
                           MAX_PATH_LEN);
                server_config.roms_path_count++;
            }
        }
        else if (strcmp(key, "media_path") == 0) {
            if (server_config.media_path_count < MAX_WATCH_PATHS) {
                expand_path(value, server_config.media_paths[server_config.media_path_count],
                           MAX_PATH_LEN);
                server_config.media_path_count++;
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

    fprintf(f, "# TMDB API key (get from https://www.themoviedb.org/settings/api)\n");
    fprintf(f, "tmdb_api_key = %s\n\n", server_config.tmdb_api_key);

    fprintf(f, "# TMDB language (en-US, nb-NO, etc.)\n");
    fprintf(f, "tmdb_language = %s\n\n", server_config.tmdb_language);

    fprintf(f, "# Cache directory for thumbnails\n");
    fprintf(f, "cache_dir = %s\n\n", server_config.cache_dir);

    fprintf(f, "# Transcoder output override (empty = Nixly_Media inside each source path)\n");
    if (server_config.output_path[0])
        fprintf(f, "output_path = %s\n\n", server_config.output_path);
    else
        fprintf(f, "# output_path =\n\n");

    fprintf(f, "# Movies directories (can have multiple)\n");
    for (int i = 0; i < server_config.movies_path_count; i++) {
        fprintf(f, "movies_path = %s\n", server_config.movies_paths[i]);
    }

    fprintf(f, "\n# TV shows directories (can have multiple)\n");
    for (int i = 0; i < server_config.tvshows_path_count; i++) {
        fprintf(f, "tvshows_path = %s\n", server_config.tvshows_paths[i]);
    }

    fprintf(f, "\n# ROMs directories (can have multiple)\n");
    for (int i = 0; i < server_config.roms_path_count; i++) {
        fprintf(f, "roms_path = %s\n", server_config.roms_paths[i]);
    }

    fprintf(f, "\n# Generic media directories (watches all content recursively)\n");
    for (int i = 0; i < server_config.media_path_count; i++) {
        fprintf(f, "media_path = %s\n", server_config.media_paths[i]);
    }

    fclose(f);
    return 0;
}
