/*
 * Nixly Media Server - Download Monitor
 * Scans a downloads directory for completed media files, classifies them
 * as Movie or TV, and moves them to the appropriate nixly_ready_media folder.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <libgen.h>

#include "downloads.h"
#include "scanner.h"
#include "config.h"

#define DL_MAX_PATH 4096
#define DL_SCAN_INTERVAL 30   /* seconds between scans */
#define DL_STABLE_AGE    30   /* file must be this old (seconds) before processing */
#define DL_SAMPLE_MAX_MB 200  /* max size in MB for a file to be considered a "sample" */

static pthread_t dl_thread;
static volatile int dl_running = 0;
static pthread_mutex_t dl_lock = PTHREAD_MUTEX_INITIALIZER;

/* Subtitle extensions we look for alongside video files */
static const char *subtitle_extensions[] = {
	".srt", ".ass", ".ssa", ".sub", ".vtt", ".idx", NULL
};

/* Extensions that indicate incomplete downloads */
static const char *incomplete_extensions[] = {
	".part", ".parts", ".!qB", ".aria2", ".tmp", ".crdownload", NULL
};

/* ---- Helpers ---- */

static int has_extension(const char *path, const char *const *exts) {
	const char *ext = strrchr(path, '.');
	if (!ext) return 0;
	for (int i = 0; exts[i]; i++) {
		if (strcasecmp(ext, exts[i]) == 0)
			return 1;
	}
	return 0;
}

static int is_incomplete_file(const char *path) {
	return has_extension(path, incomplete_extensions);
}

static int is_subtitle_file(const char *path) {
	return has_extension(path, subtitle_extensions);
}

/* Check if filename contains "sample" (case-insensitive) */
static int is_sample_file(const char *path, off_t size) {
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;

	/* Case-insensitive search for "sample" in filename */
	char lower[256];
	size_t len = strlen(base);
	if (len >= sizeof(lower)) len = sizeof(lower) - 1;
	for (size_t i = 0; i < len; i++)
		lower[i] = tolower(base[i]);
	lower[len] = '\0';

	if (strstr(lower, "sample") && size < (off_t)DL_SAMPLE_MAX_MB * 1024 * 1024)
		return 1;
	return 0;
}

/* Check if file is stable (mtime older than DL_STABLE_AGE seconds) */
static int is_file_stable(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) return 0;
	return (time(NULL) - st.st_mtime) >= DL_STABLE_AGE;
}

/* Move a file, trying rename first, falling back to copy+delete for cross-fs */
static int move_file(const char *src, const char *dst) {
	/* Try rename first (same filesystem) */
	if (rename(src, dst) == 0)
		return 0;

	if (errno != EXDEV) {
		fprintf(stderr, "Downloads: rename %s -> %s failed: %s\n",
		        src, dst, strerror(errno));
		return -1;
	}

	/* Cross-filesystem: copy + delete */
	int src_fd = open(src, O_RDONLY);
	if (src_fd < 0) {
		fprintf(stderr, "Downloads: open src %s: %s\n", src, strerror(errno));
		return -1;
	}

	struct stat st;
	if (fstat(src_fd, &st) != 0) {
		close(src_fd);
		return -1;
	}

	int dst_fd = open(dst, O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 0777);
	if (dst_fd < 0) {
		close(src_fd);
		fprintf(stderr, "Downloads: open dst %s: %s\n", dst, strerror(errno));
		return -1;
	}

	off_t remaining = st.st_size;
	while (remaining > 0) {
		ssize_t sent = sendfile(dst_fd, src_fd, NULL, remaining);
		if (sent <= 0) {
			fprintf(stderr, "Downloads: sendfile %s: %s\n", dst, strerror(errno));
			close(src_fd);
			close(dst_fd);
			unlink(dst);
			return -1;
		}
		remaining -= sent;
	}

	fsync(dst_fd);
	close(dst_fd);
	close(src_fd);

	/* Remove source after successful copy */
	unlink(src);
	return 0;
}

/* Create directory tree recursively */
static int mkdirs(const char *path) {
	char tmp[DL_MAX_PATH];
	snprintf(tmp, sizeof(tmp), "%s", path);

	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* Remove directory if empty, then check parent, etc. */
static void cleanup_empty_dirs(const char *path, const char *stop_at) {
	char dir[DL_MAX_PATH];
	snprintf(dir, sizeof(dir), "%s", path);

	while (strlen(dir) > strlen(stop_at)) {
		DIR *d = opendir(dir);
		if (!d) break;

		int empty = 1;
		struct dirent *ent;
		while ((ent = readdir(d)) != NULL) {
			if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
				continue;
			/* Skip .nfo and .nzb files — leftover metadata from scene releases */
			const char *ext = strrchr(ent->d_name, '.');
			if (ext && (strcasecmp(ext, ".nfo") == 0 || strcasecmp(ext, ".nzb") == 0 ||
			            strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".jpg") == 0 ||
			            strcasecmp(ext, ".png") == 0))
				continue;
			empty = 0;
			break;
		}
		closedir(d);

		if (!empty) break;

		/* Delete leftover junk files before rmdir */
		d = opendir(dir);
		if (d) {
			while ((ent = readdir(d)) != NULL) {
				if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
					continue;
				char junk[DL_MAX_PATH];
				snprintf(junk, sizeof(junk), "%s/%s", dir, ent->d_name);
				unlink(junk);
			}
			closedir(d);
		}

		if (rmdir(dir) != 0) break;
		printf("Downloads: Removed empty dir %s\n", dir);

		/* Go up one level */
		char *slash = strrchr(dir, '/');
		if (!slash || slash == dir) break;
		*slash = '\0';
	}
}

/* Find existing show folder across all converted_paths.
 * Looks for exact match first, then case-insensitive.
 * Returns the disk index, or -1 if not found. */
static int find_existing_show_folder(const char *show_folder, char *found_name, size_t found_size) {
	for (int i = 0; i < server_config.converted_path_count; i++) {
		char tv_dir[DL_MAX_PATH];
		snprintf(tv_dir, sizeof(tv_dir), "%s/nixly_ready_media/TV",
		         server_config.converted_paths[i]);

		DIR *d = opendir(tv_dir);
		if (!d) continue;

		struct dirent *ent;
		while ((ent = readdir(d)) != NULL) {
			if (ent->d_name[0] == '.') continue;
			if (strcasecmp(ent->d_name, show_folder) == 0) {
				snprintf(found_name, found_size, "%s", ent->d_name);
				closedir(d);
				return i;
			}
		}
		closedir(d);
	}
	return -1;
}

/* Build show folder name from parsed show_name and year */
static void build_show_folder(const char *show_name, int year, char *buf, size_t buf_size) {
	/* Replace spaces with dots for folder name consistency */
	if (year > 0)
		snprintf(buf, buf_size, "%s.%d", show_name, year);
	else
		snprintf(buf, buf_size, "%s", show_name);
}

/* Process a single media file: classify, build dest, move */
static int process_download_file(const char *filepath) {
	struct stat st;
	if (stat(filepath, &st) != 0) return -1;

	/* Skip incomplete downloads */
	if (is_incomplete_file(filepath)) return 0;

	/* Skip non-media files */
	if (!scanner_is_media_file(filepath)) return 0;

	/* Skip sample files */
	if (is_sample_file(filepath, st.st_size)) {
		printf("Downloads: Skipping sample file %s\n", filepath);
		return 0;
	}

	/* Check file stability */
	if (!is_file_stable(filepath)) return 0;

	/* Need at least one converted_path configured */
	if (server_config.converted_path_count == 0) {
		fprintf(stderr, "Downloads: No converted_paths configured\n");
		return -1;
	}

	/* Try to classify as TV episode */
	char show_name[256] = {0};
	int season = 0, episode = 0, year = 0;
	char dest[DL_MAX_PATH];
	const char *base = strrchr(filepath, '/');
	base = base ? base + 1 : filepath;

	if (scanner_parse_tv_info(filepath, show_name, &season, &episode, &year)) {
		/* TV Episode */
		char show_folder[512];
		build_show_folder(show_name, year, show_folder, sizeof(show_folder));

		/* Check if show folder already exists on any disk */
		char existing_name[512];
		int disk = find_existing_show_folder(show_folder, existing_name, sizeof(existing_name));

		if (disk >= 0) {
			/* Use existing folder name (preserves original casing) */
			snprintf(dest, sizeof(dest), "%s/nixly_ready_media/TV/%s/Season%d/%s",
			         server_config.converted_paths[disk],
			         existing_name, season, base);
		} else {
			/* New show — put on first disk */
			disk = 0;
			snprintf(dest, sizeof(dest), "%s/nixly_ready_media/TV/%s/Season%d/%s",
			         server_config.converted_paths[disk],
			         show_folder, season, base);
		}

		printf("Downloads: TV [%s S%02dE%02d] -> %s\n",
		       show_name, season, episode, dest);
	} else {
		/* Movie */
		snprintf(dest, sizeof(dest), "%s/nixly_ready_media/Movies/%s",
		         server_config.converted_paths[0], base);

		printf("Downloads: Movie [%s] -> %s\n", base, dest);
	}

	/* Check if destination already exists */
	if (access(dest, F_OK) == 0) {
		printf("Downloads: Already exists at destination, skipping: %s\n", dest);
		return 0;
	}

	/* Create destination directory */
	char dest_dir[DL_MAX_PATH];
	snprintf(dest_dir, sizeof(dest_dir), "%s", dest);
	char *last_slash = strrchr(dest_dir, '/');
	if (last_slash) *last_slash = '\0';

	if (mkdirs(dest_dir) != 0 && errno != EEXIST) {
		fprintf(stderr, "Downloads: Failed to create dir %s: %s\n",
		        dest_dir, strerror(errno));
		return -1;
	}

	/* Move the video file */
	if (move_file(filepath, dest) != 0) {
		fprintf(stderr, "Downloads: Failed to move %s\n", filepath);
		return -1;
	}

	printf("Downloads: Moved %s\n", dest);

	/* Move matching subtitle files */
	char src_dir[DL_MAX_PATH];
	snprintf(src_dir, sizeof(src_dir), "%s", filepath);
	char *src_slash = strrchr(src_dir, '/');
	if (src_slash) *src_slash = '\0';

	/* Get video stem (filename without extension) */
	char stem[256];
	snprintf(stem, sizeof(stem), "%s", base);
	char *ext = strrchr(stem, '.');
	if (ext) *ext = '\0';
	size_t stem_len = strlen(stem);

	DIR *d = opendir(src_dir);
	if (d) {
		struct dirent *ent;
		while ((ent = readdir(d)) != NULL) {
			if (!is_subtitle_file(ent->d_name)) continue;

			/* Check if subtitle matches video stem */
			if (strncasecmp(ent->d_name, stem, stem_len) != 0) continue;

			char sub_src[DL_MAX_PATH], sub_dst[DL_MAX_PATH];
			snprintf(sub_src, sizeof(sub_src), "%s/%s", src_dir, ent->d_name);
			snprintf(sub_dst, sizeof(sub_dst), "%s/%s", dest_dir, ent->d_name);

			if (move_file(sub_src, sub_dst) == 0) {
				printf("Downloads: Moved subtitle %s\n", ent->d_name);
			}
		}
		closedir(d);
	}

	/* Clean up empty source directories */
	cleanup_empty_dirs(src_dir, server_config.download_path);

	return 1;
}

/* Collect and process all media files in a directory (recursive) */
static int scan_directory(const char *path) {
	DIR *d = opendir(path);
	if (!d) return 0;

	int processed = 0;
	struct dirent *ent;
	/* We need to collect entries first because processing may delete/move files */
	char entries[256][256];
	int entry_count = 0;

	while ((ent = readdir(d)) != NULL && entry_count < 256) {
		if (ent->d_name[0] == '.') continue;
		snprintf(entries[entry_count], sizeof(entries[0]), "%s", ent->d_name);
		entry_count++;
	}
	closedir(d);

	for (int i = 0; i < entry_count && dl_running; i++) {
		char fullpath[DL_MAX_PATH];
		snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entries[i]);

		struct stat st;
		if (stat(fullpath, &st) != 0) continue;

		if (S_ISDIR(st.st_mode)) {
			processed += scan_directory(fullpath);
		} else if (S_ISREG(st.st_mode)) {
			int ret = process_download_file(fullpath);
			if (ret > 0) processed++;
		}
	}

	return processed;
}

/* ---- Public API ---- */

void downloads_process_pending(void) {
	if (server_config.download_path[0] == '\0') return;

	struct stat st;
	if (stat(server_config.download_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return;
	}

	pthread_mutex_lock(&dl_lock);
	int n = scan_directory(server_config.download_path);
	pthread_mutex_unlock(&dl_lock);

	if (n > 0) {
		printf("Downloads: Processed %d file(s)\n", n);
	}
}

void downloads_notify_file(const char *filepath) {
	printf("Downloads: inotify event for %s (will process on next scan)\n", filepath);
}

static void *dl_thread_func(void *arg) {
	(void)arg;

	while (dl_running) {
		/* Sleep in 1-second increments so we can exit promptly */
		for (int i = 0; i < DL_SCAN_INTERVAL && dl_running; i++)
			sleep(1);

		if (!dl_running) break;

		downloads_process_pending();
	}

	return NULL;
}

int downloads_init(void) {
	if (server_config.download_path[0] == '\0') {
		printf("Downloads: No download_path configured, monitor disabled\n");
		return -1;
	}

	struct stat st;
	if (stat(server_config.download_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fprintf(stderr, "Downloads: Path does not exist: %s\n",
		        server_config.download_path);
		return -1;
	}

	printf("Downloads: Monitoring %s\n", server_config.download_path);
	return 0;
}

int downloads_start(void) {
	if (server_config.download_path[0] == '\0') return -1;

	dl_running = 1;
	if (pthread_create(&dl_thread, NULL, dl_thread_func, NULL) != 0) {
		fprintf(stderr, "Downloads: Failed to start thread\n");
		dl_running = 0;
		return -1;
	}
	pthread_detach(dl_thread);
	printf("Downloads: Background thread started (interval %ds)\n", DL_SCAN_INTERVAL);
	return 0;
}

void downloads_stop(void) {
	if (!dl_running) return;
	dl_running = 0;
	printf("Downloads: Stopped\n");
}
