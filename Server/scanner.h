/*
 * Nixly Media Server - File Scanner Module
 * Scans directories for media files, extracts metadata, and fetches TMDB data
 */

#ifndef SCANNER_H
#define SCANNER_H

#include <pthread.h>
#include <time.h>

/* Live scraping progress tracking */
typedef struct {
    int active;             /* 1 if a scraping operation is running */
    const char *operation;  /* "tmdb_fetch", "tmdb_rescan", "igdb_fetch", "igdb_rescan", "show_status" */
    char current_item[256]; /* Title being scraped right now */
    int total;              /* Total items to process in this operation */
    int processed;          /* Items completed so far */
    int success;            /* Items successfully matched */
    time_t start_time;      /* When this operation started */
    pthread_mutex_t lock;
} ScrapeProgress;

extern ScrapeProgress scrape_progress;

/* Get scrape progress snapshot (thread-safe) */
void scanner_get_scrape_status(int *active, const char **operation,
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

/* Refresh show status (status + next_episode_date) for active shows */
void scanner_refresh_show_status(void);

/* ROM scanning functions */
int scanner_is_rom_file(const char *path);
int scanner_detect_console(const char *path);
int scanner_scan_rom_file(const char *filepath, int console);
int scanner_scan_rom_directory(const char *path);
void scanner_fetch_rom_covers(void);

/* Fetch IGDB metadata for ROMs that don't have it */
void scanner_fetch_rom_metadata(void);

/* Re-fetch IGDB metadata for ALL ROMs (full rescan) */
void scanner_rescan_all_rom_metadata(void);

#endif /* SCANNER_H */
