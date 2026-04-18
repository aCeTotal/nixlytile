/*
 * Nixly Media Server - Download Monitor
 * Scans a downloads directory for completed media files, classifies them
 * as Movie or TV, renames cleanly, scrapes TMDB, and moves to
 * converted_paths[]/nixly_ready_media/TV|Movies/.
 *
 * No transcoding — files are moved as-is.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <libgen.h>

#include "downloads.h"
#include "scanner.h"
#include "tmdb.h"
#include "config.h"

#define DL_MAX_PATH 4096
#define DL_SCAN_INTERVAL 30   /* seconds between scans */
#define DL_STABLE_AGE    30   /* file must be this old (seconds) before processing */
#define DL_SAMPLE_MAX_MB 200  /* max size in MB for a file to be considered a "sample" */
#define DL_MAX_PARTS     20  /* max parts in a multi-part movie */

/* Growable list of file paths */
typedef struct {
	char **paths;
	int    count;
	int    capacity;
} FileList;

static void filelist_init(FileList *fl) {
	fl->paths = NULL;
	fl->count = 0;
	fl->capacity = 0;
}

static void filelist_add(FileList *fl, const char *path) {
	if (fl->count >= fl->capacity) {
		int newcap = fl->capacity ? fl->capacity * 2 : 64;
		char **tmp = realloc(fl->paths, newcap * sizeof(char *));
		if (!tmp) return;
		fl->paths = tmp;
		fl->capacity = newcap;
	}
	fl->paths[fl->count] = strdup(path);
	if (fl->paths[fl->count])
		fl->count++;
}

static void filelist_remove(FileList *fl, int idx) {
	if (idx < 0 || idx >= fl->count) return;
	free(fl->paths[idx]);
	fl->paths[idx] = fl->paths[fl->count - 1];
	fl->count--;
}

static void filelist_free(FileList *fl) {
	for (int i = 0; i < fl->count; i++)
		free(fl->paths[i]);
	free(fl->paths);
	fl->paths = NULL;
	fl->count = 0;
	fl->capacity = 0;
}

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
	if (rename(src, dst) == 0)
		return 0;

	if (errno != EXDEV) {
		fprintf(stderr, "Downloads: rename %s -> %s failed: %s\n",
		        src, dst, strerror(errno));
		return -1;
	}

	/* Cross-filesystem: copy + delete */
	{
		struct stat warn_st;
		if (stat(src, &warn_st) == 0) {
			double gb = warn_st.st_size / (1024.0 * 1024.0 * 1024.0);
			const char *bsrc = strrchr(src, '/');
			bsrc = bsrc ? bsrc + 1 : src;
			printf("Downloads: Cross-filesystem move for %s (%.1f GB)"
			       " — consider placing unprocessed_path on same disk\n", bsrc, gb);
		}
	}
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
	char dotted[256];
	strncpy(dotted, show_name, sizeof(dotted) - 1);
	dotted[sizeof(dotted) - 1] = '\0';
	for (char *p = dotted; *p; p++) {
		if (*p == ' ') *p = '.';
	}
	if (year > 0)
		snprintf(buf, buf_size, "%s.%d", dotted, year);
	else
		snprintf(buf, buf_size, "%s", dotted);
}

/* Extract part number from filename (PT1, Part 1, pt.2, etc.)
 * Returns part number (1, 2, ...) or 0 if none found */
static int extract_part_number(const char *name) {
	for (const char *p = name; *p; p++) {
		if (p != name && isalpha((unsigned char)*(p - 1))) continue;

		const char *after = NULL;
		if (strncasecmp(p, "part", 4) == 0 && !isalpha((unsigned char)p[4]))
			after = p + 4;
		else if (strncasecmp(p, "pt", 2) == 0 && !isalpha((unsigned char)p[2]))
			after = p + 2;

		if (after) {
			while (*after == ' ' || *after == '.' || *after == '_') after++;
			if (isdigit((unsigned char)*after)) return atoi(after);
		}
	}
	return 0;
}

/* Strip part indicator from filename to produce a grouping key.
 * E.g. "The.Movie.Part1.mkv" and "The.Movie.Part2.mkv" both → "the movie"
 * Returns 1 if a part indicator was found and stripped, 0 otherwise. */
static int strip_part_for_grouping(const char *filepath, char *out, size_t out_size) {
	const char *base = strrchr(filepath, '/');
	base = base ? base + 1 : filepath;

	/* Copy basename without extension */
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", base);
	char *dot = strrchr(buf, '.');
	if (dot) *dot = '\0';

	/* Normalize dots/underscores/hyphens → spaces, lowercase */
	for (char *p = buf; *p; p++) {
		if (*p == '.' || *p == '_' || *p == '-')
			*p = ' ';
		*p = tolower((unsigned char)*p);
	}

	/* Find and remove part indicator */
	char *found = NULL;
	for (char *p = buf; *p; p++) {
		if (p != buf && isalpha((unsigned char)*(p - 1))) continue;

		if (strncasecmp(p, "part", 4) == 0 && !isalpha((unsigned char)p[4])) {
			found = p;
			break;
		}
		if (strncasecmp(p, "pt", 2) == 0 && !isalpha((unsigned char)p[2])) {
			found = p;
			break;
		}
	}

	if (!found) return 0;

	/* Remove from the part indicator to the end of the number */
	char *end = found;
	/* Skip "part" or "pt" */
	if (strncasecmp(end, "part", 4) == 0) end += 4;
	else end += 2;
	/* Skip separators */
	while (*end == ' ' || *end == '.' || *end == '_') end++;
	/* Skip digits */
	while (isdigit((unsigned char)*end)) end++;

	/* Shift remainder over the part indicator */
	memmove(found, end, strlen(end) + 1);

	/* Trim trailing spaces */
	size_t len = strlen(buf);
	while (len > 0 && buf[len - 1] == ' ') buf[--len] = '\0';
	/* Trim leading spaces */
	char *start = buf;
	while (*start == ' ') start++;

	snprintf(out, out_size, "%s", start);
	return 1;
}

/* Merge N part files into a single MKV using ffmpeg concat demuxer.
 * paths[] must be sorted by part number. Returns 0 on success, -1 on failure. */
static int merge_parts(char *paths[], int count, const char *output) {
	if (count < 2) return -1;

	/* Write concat list file */
	char list_path[DL_MAX_PATH];
	snprintf(list_path, sizeof(list_path), "%s.concat_list.txt", paths[0]);

	FILE *f = fopen(list_path, "w");
	if (!f) {
		fprintf(stderr, "Downloads: merge_parts: cannot create list file %s\n",
		        list_path);
		return -1;
	}
	for (int i = 0; i < count; i++)
		fprintf(f, "file '%s'\n", paths[i]);
	fclose(f);

	printf("Downloads: Merging %d parts → %s\n", count, output);

	pid_t pid = fork();
	if (pid < 0) {
		unlink(list_path);
		return -1;
	}

	if (pid == 0) {
		/* Redirect stderr to /dev/null */
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
		execlp("ffmpeg", "ffmpeg",
		       "-y", "-nostdin",
		       "-f", "concat", "-safe", "0",
		       "-i", list_path,
		       "-c", "copy",
		       "-f", "matroska",
		       "-reserve_index_space", "524288",
		       "-cluster_size_limit", "2097152",
		       "-cluster_time_limit", "2000",
		       output, (char *)NULL);
		_exit(127);
	}

	int status;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) { unlink(list_path); return -1; }
	}

	unlink(list_path);

	if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		/* Delete source parts */
		for (int i = 0; i < count; i++)
			unlink(paths[i]);
		printf("Downloads: Merge complete: %s\n", output);
		return 0;
	}

	fprintf(stderr, "Downloads: merge_parts: ffmpeg exited with status %d\n",
	        WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	/* Remove failed output */
	unlink(output);
	return -1;
}

/* Build merged output path from Part 1's path by removing part indicator.
 * E.g. "The.Movie.Part1.mkv" → "The.Movie.mkv" */
static void build_merged_path(const char *part1_path, char *out, size_t out_size) {
	char dir[DL_MAX_PATH], base[512];
	snprintf(dir, sizeof(dir), "%s", part1_path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		snprintf(base, sizeof(base), "%s", slash + 1);
	} else {
		dir[0] = '.'; dir[1] = '\0';
		snprintf(base, sizeof(base), "%s", part1_path);
	}

	/* Get extension */
	const char *ext = strrchr(base, '.');
	if (!ext) ext = ".mkv";

	/* Remove extension from base for processing */
	char stem[512];
	snprintf(stem, sizeof(stem), "%s", base);
	char *dot = strrchr(stem, '.');
	if (dot) *dot = '\0';

	/* Find and remove part indicator from stem */
	for (char *p = stem; *p; p++) {
		if (p != stem && isalpha((unsigned char)*(p - 1))) continue;

		char *match = NULL;
		int skip_len = 0;
		if (strncasecmp(p, "part", 4) == 0 && !isalpha((unsigned char)p[4])) {
			match = p;
			skip_len = 4;
		} else if (strncasecmp(p, "pt", 2) == 0 && !isalpha((unsigned char)p[2])) {
			match = p;
			skip_len = 2;
		}

		if (match) {
			char *end = match + skip_len;
			while (*end == ' ' || *end == '.' || *end == '_' || *end == '-') end++;
			while (isdigit((unsigned char)*end)) end++;
			/* Also consume one trailing separator if present */
			if (*end == '.' || *end == '_' || *end == '-' || *end == ' ') end++;
			memmove(match, end, strlen(end) + 1);
			/* Trim trailing separators */
			size_t slen = strlen(stem);
			while (slen > 0 && (stem[slen-1] == '.' || stem[slen-1] == '_' ||
			       stem[slen-1] == '-' || stem[slen-1] == ' '))
				stem[--slen] = '\0';
			break;
		}
	}

	snprintf(out, out_size, "%s/%s%s", dir, stem, ext);
}

/* Delete external subtitles associated with a video file */
static void delete_external_subs(const char *video_path) {
	char dir[DL_MAX_PATH];
	snprintf(dir, sizeof(dir), "%s", video_path);
	char *slash = strrchr(dir, '/');
	if (!slash) return;
	*slash = '\0';

	const char *base = slash + 1;
	char stem[256];
	snprintf(stem, sizeof(stem), "%s", base);
	char *dot = strrchr(stem, '.');
	if (dot) *dot = '\0';
	size_t stem_len = strlen(stem);

	DIR *d = opendir(dir);
	if (!d) return;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (!is_subtitle_file(ent->d_name)) continue;
		if (strncasecmp(ent->d_name, stem, stem_len) != 0) continue;
		char sub_path[DL_MAX_PATH];
		snprintf(sub_path, sizeof(sub_path), "%s/%s", dir, ent->d_name);
		unlink(sub_path);
		printf("Downloads: Deleted subtitle from merged part: %s\n", ent->d_name);
	}
	closedir(d);
}

/* Group info for multi-part merge */
typedef struct {
	char  key[512];            /* grouping key from strip_part_for_grouping */
	char *paths[DL_MAX_PARTS]; /* file paths, indexed by part_number-1 */
	int   part_nums[DL_MAX_PARTS];
	int   count;               /* number of parts found */
	int   max_part;            /* highest part number */
	int   all_stable;          /* all files stable? */
} PartGroup;

/* Scan FileList for multi-part movies, merge complete groups, defer incomplete.
 * Modifies the list in-place: merged/deferred files are removed. */
static void merge_multipart_movies(FileList *fl) {
	PartGroup groups[32];
	int ngroups = 0;

	/* Phase 1: identify multi-part files and group them */
	for (int i = 0; i < fl->count; i++) {
		const char *path = fl->paths[i];
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;

		int partnum = extract_part_number(base);
		if (partnum <= 0) continue;
		if (partnum > DL_MAX_PARTS) continue;

		if (!scanner_is_media_file(path)) continue;

		char key[512];
		if (!strip_part_for_grouping(path, key, sizeof(key))) continue;

		/* Find or create group */
		PartGroup *grp = NULL;
		for (int g = 0; g < ngroups; g++) {
			if (strcasecmp(groups[g].key, key) == 0) {
				grp = &groups[g];
				break;
			}
		}
		if (!grp) {
			if (ngroups >= 32) continue;
			grp = &groups[ngroups++];
			memset(grp, 0, sizeof(*grp));
			snprintf(grp->key, sizeof(grp->key), "%s", key);
			grp->all_stable = 1;
		}

		if (grp->count < DL_MAX_PARTS) {
			grp->paths[grp->count] = fl->paths[i];
			grp->part_nums[grp->count] = partnum;
			grp->count++;
			if (partnum > grp->max_part)
				grp->max_part = partnum;
			if (!is_file_stable(path))
				grp->all_stable = 0;
		}
	}

	if (ngroups == 0) return;

	/* Phase 2: process each group */
	for (int g = 0; g < ngroups; g++) {
		PartGroup *grp = &groups[g];
		if (grp->count < 2) {
			/* Single part found — defer (don't process individually).
			 * Remove from list so it won't be processed this scan. */
			printf("Downloads: [wait] Waiting for remaining parts of '%s' "
			       "(have part %d)\n", grp->key, grp->part_nums[0]);
			for (int p = 0; p < grp->count; p++) {
				for (int i = 0; i < fl->count; i++) {
					if (fl->paths[i] == grp->paths[p]) {
						filelist_remove(fl, i);
						break;
					}
				}
			}
			continue;
		}

		/* Check for contiguous parts 1..N */
		int has_part[DL_MAX_PARTS + 1] = {0};
		for (int p = 0; p < grp->count; p++)
			has_part[grp->part_nums[p]] = 1;

		int contiguous = 1;
		for (int n = 1; n <= grp->max_part; n++) {
			if (!has_part[n]) {
				printf("Downloads: [warn] '%s' missing Part %d (have %d parts, max %d) — skipping\n",
				       grp->key, n, grp->count, grp->max_part);
				contiguous = 0;
				break;
			}
		}

		if (!contiguous) {
			/* Remove from list so parts aren't processed individually */
			for (int p = 0; p < grp->count; p++) {
				for (int i = 0; i < fl->count; i++) {
					if (fl->paths[i] == grp->paths[p]) {
						filelist_remove(fl, i);
						break;
					}
				}
			}
			continue;
		}

		if (!grp->all_stable) {
			printf("Downloads: [wait] Parts of '%s' still downloading\n", grp->key);
			for (int p = 0; p < grp->count; p++) {
				for (int i = 0; i < fl->count; i++) {
					if (fl->paths[i] == grp->paths[p]) {
						filelist_remove(fl, i);
						break;
					}
				}
			}
			continue;
		}

		/* Sort paths by part number */
		char *sorted[DL_MAX_PARTS];
		for (int n = 1; n <= grp->max_part; n++) {
			for (int p = 0; p < grp->count; p++) {
				if (grp->part_nums[p] == n) {
					sorted[n - 1] = grp->paths[p];
					break;
				}
			}
		}

		/* Build output path from Part 1 */
		char merged_path[DL_MAX_PATH];
		build_merged_path(sorted[0], merged_path, sizeof(merged_path));

		/* Merge */
		if (merge_parts(sorted, grp->max_part, merged_path) == 0) {
			/* Delete external subs for Part 2+ (Part 1's subs will be
			 * renamed by process_download_file to match merged name) */
			for (int n = 1; n < grp->max_part; n++)
				delete_external_subs(sorted[n]);

			/* Remove part entries from list, add merged file */
			for (int p = 0; p < grp->count; p++) {
				for (int i = 0; i < fl->count; i++) {
					if (fl->paths[i] == grp->paths[p]) {
						filelist_remove(fl, i);
						break;
					}
				}
			}
			filelist_add(fl, merged_path);
			printf("Downloads: Merged '%s' (%d parts)\n",
			       grp->key, grp->max_part);
		} else {
			fprintf(stderr, "Downloads: Merge failed for '%s', leaving parts for retry\n",
			        grp->key);
			/* Remove from list so they aren't processed individually this scan */
			for (int p = 0; p < grp->count; p++) {
				for (int i = 0; i < fl->count; i++) {
					if (fl->paths[i] == grp->paths[p]) {
						filelist_remove(fl, i);
						break;
					}
				}
			}
		}
	}
}

/* Recursively collect all regular files from a directory into a FileList */
static void collect_files(const char *path, FileList *fl) {
	DIR *d = opendir(path);
	if (!d) return;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.') continue;

		char fullpath[DL_MAX_PATH];
		snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);

		struct stat st;
		if (stat(fullpath, &st) != 0) continue;

		if (S_ISDIR(st.st_mode))
			collect_files(fullpath, fl);
		else if (S_ISREG(st.st_mode))
			filelist_add(fl, fullpath);
	}
	closedir(d);
}

/* Process a single media file: classify, rename, move, scrape TMDB */
static int process_download_file(const char *filepath) {
	struct stat st;
	if (stat(filepath, &st) != 0) return -1;

	const char *base = strrchr(filepath, '/');
	base = base ? base + 1 : filepath;

	/* Skip incomplete downloads */
	if (is_incomplete_file(filepath)) {
		printf("Downloads: [skip] incomplete: %s\n", base);
		return 0;
	}

	/* Skip non-media files */
	if (!scanner_is_media_file(filepath)) return 0;

	/* Skip sample files */
	if (is_sample_file(filepath, st.st_size)) {
		printf("Downloads: [skip] sample: %s\n", base);
		return 0;
	}

	/* Check file stability */
	if (!is_file_stable(filepath)) {
		printf("Downloads: [skip] still downloading (mtime < %ds): %s\n",
		       DL_STABLE_AGE, base);
		return 0;
	}

	/* Need at least one converted_path configured */
	if (server_config.converted_path_count == 0) {
		fprintf(stderr, "Downloads: ERROR: No converted_path configured — cannot move files.\n"
		        "  Add converted_path to your config, e.g.:\n"
		        "  converted_path = /mnt/bigdisk1/www/aceclan\n");
		return -1;
	}

	/* Try to classify as TV episode */
	char show_name[256] = {0};
	int season = 0, episode = 0, year = 0;
	char dest[DL_MAX_PATH];
	char movie_clean_stem[256] = {0};  /* Clean movie name stem for subtitle renaming */
	TmdbMovie *movie_result = NULL;  /* Non-NULL if movie pre-validated against TMDB */

	if (scanner_parse_tv_info(filepath, show_name, &season, &episode, &year)) {
		/* TV Episode — generate clean filename */
		char show_folder[512];
		build_show_folder(show_name, year, show_folder, sizeof(show_folder));

		const char *orig_ext = strrchr(base, '.');
		if (!orig_ext) orig_ext = ".mkv";

		char clean_name[512];
		char dotted_name[256];
		strncpy(dotted_name, show_name, sizeof(dotted_name) - 1);
		dotted_name[sizeof(dotted_name) - 1] = '\0';
		for (char *p = dotted_name; *p; p++) {
			if (*p == ' ') *p = '.';
		}
		if (year > 0)
			snprintf(clean_name, sizeof(clean_name), "%s.%d.S%02dE%02d%s",
			         dotted_name, year, season, episode, orig_ext);
		else
			snprintf(clean_name, sizeof(clean_name), "%s.S%02dE%02d%s",
			         dotted_name, season, episode, orig_ext);

		/* Check if show folder already exists on any disk */
		char existing_name[512];
		int disk = find_existing_show_folder(show_folder, existing_name, sizeof(existing_name));

		if (disk >= 0) {
			snprintf(dest, sizeof(dest), "%s/nixly_ready_media/TV/%s/Season%d/%s",
			         server_config.converted_paths[disk],
			         existing_name, season, clean_name);
		} else {
			disk = 0;
			snprintf(dest, sizeof(dest), "%s/nixly_ready_media/TV/%s/Season%d/%s",
			         server_config.converted_paths[disk],
			         show_folder, season, clean_name);
		}

		printf("Downloads: TV [%s S%02dE%02d] -> %s\n",
		       show_name, season, episode, dest);
	} else {
		/* Movie — verify TMDB match before moving (if API key configured) */
		TmdbMovie *movie = NULL;
		if (server_config.tmdb_api_key[0]) {
			char title_buf[256];
			scanner_extract_title(filepath, title_buf, sizeof(title_buf));
			movie = scanner_search_movie_tmdb(title_buf);
			if (!movie) {
				printf("Downloads: TMDB lookup failed for '%s', not moving\n", base);
				return 0;
			}
		}

		/* Build clean filename from TMDB title, or fall back to original */
		const char *orig_ext = strrchr(base, '.');
		if (!orig_ext) orig_ext = ".mkv";
		int part = extract_part_number(base);

		char clean_movie[512];
		if (movie) {
			char dotted[256];
			size_t j = 0;
			for (size_t i = 0; movie->title[i] && j < sizeof(dotted) - 1; i++) {
				char c = movie->title[i];
				if (c == ' ' || c == '_' || c == '.' || c == '-') {
					if (j > 0 && dotted[j - 1] != '.')
						dotted[j++] = '.';
				} else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				           (c >= '0' && c <= '9')) {
					dotted[j++] = c;
				}
				/* other chars (colons, quotes, etc.) stripped */
			}
			while (j > 0 && dotted[j - 1] == '.') j--;
			dotted[j] = '\0';

			/* Build: Title.Year[.PartN].ext */
			char stem[384];
			if (movie->year > 0 && part > 0)
				snprintf(stem, sizeof(stem), "%s.%d.Part%d",
				         dotted, movie->year, part);
			else if (movie->year > 0)
				snprintf(stem, sizeof(stem), "%s.%d", dotted, movie->year);
			else if (part > 0)
				snprintf(stem, sizeof(stem), "%s.Part%d", dotted, part);
			else
				snprintf(stem, sizeof(stem), "%s", dotted);

			snprintf(clean_movie, sizeof(clean_movie), "%s%s", stem, orig_ext);

			/* Store clean stem for subtitle renaming */
			snprintf(movie_clean_stem, sizeof(movie_clean_stem), "%s", stem);
		} else {
			snprintf(clean_movie, sizeof(clean_movie), "%s", base);
		}

		snprintf(dest, sizeof(dest), "%s/nixly_ready_media/Movies/%s",
		         server_config.converted_paths[0], clean_movie);

		printf("Downloads: Movie [%s] -> %s\n", base, dest);

		/* Check if destination already exists */
		if (access(dest, F_OK) == 0) {
			printf("Downloads: Already exists at destination, skipping: %s\n", dest);
			if (movie) tmdb_free_movie(movie);
			return 0;
		}

		/* Will move + scan below (movie_result set for post-move TMDB apply) */
		movie_result = movie;
	}

	/* Common path: create dirs, move, scan */

	/* Check if destination already exists (TV path; movie checked above) */
	if (!movie_result && access(dest, F_OK) == 0) {
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
		if (movie_result) tmdb_free_movie(movie_result);
		return -1;
	}

	/* Move the video file */
	if (move_file(filepath, dest) != 0) {
		fprintf(stderr, "Downloads: Failed to move %s\n", filepath);
		if (movie_result) tmdb_free_movie(movie_result);
		return -1;
	}

	printf("Downloads: Moved %s\n", dest);

	if (movie_result) {
		/* Movie: scan without TMDB fetch, then apply pre-fetched result */
		int db_id = scanner_scan_file(dest, 0);
		if (db_id > 0)
			scanner_apply_movie_tmdb(db_id, movie_result);
		tmdb_free_movie(movie_result);
	} else {
		/* TV: scan and fetch TMDB metadata */
		scanner_scan_file(dest, 1);
	}

	/* Move matching subtitle files */
	char src_dir[DL_MAX_PATH];
	snprintf(src_dir, sizeof(src_dir), "%s", filepath);
	char *src_slash = strrchr(src_dir, '/');
	if (src_slash) *src_slash = '\0';

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
			if (strncasecmp(ent->d_name, stem, stem_len) != 0) continue;

			char sub_src[DL_MAX_PATH], sub_dst[DL_MAX_PATH];
			snprintf(sub_src, sizeof(sub_src), "%s/%s", src_dir, ent->d_name);

			/* Rename subtitle to match clean movie name */
			if (movie_clean_stem[0]) {
				const char *suffix = ent->d_name + stem_len;
				snprintf(sub_dst, sizeof(sub_dst), "%s/%s%s",
				         dest_dir, movie_clean_stem, suffix);
			} else {
				snprintf(sub_dst, sizeof(sub_dst), "%s/%s",
				         dest_dir, ent->d_name);
			}

			if (move_file(sub_src, sub_dst) == 0) {
				printf("Downloads: Moved subtitle %s\n", sub_dst);
			}
		}
		closedir(d);
	}

	/* Clean up empty source directories. Stop at the unprocessed_path root
	 * that contains this src_dir so we don't ascend outside it. */
	const char *stop_at = NULL;
	for (int i = 0; i < server_config.unprocessed_path_count; i++) {
		const char *root = server_config.unprocessed_paths[i];
		size_t rlen = strlen(root);
		if (strncmp(src_dir, root, rlen) == 0) {
			stop_at = root;
			break;
		}
	}
	if (stop_at) cleanup_empty_dirs(src_dir, stop_at);

	return 1;
}

/* Collect files, merge multi-part movies, then process remaining files */
static int scan_directory(const char *path) {
	FileList fl;
	filelist_init(&fl);

	/* Phase 1: recursively collect all files */
	collect_files(path, &fl);

	if (fl.count == 0) {
		filelist_free(&fl);
		return 0;
	}

	/* Phase 2: group and merge multi-part movies */
	merge_multipart_movies(&fl);

	/* Phase 3: process remaining files individually */
	int processed = 0;
	for (int i = 0; i < fl.count && dl_running; i++) {
		int ret = process_download_file(fl.paths[i]);
		if (ret > 0) processed++;
	}

	filelist_free(&fl);
	return processed;
}

/* ---- Public API ---- */

void downloads_process_pending(void) {
	if (server_config.unprocessed_path_count == 0) return;

	int total = 0;
	pthread_mutex_lock(&dl_lock);
	for (int i = 0; i < server_config.unprocessed_path_count; i++) {
		const char *src = server_config.unprocessed_paths[i];
		struct stat st;
		if (stat(src, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		total += scan_directory(src);
	}
	pthread_mutex_unlock(&dl_lock);

	if (total > 0) {
		printf("Downloads: Processed %d file(s)\n", total);
	}
}

void downloads_notify_file(const char *filepath) {
	printf("Downloads: inotify event for %s (will process on next scan)\n", filepath);
}

static void *dl_thread_func(void *arg) {
	(void)arg;

	while (dl_running) {
		for (int i = 0; i < DL_SCAN_INTERVAL && dl_running; i++)
			sleep(1);

		if (!dl_running) break;

		downloads_process_pending();
	}

	return NULL;
}

int downloads_init(void) {
	if (server_config.unprocessed_path_count == 0) {
		printf("Downloads: No unprocessed_path configured, monitor disabled\n");
		return -1;
	}

	if (server_config.converted_path_count == 0) {
		fprintf(stderr, "Downloads: ERROR: No converted_path configured — cannot move files.\n");
		return -1;
	}

	/* Create TV and Movies directories if they don't exist */
	for (int i = 0; i < server_config.converted_path_count; i++) {
		char tv_dir[DL_MAX_PATH], movies_dir[DL_MAX_PATH];
		snprintf(tv_dir, sizeof(tv_dir), "%s/nixly_ready_media/TV",
		         server_config.converted_paths[i]);
		snprintf(movies_dir, sizeof(movies_dir), "%s/nixly_ready_media/Movies",
		         server_config.converted_paths[i]);
		mkdirs(tv_dir);
		mkdirs(movies_dir);
	}

	for (int i = 0; i < server_config.unprocessed_path_count; i++) {
		printf("Downloads: Monitoring  %s\n", server_config.unprocessed_paths[i]);
	}
	printf("Downloads: TV dest     %s/nixly_ready_media/TV\n",
	       server_config.converted_paths[0]);
	printf("Downloads: Movies dest %s/nixly_ready_media/Movies\n",
	       server_config.converted_paths[0]);
	return 0;
}

int downloads_start(void) {
	if (server_config.unprocessed_path_count == 0) return -1;

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
