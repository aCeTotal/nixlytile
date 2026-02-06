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

    /* Bandwidth limit: upload speed in Mbps, 0 = unlimited */
    int upload_mbps;

    /* Server identity for multi-server deduplication */
    char server_id[64];      /* Unique server ID (auto-generated if empty) */
    char server_name[128];   /* Human-readable name */

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

/* Get server rating (1-10) based on upload_mbps */
int config_get_server_rating(void);

/* Get server priority for current client
 * Local clients: 1000 + rating
 * Remote clients: rating
 * Used for multi-server deduplication */
int config_get_server_priority(void);

/* Set/get current client locality (called by server per-request) */
void config_set_client_local(int is_local);
int config_get_client_local(void);

#endif /* CONFIG_H */
