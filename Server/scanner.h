/*
 * Nixly Media Server - File Scanner Module
 * Scans directories for media files, extracts metadata, and fetches TMDB data
 */

#ifndef SCANNER_H
#define SCANNER_H

#include <pthread.h>
#include <time.h>
#include "tmdb.h"

/* Live scraping progress tracking */
typedef struct {
    int active;
    const char *operation;  /* e.g. "tmdb_fetch" */
    char current_item[256];
    int total;
    int processed;
    int success;
    time_t start_time;
    int session_active;
    pthread_mutex_t lock;
} ScrapeProgress;

extern ScrapeProgress tmdb_progress;

/* Get scrape progress snapshot (thread-safe) */
void scanner_get_progress(ScrapeProgress *p, int *active, const char **operation,
    char *current_item, int current_item_size,
    int *total, int *processed, int *success, time_t *start_time);

/* Check if file is a supported media file */
int scanner_is_media_file(const char *path);

/* Scan a single file and add to database
 * fetch_tmdb: if 1, immediately fetch TMDB metadata
 * Returns database ID on success, -1 on error, 0 if already exists
 */
int scanner_scan_file(const char *filepath, int fetch_tmdb);

/* Recursively scan directory for media files */
int scanner_scan_directory(const char *path);

/* Fetch TMDB metadata for entries that don't have it */
void scanner_fetch_missing_tmdb(void);

/* Re-fetch TMDB metadata for ALL entries (full rescan) */
void scanner_rescan_all_tmdb(void);

/* Search TMDB for a movie by raw title string.
 * Returns TmdbMovie* on success (caller must tmdb_free_movie()), NULL on failure.
 * Used by downloads.c for pre-validation before moving files. */
TmdbMovie *scanner_search_movie_tmdb(const char *title);

/* Extract clean title from filename (for external use by downloads.c) */
void scanner_extract_title(const char *filepath, char *title, size_t title_size);

/* Apply pre-fetched TMDB movie data to a database entry */
void scanner_apply_movie_tmdb(int db_id, TmdbMovie *movie);

/* Refresh show status (status + next_episode_date) for active shows */
void scanner_refresh_show_status(void);

/* Parse TV show info from filename (S01E02 patterns, directory structure, etc.)
 * Returns 1 if TV episode detected, 0 if not */
int scanner_parse_tv_info(const char *filename, char *show_name, int *season, int *episode, int *year);

/* Session-based progress: accumulates totals across multiple operations */
void scanner_scrape_session_begin(ScrapeProgress *p);
void scanner_scrape_session_end(ScrapeProgress *p);

#endif /* SCANNER_H */
