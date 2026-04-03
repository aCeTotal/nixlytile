/*
 * Nixly Media Server - Download Monitor
 * Auto-classifies and moves media from downloads to nixly_ready_media
 */

#ifndef DOWNLOADS_H
#define DOWNLOADS_H

/* Initialize download monitor (call once at startup) */
int downloads_init(void);

/* Start the background monitoring thread (30s interval) */
int downloads_start(void);

/* Stop the background thread */
void downloads_stop(void);

/* Process all pending downloads now (called from sync thread as backup) */
void downloads_process_pending(void);

/* Notify about a file change in downloads dir (called from inotify callback) */
void downloads_notify_file(const char *filepath);

#endif /* DOWNLOADS_H */
