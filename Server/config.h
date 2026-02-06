/*
 * Nixly Media Server - Configuration
 */

#ifndef CONFIG_H
#define CONFIG_H

#define MAX_WATCH_PATHS 32
#define MAX_PATH_LEN 4096

typedef struct {
    /* Server settings */
    int port;
    char db_path[MAX_PATH_LEN];

    /* TMDB API */
    char tmdb_api_key[128];
    char tmdb_language[8];          /* e.g., "en-US", "nb-NO" */

    /* Watch paths */
    char movies_paths[MAX_WATCH_PATHS][MAX_PATH_LEN];
    int movies_path_count;

    char tvshows_paths[MAX_WATCH_PATHS][MAX_PATH_LEN];
    int tvshows_path_count;

    char roms_paths[MAX_WATCH_PATHS][MAX_PATH_LEN];
    int roms_path_count;

    /* Generic media paths (watches all content recursively) */
    char media_paths[MAX_WATCH_PATHS][MAX_PATH_LEN];
    int media_path_count;

    /* Cache directory for thumbnails */
    char cache_dir[MAX_PATH_LEN];

    /* Transcoder output override (empty = Nixly_Media inside each source path) */
    char output_path[MAX_PATH_LEN];
} ServerConfig;

/* Global config */
extern ServerConfig server_config;

/* Load config from file, returns 0 on success */
int config_load(const char *path);

/* Save config to file */
int config_save(const char *path);

/* Initialize with defaults */
void config_init_defaults(void);

#endif /* CONFIG_H */
