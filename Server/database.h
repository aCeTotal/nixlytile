/*
 * Nixly Media Server - Database Module
 * SQLite-based media library database with TMDB metadata
 */

#ifndef DATABASE_H
#define DATABASE_H

#include <stdint.h>

typedef enum {
    MEDIA_TYPE_MOVIE = 0,
    MEDIA_TYPE_TVSHOW = 1,
    MEDIA_TYPE_EPISODE = 2
} MediaType;

typedef enum {
    CONSOLE_NES = 0,
    CONSOLE_SNES = 1,
    CONSOLE_N64 = 2,
    CONSOLE_GAMECUBE = 3,
    CONSOLE_WII = 4,
    CONSOLE_SWITCH = 5,
    CONSOLE_COUNT = 6
} ConsoleType;

typedef struct {
    int id;
    MediaType type;
    char *title;
    char *filepath;
    char *show_name;      /* For episodes: parent show name */
    int season;           /* For episodes */
    int episode;          /* For episodes */
    int64_t size;         /* File size in bytes */
    int duration;         /* Duration in seconds */
    int width;            /* Video width */
    int height;           /* Video height */
    char *codec_video;
    char *codec_audio;
    int64_t bitrate;
    char *added_date;

    /* TMDB metadata */
    int tmdb_id;          /* TMDB movie/show ID */
    char *tmdb_title;     /* Title from TMDB */
    char *overview;       /* Plot summary */
    char *poster_path;    /* Local path to poster */
    char *backdrop_path;  /* Local path to backdrop */
    char *release_date;   /* YYYY-MM-DD */
    int year;
    float rating;         /* TMDB vote average 0-10 */
    int vote_count;
    char *genres;         /* Comma-separated */

    /* For episodes */
    int tmdb_show_id;     /* Parent show's TMDB ID */
    char *episode_title;  /* Episode name from TMDB */
    char *episode_overview;
    char *still_path;     /* Episode screenshot */

    /* TMDB show totals (stored per-episode, aggregated for show view) */
    int tmdb_total_seasons;    /* Total seasons from TMDB */
    int tmdb_total_episodes;   /* Total episodes from TMDB */
    int tmdb_episode_runtime;  /* Typical episode runtime in minutes from TMDB */
    char *tmdb_status;         /* "Returning Series", "Ended", "Canceled", etc. */
    char *tmdb_next_episode;   /* YYYY-MM-DD of next episode air date */
} MediaEntry;

typedef struct {
    int id;
    ConsoleType console;
    char *title;
    char *filepath;
    char *cover_path;     /* Path to cover art */
    int64_t size;         /* File size in bytes */
    char *region;         /* USA, EUR, JPN, etc. */
    char *added_date;
} RomEntry;

/* Initialize database, create tables if needed */
int database_init(const char *db_path);

/* Close database connection */
void database_close(void);

/* Add or update media entry */
int database_add_media(MediaEntry *entry);

/* Update TMDB metadata for existing entry */
int database_update_tmdb(int id, MediaEntry *tmdb_data);

/* Check if file already exists in database */
int database_file_exists(const char *filepath);

/* Get media ID by filepath */
int database_get_id_by_path(const char *filepath);

/* Delete media entry by filepath */
int database_delete_by_path(const char *filepath);

/* Remove entries for files that no longer exist on disk */
int database_cleanup_missing(void);

/* Get total media count */
int database_get_count(void);

/* Get filepath by media ID */
char *database_get_filepath(int id);

/* Get entries missing TMDB data */
int database_get_entries_without_tmdb(MediaEntry **entries, int *count);

/* JSON exports for API */
char *database_get_library_json(void);
char *database_get_movies_json(void);
char *database_get_tvshows_json(void);
char *database_get_media_json(int id);
char *database_get_show_seasons_json(const char *show_name);
char *database_get_show_episodes_json(const char *show_name, int season);

/* Retro gaming functions */
int database_add_rom(RomEntry *entry);
int database_rom_exists(const char *filepath);
int database_get_rom_count(void);
int database_get_rom_count_by_console(ConsoleType console);
char *database_get_rom_filepath(int id);
char *database_get_roms_json(void);
char *database_get_roms_by_console_json(ConsoleType console);
char *database_get_rom_json(int id);

/* Update show status and totals for all episodes of a show */
int database_update_show_status(int tmdb_show_id, const char *status, const char *next_episode,
                                int total_seasons, int total_episodes, int episode_runtime);

/* Get unique TMDB show IDs for active (non-ended) shows */
int database_get_active_show_ids(int **ids, int *count);

/* Free entries */
void database_free_entry(MediaEntry *entry);
void database_free_rom(RomEntry *entry);

/* Console name helper */
const char *database_console_name(ConsoleType console);

#endif /* DATABASE_H */
