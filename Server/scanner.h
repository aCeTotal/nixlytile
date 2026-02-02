/*
 * Nixly Media Server - File Scanner Module
 * Scans directories for media files, extracts metadata, and fetches TMDB data
 */

#ifndef SCANNER_H
#define SCANNER_H

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

#endif /* SCANNER_H */
