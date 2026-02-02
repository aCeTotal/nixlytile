/*
 * Nixly Media Server - File System Watcher
 * Uses inotify for real-time file change detection
 */

#ifndef WATCHER_H
#define WATCHER_H

typedef enum {
    WATCH_TYPE_MOVIES,
    WATCH_TYPE_TVSHOWS,
    WATCH_TYPE_ROMS,
    WATCH_TYPE_MEDIA     /* Generic - watches all media types */
} WatchType;

typedef void (*WatchCallback)(const char *filepath, int is_delete, WatchType type);

/* Initialize watcher system */
int watcher_init(void);

/* Add a directory to watch (recursive) */
int watcher_add_path(const char *path, WatchType type);

/* Set callback for file changes */
void watcher_set_callback(WatchCallback cb);

/* Start watching in a background thread */
int watcher_start(void);

/* Stop watching */
void watcher_stop(void);

/* Cleanup */
void watcher_cleanup(void);

/* Get number of directories being watched */
int watcher_get_count(void);

#endif /* WATCHER_H */
