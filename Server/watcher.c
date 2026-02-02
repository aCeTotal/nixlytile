/*
 * Nixly Media Server - File System Watcher Implementation
 * Uses inotify for instant file change detection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <errno.h>
#include "watcher.h"

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (1024 * (EVENT_SIZE + 256))
#define MAX_WATCHES 4096

typedef struct {
    int wd;                     /* Watch descriptor */
    char path[4096];
    WatchType type;
} WatchEntry;

static int inotify_fd = -1;
static WatchEntry watches[MAX_WATCHES];
static int watch_count = 0;
static pthread_t watcher_thread;
static volatile int running = 0;
static WatchCallback callback = NULL;
static pthread_mutex_t watch_mutex = PTHREAD_MUTEX_INITIALIZER;

int watcher_init(void) {
    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        perror("inotify_init1");
        return -1;
    }
    watch_count = 0;
    printf("File watcher initialized\n");
    return 0;
}

static int add_watch_internal(const char *path, WatchType type) {
    if (watch_count >= MAX_WATCHES) {
        fprintf(stderr, "Max watches reached\n");
        return -1;
    }

    int wd = inotify_add_watch(inotify_fd, path,
        IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE);

    if (wd < 0) {
        if (errno != ENOENT) {
            perror(path);
        }
        return -1;
    }

    pthread_mutex_lock(&watch_mutex);
    watches[watch_count].wd = wd;
    strncpy(watches[watch_count].path, path, sizeof(watches[watch_count].path) - 1);
    watches[watch_count].type = type;
    watch_count++;
    pthread_mutex_unlock(&watch_mutex);

    return wd;
}

static void add_watch_recursive(const char *path, WatchType type) {
    add_watch_internal(path, type);

    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            add_watch_recursive(fullpath, type);
        }
    }

    closedir(dir);
}

int watcher_add_path(const char *path, WatchType type) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "Watch path does not exist: %s\n", path);
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Watch path is not a directory: %s\n", path);
        return -1;
    }

    printf("Adding watch: %s (type %d)\n", path, type);
    add_watch_recursive(path, type);
    return 0;
}

void watcher_set_callback(WatchCallback cb) {
    callback = cb;
}

static WatchEntry *find_watch_by_wd(int wd) {
    pthread_mutex_lock(&watch_mutex);
    for (int i = 0; i < watch_count; i++) {
        if (watches[i].wd == wd) {
            pthread_mutex_unlock(&watch_mutex);
            return &watches[i];
        }
    }
    pthread_mutex_unlock(&watch_mutex);
    return NULL;
}

static void *watcher_thread_func(void *arg) {
    (void)arg;
    char buffer[EVENT_BUF_LEN];

    printf("File watcher thread started\n");

    while (running) {
        int length = read(inotify_fd, buffer, EVENT_BUF_LEN);

        if (length < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* No events, sleep briefly */
                usleep(100000); /* 100ms */
                continue;
            }
            perror("inotify read");
            break;
        }

        int i = 0;
        while (i < length) {
            struct inotify_event *event = (struct inotify_event *)&buffer[i];

            if (event->len > 0 && callback) {
                WatchEntry *watch = find_watch_by_wd(event->wd);
                if (watch) {
                    char fullpath[4096];
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", watch->path, event->name);

                    int is_delete = (event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0;
                    int is_create = (event->mask & (IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE)) != 0;

                    /* Handle new directory - add watch recursively */
                    if ((event->mask & IN_CREATE) && (event->mask & IN_ISDIR)) {
                        add_watch_recursive(fullpath, watch->type);
                    }

                    /* Only call callback for files (not directories) */
                    if (!(event->mask & IN_ISDIR)) {
                        if (is_delete) {
                            printf("File deleted: %s\n", fullpath);
                            callback(fullpath, 1, watch->type);
                        } else if (is_create) {
                            printf("File created/modified: %s\n", fullpath);
                            callback(fullpath, 0, watch->type);
                        }
                    }
                }
            }

            i += EVENT_SIZE + event->len;
        }
    }

    printf("File watcher thread stopped\n");
    return NULL;
}

int watcher_start(void) {
    if (running) return 0;

    running = 1;
    if (pthread_create(&watcher_thread, NULL, watcher_thread_func, NULL) != 0) {
        perror("pthread_create watcher");
        running = 0;
        return -1;
    }

    return 0;
}

void watcher_stop(void) {
    if (!running) return;

    running = 0;
    pthread_join(watcher_thread, NULL);
}

void watcher_cleanup(void) {
    watcher_stop();

    pthread_mutex_lock(&watch_mutex);
    for (int i = 0; i < watch_count; i++) {
        inotify_rm_watch(inotify_fd, watches[i].wd);
    }
    watch_count = 0;
    pthread_mutex_unlock(&watch_mutex);

    if (inotify_fd >= 0) {
        close(inotify_fd);
        inotify_fd = -1;
    }

    printf("File watcher cleaned up\n");
}

int watcher_get_count(void) {
    int count;
    pthread_mutex_lock(&watch_mutex);
    count = watch_count;
    pthread_mutex_unlock(&watch_mutex);
    return count;
}
