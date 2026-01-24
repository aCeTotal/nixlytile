/*
 * Nixly Media Server - File Scanner Module
 * Scans directories for media files and extracts metadata using FFmpeg
 */

#ifndef SCANNER_H
#define SCANNER_H

/* Scan a directory recursively for media files */
int scanner_scan_directory(const char *path);

/* Check if file is a supported media format */
int scanner_is_media_file(const char *path);

#endif /* SCANNER_H */
