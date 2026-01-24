/*
 * Nixly Media Server - File Scanner Module
 * Scans directories for media files and extracts metadata using FFmpeg
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>

#include "scanner.h"
#include "database.h"

/* Supported video extensions */
static const char *video_extensions[] = {
    ".mp4", ".mkv", ".avi", ".mov", ".webm", ".m4v", ".ts", ".m2ts",
    ".wmv", ".flv", ".mpg", ".mpeg", ".vob", ".ogv", NULL
};

int scanner_is_media_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;

    for (int i = 0; video_extensions[i]; i++) {
        if (strcasecmp(ext, video_extensions[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Try to parse TV show info from filename
 * Patterns: Show.Name.S01E02, Show Name - 1x02, etc.
 */
static int parse_tv_info(const char *filename, char *show_name, int *season, int *episode) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    char name[256];
    strncpy(name, base, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    /* Remove extension */
    char *ext = strrchr(name, '.');
    if (ext) *ext = '\0';

    /* Try S01E02 pattern */
    char *s = name;
    while (*s) {
        if ((s[0] == 'S' || s[0] == 's') && isdigit(s[1])) {
            int sn = 0, ep = 0;
            if (sscanf(s, "S%dE%d", &sn, &ep) == 2 ||
                sscanf(s, "s%de%d", &sn, &ep) == 2) {
                *season = sn;
                *episode = ep;

                /* Extract show name (everything before SxxExx) */
                size_t len = s - name;
                if (len > 0) {
                    strncpy(show_name, name, len);
                    show_name[len] = '\0';
                    /* Clean up dots and underscores */
                    for (char *p = show_name; *p; p++) {
                        if (*p == '.' || *p == '_') *p = ' ';
                    }
                    /* Trim trailing spaces */
                    char *end = show_name + strlen(show_name) - 1;
                    while (end > show_name && *end == ' ') *end-- = '\0';
                }
                return 1;
            }
        }
        /* Try 1x02 pattern */
        if (isdigit(s[0]) && s[1] == 'x' && isdigit(s[2])) {
            int sn = 0, ep = 0;
            if (sscanf(s, "%dx%d", &sn, &ep) == 2) {
                *season = sn;
                *episode = ep;

                size_t len = s - name;
                if (len > 0) {
                    strncpy(show_name, name, len);
                    show_name[len] = '\0';
                    for (char *p = show_name; *p; p++) {
                        if (*p == '.' || *p == '_') *p = ' ';
                    }
                    char *end = show_name + strlen(show_name) - 1;
                    while (end > show_name && (*end == ' ' || *end == '-')) *end-- = '\0';
                }
                return 1;
            }
        }
        s++;
    }

    return 0;
}

/* Extract clean title from filename */
static void extract_title(const char *filepath, char *title, size_t title_size) {
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;

    strncpy(title, base, title_size - 1);
    title[title_size - 1] = '\0';

    /* Remove extension */
    char *ext = strrchr(title, '.');
    if (ext) *ext = '\0';

    /* Replace dots and underscores with spaces */
    for (char *p = title; *p; p++) {
        if (*p == '.' || *p == '_') *p = ' ';
    }
}

/* Probe media file with FFmpeg and add to database */
static int scan_media_file(const char *filepath) {
    /* Skip if already in database */
    if (database_file_exists(filepath)) {
        return 0;
    }

    AVFormatContext *fmt_ctx = NULL;
    int ret = avformat_open_input(&fmt_ctx, filepath, NULL, NULL);
    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "  Warning: Cannot open %s: %s\n", filepath, err);
        return -1;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    MediaEntry entry = {0};
    entry.filepath = (char *)filepath;

    /* Get file size */
    struct stat st;
    if (stat(filepath, &st) == 0) {
        entry.size = st.st_size;
    }

    /* Duration */
    if (fmt_ctx->duration > 0) {
        entry.duration = fmt_ctx->duration / AV_TIME_BASE;
    }

    /* Bitrate */
    entry.bitrate = fmt_ctx->bit_rate;

    /* Find video and audio streams */
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream *stream = fmt_ctx->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;

        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !entry.codec_video) {
            entry.width = codecpar->width;
            entry.height = codecpar->height;
            const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
            if (codec) {
                entry.codec_video = (char *)codec->name;
            }
        }
        else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !entry.codec_audio) {
            const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
            if (codec) {
                entry.codec_audio = (char *)codec->name;
            }
        }
    }

    /* Determine type and extract title */
    char title[256];
    char show_name[256] = {0};
    int season = 0, episode = 0;

    if (parse_tv_info(filepath, show_name, &season, &episode)) {
        entry.type = MEDIA_TYPE_EPISODE;
        entry.show_name = show_name;
        entry.season = season;
        entry.episode = episode;
        snprintf(title, sizeof(title), "%s S%02dE%02d", show_name, season, episode);
        entry.title = title;
    } else {
        entry.type = MEDIA_TYPE_MOVIE;
        extract_title(filepath, title, sizeof(title));
        entry.title = title;
    }

    printf("  Added: %s (%dx%d, %d min)\n",
           entry.title, entry.width, entry.height, entry.duration / 60);

    database_add_media(&entry);
    avformat_close_input(&fmt_ctx);

    return 0;
}

/* Recursively scan directory */
int scanner_scan_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        perror(path);
        return -1;
    }

    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        /* Skip hidden files and . .. */
        if (entry->d_name[0] == '.') continue;

        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            count += scanner_scan_directory(fullpath);
        }
        else if (S_ISREG(st.st_mode) && scanner_is_media_file(fullpath)) {
            if (scan_media_file(fullpath) == 0) {
                count++;
            }
        }
    }

    closedir(dir);
    return count;
}
