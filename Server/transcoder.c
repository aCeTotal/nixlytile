#define _GNU_SOURCE

/*
 * Nixly Media Server - Transcoder Module
 * Automatic x265 CRF 14 conversion to MKV with FLAC 7.1 + Atmos passthrough
 *
 * Processing order per source path:
 *   1. TV shows (sorted by show name -> season -> episode)
 *   2. Movies (sorted by title)
 *
 * Audio tracks:
 *   Track 1: "Lossless 7.1" - FLAC 7.1 (from best available audio stream)
 *   Track 2: "Atmos Passthrough" - TrueHD passthrough (if available)
 *
 * Video:
 *   x265 CRF 14, 10-bit, closed GOP, keyint 48
 *   HDR10/HLG metadata preserved automatically when present
 *
 * MKV optimized for HTTP streaming:
 *   - Cues (seek index) at file start via reserve_index_space
 *   - 2s cluster time limit for frequent seek points
 *   - All subtitle formats preserved as-is
 *
 * Output: <converted_path>/nixly_ready_media/TV|Movies/<title>.x265.CRF14.mkv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>

#include "transcoder.h"
#include "config.h"
#include "database.h"
#include "scanner.h"
#include "tmdb.h"

/* Supported video extensions */
static const char *video_extensions[] = {
    ".mp4", ".mkv", ".avi", ".mov", ".webm", ".m4v", ".ts", ".m2ts",
    ".wmv", ".flv", ".mpg", ".mpeg", ".vob", ".ogv", NULL
};

/* Transcoder state */
static struct {
    TranscodeState state;
    pthread_t thread;
    pthread_mutex_t lock;
    volatile int stop_requested;
    volatile pid_t ffmpeg_pid;
    int thread_active;

    /* Progress */
    char current_file[4096];
    int total_jobs;
    int completed_jobs;
    double duration_secs;   /* Total duration of current file */
    double progress_secs;   /* Current encoding position */
} tc;

/* Skip list: titles to skip during transcoding */
#define MAX_SKIP 256
static struct {
    char titles[MAX_SKIP][256];
    int count;
    pthread_mutex_t lock;
} skip_list = { .count = 0, .lock = PTHREAD_MUTEX_INITIALIZER };

/* Priority override: title to process next */
static struct {
    char title[256];
    pthread_mutex_t lock;
} priority_override = { .title = {0}, .lock = PTHREAD_MUTEX_INITIALIZER };

static int is_skipped(const char *title) {
    pthread_mutex_lock(&skip_list.lock);
    for (int i = 0; i < skip_list.count; i++) {
        if (strcmp(skip_list.titles[i], title) == 0) {
            pthread_mutex_unlock(&skip_list.lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&skip_list.lock);
    return 0;
}

/* ================================================================
 *  Helpers
 * ================================================================ */

static int is_video_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    for (int i = 0; video_extensions[i]; i++) {
        if (strcasecmp(ext, video_extensions[i]) == 0) return 1;
    }
    return 0;
}

static int is_already_converted(const char *path) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return (strstr(base, ".x265.CRF14") != NULL);
}

static int is_in_output_dir(const char *path) {
    /* Skip anything inside nixly_ready_media or nixly_transcode_tmp */
    if (strstr(path, "/nixly_ready_media") != NULL) return 1;
    if (strstr(path, "/nixly_transcode_tmp") != NULL) return 1;
    return 0;
}

/* Wait until file is stable (not being written to)
 * Returns 1 if file is stable, 0 if file disappeared or stop requested */
static int wait_for_stable_file(const char *path) {
    struct stat st1, st2;
    const int stability_checks = 3;      /* Number of consecutive stable checks */
    const int check_interval_sec = 10;   /* Seconds between checks */

    if (stat(path, &st1) != 0) {
        fprintf(stderr, "  File not found: %s\n", path);
        return 0;
    }

    printf("  Checking file stability...\n");

    for (int stable_count = 0; stable_count < stability_checks; ) {
        if (tc.stop_requested) return 0;

        sleep(check_interval_sec);

        if (stat(path, &st2) != 0) {
            fprintf(stderr, "  File disappeared: %s\n", path);
            return 0;
        }

        if (st2.st_size == st1.st_size && st2.st_mtime == st1.st_mtime) {
            stable_count++;
            printf("  File stable check %d/%d (size: %ld bytes)\n",
                   stable_count, stability_checks, (long)st2.st_size);
        } else {
            /* File changed, reset counter */
            stable_count = 0;
            printf("  File still being written (size: %ld -> %ld bytes)\n",
                   (long)st1.st_size, (long)st2.st_size);
            st1 = st2;
        }
    }

    printf("  File is complete and stable\n");
    return 1;
}

/* Recursive mkdir */
static void mkdir_p(const char *path) {
    char tmp[4096];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Recursive rmdir (for cleaning up failed DASH output) */
static void rmdir_r(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            rmdir_r(fullpath);
        } else {
            unlink(fullpath);
        }
    }
    closedir(dir);
    rmdir(path);
}

/* ================================================================
 *  Blu-ray Detection
 * ================================================================ */

/* Check if a directory is a Blu-ray disc copy (contains BDMV/index.bdmv) */
static int is_bluray_dir(const char *path) {
    char idx[4096];
    struct stat st;

    snprintf(idx, sizeof(idx), "%s/BDMV/index.bdmv", path);
    if (stat(idx, &st) == 0) return 1;

    /* Some rips use uppercase */
    snprintf(idx, sizeof(idx), "%s/BDMV/INDEX.BDM", path);
    if (stat(idx, &st) == 0) return 1;

    return 0;
}

/* Find the main feature in a Blu-ray structure (largest .m2ts in BDMV/STREAM/) */
static int find_bluray_main_feature(const char *bdmv_root, char *out, size_t max) {
    char stream_dir[4096];
    snprintf(stream_dir, sizeof(stream_dir), "%s/BDMV/STREAM", bdmv_root);

    DIR *dir = opendir(stream_dir);
    if (!dir) return 0;

    off_t largest = 0;
    char best[4096] = {0};

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *ext = strrchr(entry->d_name, '.');
        if (!ext || strcasecmp(ext, ".m2ts") != 0) continue;

        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", stream_dir, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) == 0 && st.st_size > largest) {
            largest = st.st_size;
            strncpy(best, fullpath, sizeof(best) - 1);
        }
    }
    closedir(dir);

    if (largest > 0) {
        strncpy(out, best, max - 1);
        out[max - 1] = '\0';
        return 1;
    }
    return 0;
}

/* ================================================================
 *  Multi-part Blu-ray Detection
 *  Detects: p1/P1/part1, p2/P2/part2 (case-insensitive)
 *  Separated by '.', '_', '-', ' ' or at end of name
 * ================================================================ */

/* Detect part number from a directory name.
 * Returns 1 or 2 if found, 0 otherwise.
 * If base_out is non-NULL, writes the path with the part indicator stripped. */
static int detect_bluray_part(const char *path, char *base_out, size_t base_max) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    /* Scan for part indicators in the directory name */
    for (const char *p = name; *p; p++) {
        /* Must be at start of name or preceded by a separator */
        if (p != name && *(p - 1) != '.' && *(p - 1) != '_' &&
            *(p - 1) != '-' && *(p - 1) != ' ')
            continue;

        int part = 0;
        int match_len = 0;

        /* Check "part1"/"part2" first (longer match) */
        if (strncasecmp(p, "part1", 5) == 0) { part = 1; match_len = 5; }
        else if (strncasecmp(p, "part2", 5) == 0) { part = 2; match_len = 5; }
        /* Then "p1"/"p2" */
        else if (strncasecmp(p, "p1", 2) == 0) { part = 1; match_len = 2; }
        else if (strncasecmp(p, "p2", 2) == 0) { part = 2; match_len = 2; }

        if (part == 0) continue;

        /* Must be followed by end-of-string, separator, or whitespace */
        char after = p[match_len];
        if (after != '\0' && after != '.' && after != '_' &&
            after != '-' && after != ' ')
            continue;

        /* Build base path with part indicator stripped */
        if (base_out) {
            size_t prefix_len = (size_t)(p - path);
            /* Also strip the preceding separator */
            if (prefix_len > 0) {
                char sep = path[prefix_len - 1];
                if (sep == '.' || sep == '_' || sep == '-' || sep == ' ')
                    prefix_len--;
            }
            if (prefix_len >= base_max) prefix_len = base_max - 1;
            memcpy(base_out, path, prefix_len);

            /* Append any suffix after the part indicator */
            const char *suffix = p + match_len;
            size_t suffix_len = strlen(suffix);
            if (prefix_len + suffix_len >= base_max)
                suffix_len = base_max - 1 - prefix_len;
            memcpy(base_out + prefix_len, suffix, suffix_len);
            base_out[prefix_len + suffix_len] = '\0';
        }

        return part;
    }

    return 0;
}

/* Normalize title: lowercase, spaces/separators -> dots, strip specials */
static void normalize_title(const char *input, char *output, size_t max_len) {
    size_t j = 0;

    for (size_t i = 0; input[i] && j < max_len - 1; i++) {
        char c = input[i];
        if (c == ' ' || c == '_') {
            if (j > 0 && output[j - 1] != '.') output[j++] = '.';
        } else if (c >= 'A' && c <= 'Z') {
            output[j++] = c + 32; /* tolower */
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            output[j++] = c;
        } else if (c == '.' || c == '-') {
            if (j > 0 && output[j - 1] != '.') output[j++] = '.';
        }
        /* other chars (colons, quotes, etc.) are stripped */
    }

    /* Trim trailing dots */
    while (j > 0 && output[j - 1] == '.') j--;
    output[j] = '\0';
}

/* Strip year and scene tags from a movie title for cleaner filenames.
 * "The Matrix 1999 1080p BluRay" -> "The Matrix", year=1999
 * "The Matrix (1999)" -> "The Matrix", year=1999 */
static int strip_year_from_title(char *title) {
    char *p = title;
    int found_year = 0;
    char *year_pos = NULL;

    while (*p) {
        /* Check for year in parentheses: (1999) or [1999] */
        if ((p[0] == '(' || p[0] == '[') &&
            isdigit(p[1]) && isdigit(p[2]) && isdigit(p[3]) && isdigit(p[4]) &&
            (p[5] == ')' || p[5] == ']')) {
            int y = atoi(p + 1);
            if (y >= 1900 && y <= 2100) {
                found_year = y;
                year_pos = p;
                /* Truncate before the parenthesis */
                while (year_pos > title && (*(year_pos - 1) == ' ' || *(year_pos - 1) == '.'))
                    year_pos--;
                *year_pos = '\0';
                return found_year;
            }
        }

        /* Check for bare 4-digit year */
        if (isdigit(p[0]) && isdigit(p[1]) && isdigit(p[2]) && isdigit(p[3])) {
            int y = atoi(p);
            if (y >= 1900 && y <= 2100) {
                /* Check if followed by resolution/source, end-of-string, or whitespace */
                char *after = p + 4;
                while (*after == ' ' || *after == '.') after++;
                if (*after == '\0' ||
                    strncasecmp(after, "1080", 4) == 0 ||
                    strncasecmp(after, "2160", 4) == 0 ||
                    strncasecmp(after, "720", 3) == 0 ||
                    strncasecmp(after, "4K", 2) == 0 ||
                    strncasecmp(after, "BluRay", 6) == 0 ||
                    strncasecmp(after, "BDRip", 5) == 0 ||
                    strncasecmp(after, "WEB", 3) == 0 ||
                    strncasecmp(after, "HDTV", 4) == 0 ||
                    strncasecmp(after, "Remux", 5) == 0 ||
                    strncasecmp(after, "x264", 4) == 0 ||
                    strncasecmp(after, "x265", 4) == 0 ||
                    strncasecmp(after, "HEVC", 4) == 0 ||
                    strncasecmp(after, "HDR", 3) == 0 ||
                    strncasecmp(after, "AV1", 3) == 0 ||
                    strncasecmp(after, "DTS", 3) == 0 ||
                    strncasecmp(after, "TrueHD", 6) == 0 ||
                    strncasecmp(after, "Atmos", 5) == 0 ||
                    strncasecmp(after, "DD", 2) == 0 ||
                    strncasecmp(after, "AC3", 3) == 0 ||
                    strncasecmp(after, "AAC", 3) == 0 ||
                    strncasecmp(after, "FLAC", 4) == 0 ||
                    strncasecmp(after, "Repack", 6) == 0 ||
                    strncasecmp(after, "Proper", 6) == 0 ||
                    strncasecmp(after, "MULTi", 5) == 0 ||
                    strncasecmp(after, "MULTI", 5) == 0 ||
                    strncasecmp(after, "FRA", 3) == 0 ||
                    strncasecmp(after, "ENG", 3) == 0 ||
                    strncasecmp(after, "VOSTFR", 6) == 0 ||
                    strncasecmp(after, "10bit", 5) == 0 ||
                    strncasecmp(after, "8bit", 4) == 0) {
                    /* Truncate at year */
                    while (p > title && (*(p - 1) == ' ' || *(p - 1) == '.'))
                        p--;
                    *p = '\0';
                    return y;
                }
            }
        }
        p++;
    }
    return 0;
}

/* ================================================================
 *  Subtitle Probing (embedded + external files)
 * ================================================================ */

#define MAX_SUBTITLES 32
#define MAX_EXTERNAL_SUBS 16

/* Supported external subtitle extensions */
static const char *subtitle_extensions[] = {
    ".srt", ".ass", ".ssa", ".sub", ".idx", ".vtt", ".smi", NULL
};

typedef struct {
    /* Embedded subtitles from main video file */
    int stream_idx[MAX_SUBTITLES];  /* Subtitle-stream-relative indices */
    int is_english[MAX_SUBTITLES];  /* 1=English, 0=Norwegian */
    int count;

    /* External subtitle files */
    char ext_files[MAX_EXTERNAL_SUBS][4096];
    int ext_is_english[MAX_EXTERNAL_SUBS];
    int ext_count;
} SubtitleProbe;

/* Check if language tag matches English */
static int is_english_lang(const char *lang) {
    if (!lang || !*lang) return 0;
    return (strcasecmp(lang, "eng") == 0 ||
            strcasecmp(lang, "en") == 0 ||
            strcasecmp(lang, "english") == 0);
}

/* Check if language tag matches Norwegian */
static int is_norwegian_lang(const char *lang) {
    if (!lang || !*lang) return 0;
    return (strcasecmp(lang, "nor") == 0 ||
            strcasecmp(lang, "nob") == 0 ||
            strcasecmp(lang, "nno") == 0 ||
            strcasecmp(lang, "no") == 0 ||
            strcasecmp(lang, "nb") == 0 ||
            strcasecmp(lang, "nn") == 0 ||
            strcasecmp(lang, "norwegian") == 0);
}

/* Check if filename/path contains language hints */
static int detect_lang_from_name(const char *name, int *is_eng, int *is_nor) {
    *is_eng = 0;
    *is_nor = 0;

    if (strcasestr(name, ".eng.") || strcasestr(name, ".en.") ||
        strcasestr(name, "_eng.") || strcasestr(name, "_en.") ||
        strcasestr(name, ".english.") || strcasestr(name, "_english.") ||
        strcasestr(name, "[eng]") || strcasestr(name, "[en]") ||
        strcasestr(name, "(eng)") || strcasestr(name, "(en)")) {
        *is_eng = 1;
        return 1;
    }

    if (strcasestr(name, ".nor.") || strcasestr(name, ".no.") ||
        strcasestr(name, ".nb.") || strcasestr(name, ".nn.") ||
        strcasestr(name, "_nor.") || strcasestr(name, "_no.") ||
        strcasestr(name, "_nb.") || strcasestr(name, "_nn.") ||
        strcasestr(name, ".norwegian.") || strcasestr(name, "_norwegian.") ||
        strcasestr(name, ".norsk.") || strcasestr(name, "_norsk.") ||
        strcasestr(name, "[nor]") || strcasestr(name, "[no]") ||
        strcasestr(name, "(nor)") || strcasestr(name, "(no)")) {
        *is_nor = 1;
        return 1;
    }

    return 0;
}

/* Check if file is a subtitle file */
static int is_subtitle_file(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return 0;
    for (int i = 0; subtitle_extensions[i]; i++) {
        if (strcasecmp(ext, subtitle_extensions[i]) == 0) return 1;
    }
    return 0;
}

/* Find external subtitle files that match the video filename */
static void find_external_subtitles(const char *video_path, SubtitleProbe *info) {
    /* Get directory and base name of video file */
    char dir[4096];
    char base[512];
    strncpy(dir, video_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (slash) {
        strncpy(base, slash + 1, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        *slash = '\0';
    } else {
        strncpy(base, dir, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
        strcpy(dir, ".");
    }

    /* Remove video extension from base name */
    char *ext = strrchr(base, '.');
    if (ext) *ext = '\0';

    /* Scan directory for matching subtitle files */
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && info->ext_count < MAX_EXTERNAL_SUBS) {
        if (entry->d_name[0] == '.') continue;
        if (!is_subtitle_file(entry->d_name)) continue;

        /* Check if subtitle filename starts with the video base name */
        if (strncasecmp(entry->d_name, base, strlen(base)) != 0) continue;

        /* Detect language from filename */
        int is_eng = 0, is_nor = 0;
        detect_lang_from_name(entry->d_name, &is_eng, &is_nor);

        /* Only include English or Norwegian subtitles */
        if (!is_eng && !is_nor) continue;

        /* Add to external subtitle list */
        snprintf(info->ext_files[info->ext_count], sizeof(info->ext_files[0]),
                 "%s/%s", dir, entry->d_name);
        info->ext_is_english[info->ext_count] = is_eng;
        info->ext_count++;

        printf("  Found external subtitle: %s (%s)\n",
               entry->d_name, is_eng ? "English" : "Norwegian");
    }

    closedir(d);
}

static void probe_subtitles(const char *filepath, SubtitleProbe *info) {
    memset(info, 0, sizeof(*info));

    /* First, find external subtitle files */
    find_external_subtitles(filepath, info);

    /* Then probe embedded subtitles in the video file */
    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) < 0) return;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return;
    }

    int sub_idx = 0;
    for (unsigned i = 0; i < fmt_ctx->nb_streams && info->count < MAX_SUBTITLES; i++) {
        AVCodecParameters *cp = fmt_ctx->streams[i]->codecpar;
        if (cp->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;

        /* Get language from stream metadata */
        const char *lang = NULL;
        AVDictionaryEntry *tag = av_dict_get(fmt_ctx->streams[i]->metadata, "language", NULL, 0);
        if (tag) lang = tag->value;

        /* Also check title for language hints if no language tag */
        const char *title = NULL;
        AVDictionaryEntry *title_tag = av_dict_get(fmt_ctx->streams[i]->metadata, "title", NULL, 0);
        if (title_tag) title = title_tag->value;

        int is_eng = is_english_lang(lang);
        int is_nor = is_norwegian_lang(lang);

        /* If no language tag, check title for hints */
        if (!is_eng && !is_nor && title) {
            if (strcasestr(title, "english") || strcasestr(title, "eng"))
                is_eng = 1;
            else if (strcasestr(title, "norsk") || strcasestr(title, "norwegian") ||
                     strcasestr(title, "nor"))
                is_nor = 1;
        }

        if (is_eng || is_nor) {
            info->stream_idx[info->count] = sub_idx;
            info->is_english[info->count] = is_eng;
            info->count++;
        }

        sub_idx++;
    }

    avformat_close_input(&fmt_ctx);
}

/* ================================================================
 *  Audio Probing
 * ================================================================ */

typedef struct {
    int best_audio_idx;     /* Audio-stream-relative index of best (most channels) */
    int best_channels;
    int nor_audio_idx;      /* Audio-stream-relative index of Norwegian audio (-1 = none) */
    int nor_channels;
    int atmos_idx;          /* Audio-stream-relative index of TrueHD/Atmos (-1 = none) */
} AudioProbe;

static void probe_audio(const char *filepath, AudioProbe *info) {
    info->best_audio_idx = 0;
    info->best_channels = 0;
    info->nor_audio_idx = -1;
    info->nor_channels = 0;
    info->atmos_idx = -1;

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) < 0) return;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return;
    }

    int audio_idx = 0;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVCodecParameters *cp = fmt_ctx->streams[i]->codecpar;
        if (cp->codec_type != AVMEDIA_TYPE_AUDIO) continue;

        int ch = cp->ch_layout.nb_channels;

        /* Best audio = most channels */
        if (ch > info->best_channels) {
            info->best_channels = ch;
            info->best_audio_idx = audio_idx;
        }

        /* Norwegian audio: pick the one with most channels */
        AVDictionaryEntry *lang_tag = av_dict_get(
            fmt_ctx->streams[i]->metadata, "language", NULL, 0);
        if (lang_tag && is_norwegian_lang(lang_tag->value)) {
            if (ch > info->nor_channels) {
                info->nor_channels = ch;
                info->nor_audio_idx = audio_idx;
            }
        }

        /* Atmos detection: TrueHD is the primary Atmos carrier (Blu-ray) */
        if (cp->codec_id == AV_CODEC_ID_TRUEHD) {
            info->atmos_idx = audio_idx;
        }

        audio_idx++;
    }

    avformat_close_input(&fmt_ctx);
}

/* ================================================================
 *  Video / HDR Probing
 * ================================================================ */

typedef struct {
    int is_hdr;
    int color_primaries;    /* AVCOL_PRI_* */
    int color_trc;          /* AVCOL_TRC_* */
    int color_space;        /* AVCOL_SPC_* */
    int has_master_display;
    char master_display[256];  /* x265 format: G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min) */
    int has_cll;
    int max_cll;
    int max_fall;
    int width;
    int height;
    int bit_depth;          /* Source bit depth (8, 10, 12, etc.) */
} VideoProbe;

static void probe_video(const char *filepath, VideoProbe *info) {
    memset(info, 0, sizeof(*info));

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) < 0) return;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        avformat_close_input(&fmt_ctx);
        return;
    }

    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        AVCodecParameters *cp = fmt_ctx->streams[i]->codecpar;
        if (cp->codec_type != AVMEDIA_TYPE_VIDEO) continue;

        /* Capture resolution */
        info->width = cp->width;
        info->height = cp->height;

        /* Detect bit depth from pixel format */
        const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(cp->format);
        if (pix_desc) {
            info->bit_depth = pix_desc->comp[0].depth;
        } else {
            info->bit_depth = 8;  /* Default fallback */
        }

        /* Check for HDR via color properties */
        if (cp->color_primaries == AVCOL_PRI_BT2020 &&
            (cp->color_trc == AVCOL_TRC_SMPTE2084 ||
             cp->color_trc == AVCOL_TRC_ARIB_STD_B67)) {
            info->is_hdr = 1;
            info->color_primaries = cp->color_primaries;
            info->color_trc = cp->color_trc;
            info->color_space = cp->color_space;
        }

        /* Check for mastering display metadata and content light level
         * via codecpar coded_side_data (FFmpeg 6.1+) */
        for (int j = 0; j < cp->nb_coded_side_data; j++) {
            AVPacketSideData *sd = &cp->coded_side_data[j];

            if (sd->type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA &&
                sd->size >= (int)sizeof(AVMasteringDisplayMetadata)) {
                const AVMasteringDisplayMetadata *mdm =
                    (const AVMasteringDisplayMetadata *)sd->data;
                if (mdm->has_primaries && mdm->has_luminance) {
                    info->has_master_display = 1;
                    /* x265 format: G(gx,gy)B(bx,by)R(rx,ry)WP(wpx,wpy)L(maxL,minL)
                     * Chromaticity in 0.00002 units (multiply by 50000)
                     * Luminance in 0.0001 cd/m^2 units (multiply by 10000) */
                    snprintf(info->master_display, sizeof(info->master_display),
                        "G(%d,%d)B(%d,%d)R(%d,%d)WP(%d,%d)L(%d,%d)",
                        (int)(av_q2d(mdm->display_primaries[1][0]) * 50000),
                        (int)(av_q2d(mdm->display_primaries[1][1]) * 50000),
                        (int)(av_q2d(mdm->display_primaries[2][0]) * 50000),
                        (int)(av_q2d(mdm->display_primaries[2][1]) * 50000),
                        (int)(av_q2d(mdm->display_primaries[0][0]) * 50000),
                        (int)(av_q2d(mdm->display_primaries[0][1]) * 50000),
                        (int)(av_q2d(mdm->white_point[0]) * 50000),
                        (int)(av_q2d(mdm->white_point[1]) * 50000),
                        (int)(av_q2d(mdm->max_luminance) * 10000),
                        (int)(av_q2d(mdm->min_luminance) * 10000));
                }
            }

            if (sd->type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL &&
                sd->size >= (int)sizeof(AVContentLightMetadata)) {
                const AVContentLightMetadata *cll =
                    (const AVContentLightMetadata *)sd->data;
                info->has_cll = 1;
                info->max_cll = cll->MaxCLL;
                info->max_fall = cll->MaxFALL;
            }
        }

        break; /* Only first video stream */
    }

    avformat_close_input(&fmt_ctx);
}

/* ================================================================
 *  Filename / TV Info Parsing
 * ================================================================ */

/* Parse SxxExx pattern manually (case-insensitive) */
static int parse_sxxexx(const char *s, int *season, int *episode) {
    if ((s[0] != 'S' && s[0] != 's') || !isdigit(s[1]))
        return 0;

    /* Parse season number */
    int sn = 0;
    int i = 1;
    while (isdigit(s[i])) {
        sn = sn * 10 + (s[i] - '0');
        i++;
    }

    /* Expect 'E' or 'e' */
    if (s[i] != 'E' && s[i] != 'e')
        return 0;
    i++;

    /* Parse episode number */
    if (!isdigit(s[i]))
        return 0;

    int ep = 0;
    while (isdigit(s[i])) {
        ep = ep * 10 + (s[i] - '0');
        i++;
    }

    *season = sn;
    *episode = ep;
    return 1;
}

static int parse_tv_episode(const char *filepath, char *show_name, int *season, int *episode, int *year) {
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;

    char name[512];
    strncpy(name, base, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    *year = 0;
    *season = 0;
    *episode = 0;

    /* Remove only the final extension (e.g., .mkv) */
    char *ext = strrchr(name, '.');
    if (ext) *ext = '\0';

    /* Find SxxExx or NxNN pattern */
    char *s = name;
    while (*s) {
        /* Try SxxExx pattern (case-insensitive) */
        if ((s[0] == 'S' || s[0] == 's') && isdigit(s[1])) {
            int sn = 0, ep = 0;
            if (parse_sxxexx(s, &sn, &ep)) {
                *season = sn;
                *episode = ep;
                size_t len = s - name;
                if (len > 0) {
                    strncpy(show_name, name, len);
                    show_name[len] = '\0';
                    /* Clean separators */
                    for (char *p = show_name; *p; p++) {
                        if (*p == '.' || *p == '_') *p = ' ';
                    }
                    /* Trim */
                    size_t sl = strlen(show_name);
                    while (sl > 0 && show_name[sl - 1] == ' ') show_name[--sl] = '\0';
                    /* Extract trailing year if present (e.g., "MacGyver 1985") */
                    sl = strlen(show_name);
                    if (sl >= 5) {
                        int y = atoi(show_name + sl - 4);
                        if (y >= 1900 && y <= 2100 && show_name[sl - 5] == ' ') {
                            *year = y;
                            show_name[sl - 5] = '\0';
                        }
                    }
                }
                printf("  Parsed TV: \"%s\" S%02dE%02d (year=%d) from \"%s\"\n",
                       show_name, *season, *episode, *year, base);
                return 1;
            }
        }
        /* Try NxNN pattern (e.g., 1x05) */
        if (isdigit(s[0]) && (s[1] == 'x' || s[1] == 'X') && isdigit(s[2])) {
            int sn = 0, ep = 0;
            if (sscanf(s, "%d%*[xX]%d", &sn, &ep) == 2) {
                *season = sn;
                *episode = ep;
                size_t len = s - name;
                if (len > 0) {
                    strncpy(show_name, name, len);
                    show_name[len] = '\0';
                    for (char *p = show_name; *p; p++) {
                        if (*p == '.' || *p == '_') *p = ' ';
                    }
                    size_t sl = strlen(show_name);
                    while (sl > 0 && show_name[sl - 1] == ' ') show_name[--sl] = '\0';
                    /* Extract trailing year if present */
                    sl = strlen(show_name);
                    if (sl >= 5) {
                        int y = atoi(show_name + sl - 4);
                        if (y >= 1900 && y <= 2100 && show_name[sl - 5] == ' ') {
                            *year = y;
                            show_name[sl - 5] = '\0';
                        }
                    }
                }
                printf("  Parsed TV: \"%s\" S%02dE%02d (year=%d) from \"%s\"\n",
                       show_name, *season, *episode, *year, base);
                return 1;
            }
        }
        s++;
    }

    return 0;
}

static void extract_movie_title(const char *filepath, char *title, size_t max) {
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;

    strncpy(title, base, max - 1);
    title[max - 1] = '\0';

    /* Remove extension */
    char *ext = strrchr(title, '.');
    if (ext) *ext = '\0';

    /* Clean separators to spaces */
    for (char *p = title; *p; p++) {
        if (*p == '.' || *p == '_') *p = ' ';
    }

    /* Trim */
    size_t len = strlen(title);
    while (len > 0 && title[len - 1] == ' ') title[--len] = '\0';
}

/* ================================================================
 *  Job Collection
 * ================================================================ */

typedef struct {
    TranscodeJob *jobs;
    int count;
    int capacity;
} JobList;

static void joblist_add(JobList *list, TranscodeJob *job) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity ? list->capacity * 2 : 64;
        list->jobs = realloc(list->jobs, list->capacity * sizeof(TranscodeJob));
    }
    list->jobs[list->count++] = *job;
}

static void collect_from_dir(const char *path, JobList *list) {
    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Skip the output directory */
            if (is_in_output_dir(fullpath)) continue;

            /* Blu-ray disc copy: extract main feature, skip menus */
            if (is_bluray_dir(fullpath)) {
                char main_feature[4096];
                if (find_bluray_main_feature(fullpath, main_feature, sizeof(main_feature))) {
                    TranscodeJob job = {0};
                    strncpy(job.filepath, main_feature, sizeof(job.filepath) - 1);
                    strncpy(job.source_dir, fullpath, sizeof(job.source_dir) - 1);
                    job.mtime = st.st_mtime;

                    /* Parse title from the Blu-ray folder name */
                    char show_name[256] = {0};
                    int season = 0, episode = 0, year = 0;

                    if (parse_tv_episode(fullpath, show_name, &season, &episode, &year)) {
                        job.type = 2;
                        strncpy(job.show_name, show_name, sizeof(job.show_name) - 1);
                        job.season = season;
                        job.episode = episode;
                        job.year = year;
                        if (year > 0)
                            snprintf(job.title, sizeof(job.title), "%s %d S%02dE%02d",
                                     show_name, year, season, episode);
                        else
                            snprintf(job.title, sizeof(job.title), "%s S%02dE%02d",
                                     show_name, season, episode);
                    } else {
                        job.type = 0;
                        extract_movie_title(fullpath, job.title, sizeof(job.title));
                    }

                    joblist_add(list, &job);
                }
                continue; /* Don't recurse into Blu-ray structure */
            }

            collect_from_dir(fullpath, list);
        } else if (S_ISREG(st.st_mode) && is_video_file(fullpath)) {
            /* Skip already converted files */
            if (is_already_converted(fullpath)) continue;
            if (is_in_output_dir(fullpath)) continue;

            TranscodeJob job = {0};
            strncpy(job.filepath, fullpath, sizeof(job.filepath) - 1);
            job.mtime = st.st_mtime;

            char show_name[256] = {0};
            int season = 0, episode = 0, year = 0;

            if (parse_tv_episode(fullpath, show_name, &season, &episode, &year)) {
                job.type = 2; /* MEDIA_TYPE_EPISODE */
                strncpy(job.show_name, show_name, sizeof(job.show_name) - 1);
                job.season = season;
                job.episode = episode;
                job.year = year;
                if (year > 0)
                    snprintf(job.title, sizeof(job.title), "%s %d S%02dE%02d",
                             show_name, year, season, episode);
                else
                    snprintf(job.title, sizeof(job.title), "%s S%02dE%02d",
                             show_name, season, episode);
            } else {
                job.type = 0; /* MEDIA_TYPE_MOVIE */
                extract_movie_title(fullpath, job.title, sizeof(job.title));
            }

            joblist_add(list, &job);
        }
    }

    closedir(dir);
}

/* After job collection, merge multi-part BDR entries into single jobs.
 * Part 1's main feature becomes filepath, part 2's becomes filepath2.
 * Both source_dir entries are kept for cleanup after conversion. */
static void merge_multipart_bluray_jobs(JobList *list) {
    for (int i = 0; i < list->count; i++) {
        if (!list->jobs[i].source_dir[0]) continue;  /* Only BDR jobs */
        if (list->jobs[i].filepath2[0]) continue;     /* Already merged */

        char base_i[4096];
        int part_i = detect_bluray_part(list->jobs[i].source_dir, base_i, sizeof(base_i));
        if (part_i == 0) continue;

        for (int j = i + 1; j < list->count; j++) {
            if (!list->jobs[j].source_dir[0]) continue;
            if (list->jobs[j].filepath2[0]) continue;

            char base_j[4096];
            int part_j = detect_bluray_part(list->jobs[j].source_dir, base_j, sizeof(base_j));
            if (part_j == 0 || part_j == part_i) continue;

            if (strcasecmp(base_i, base_j) != 0) continue;

            /* Found matching pair — ensure jobs[i] holds part 1 */
            if (part_i == 2) {
                /* Swap: make jobs[i] the part-1 job */
                char tmp[4096];
                strncpy(tmp, list->jobs[i].filepath, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
                strncpy(list->jobs[i].filepath, list->jobs[j].filepath, sizeof(list->jobs[i].filepath) - 1);
                strncpy(list->jobs[j].filepath, tmp, sizeof(list->jobs[j].filepath) - 1);

                strncpy(tmp, list->jobs[i].source_dir, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
                strncpy(list->jobs[i].source_dir, list->jobs[j].source_dir, sizeof(list->jobs[i].source_dir) - 1);
                strncpy(list->jobs[j].source_dir, tmp, sizeof(list->jobs[j].source_dir) - 1);
            }

            /* Copy part 2 into the combined job */
            strncpy(list->jobs[i].filepath2, list->jobs[j].filepath,
                    sizeof(list->jobs[i].filepath2) - 1);
            strncpy(list->jobs[i].source_dir2, list->jobs[j].source_dir,
                    sizeof(list->jobs[i].source_dir2) - 1);

            /* Use base name (without part indicator) for the title */
            extract_movie_title(base_i, list->jobs[i].title, sizeof(list->jobs[i].title));

            printf("  Multi-part BDR: \"%s\" (2 parts merged)\n", list->jobs[i].title);

            /* Remove the consumed job by shifting */
            for (int k = j; k < list->count - 1; k++)
                list->jobs[k] = list->jobs[k + 1];
            list->count--;
            break;
        }
    }
}

/* Sort by mtime descending (newest first) */
static int mtime_compare_desc(const void *a, const void *b) {
    const TranscodeJob *ja = (const TranscodeJob *)a;
    const TranscodeJob *jb = (const TranscodeJob *)b;
    if (jb->mtime > ja->mtime) return 1;
    if (jb->mtime < ja->mtime) return -1;
    return 0;
}

/* Sort episodes by season -> episode (for within a series batch) */
static int episode_compare(const void *a, const void *b) {
    const TranscodeJob *ja = (const TranscodeJob *)a;
    const TranscodeJob *jb = (const TranscodeJob *)b;
    if (ja->season != jb->season) return ja->season - jb->season;
    return ja->episode - jb->episode;
}

/* ================================================================
 *  Disk Selection (3% reserve)
 * ================================================================ */

/* Cached show->disk mapping for current transcoder run */
typedef struct {
    char show_name[256];
    int disk_idx;
} ShowDiskMapping;

static ShowDiskMapping *disk_map = NULL;
static int disk_map_count = 0;

static void disk_map_reset(void) {
    free(disk_map);
    disk_map = NULL;
    disk_map_count = 0;
}

/* Select output disk for a job.
 * Rules:
 *   1. If show already exists on a disk, use that disk
 *   2. Otherwise pick first disk with >= 3% free space
 *   Returns disk index or -1 if no disk has space */
static int select_output_disk(const char *show_name, int is_episode) {
    /* Check cached mapping for episodes */
    if (is_episode && show_name[0]) {
        for (int i = 0; i < disk_map_count; i++) {
            if (strcasecmp(disk_map[i].show_name, show_name) == 0)
                return disk_map[i].disk_idx;
        }

        /* Check if show already exists on any disk */
        for (int d = 0; d < server_config.converted_path_count; d++) {
            char check_path[4096];
            char norm[256];
            normalize_title(show_name, norm, sizeof(norm));
            snprintf(check_path, sizeof(check_path), "%s/nixly_ready_media/TV/%s",
                     server_config.converted_paths[d], norm);
            struct stat st;
            if (stat(check_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                /* Cache this mapping */
                disk_map = realloc(disk_map, (disk_map_count + 1) * sizeof(ShowDiskMapping));
                strncpy(disk_map[disk_map_count].show_name, show_name, 255);
                disk_map[disk_map_count].show_name[255] = '\0';
                disk_map[disk_map_count].disk_idx = d;
                disk_map_count++;
                printf("  Disk select: Show \"%s\" already on disk %d (%s)\n",
                       show_name, d, server_config.converted_paths[d]);
                return d;
            }
        }
    }

    /* Pick first disk with >= 3% free space */
    for (int d = 0; d < server_config.converted_path_count; d++) {
        struct statvfs vfs;
        if (statvfs(server_config.converted_paths[d], &vfs) != 0) continue;

        unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;

        if (total == 0) continue;
        double pct_free = (double)avail / (double)total * 100.0;

        if (pct_free >= 3.0) {
            printf("  Disk select: Using disk %d (%s) - %.1f%% free\n",
                   d, server_config.converted_paths[d], pct_free);

            /* Cache for episodes */
            if (is_episode && show_name[0]) {
                disk_map = realloc(disk_map, (disk_map_count + 1) * sizeof(ShowDiskMapping));
                strncpy(disk_map[disk_map_count].show_name, show_name, 255);
                disk_map[disk_map_count].show_name[255] = '\0';
                disk_map[disk_map_count].disk_idx = d;
                disk_map_count++;
            }
            return d;
        } else {
            printf("  Disk select: Disk %d (%s) too full (%.1f%% free, need 3%%)\n",
                   d, server_config.converted_paths[d], pct_free);
        }
    }

    return -1; /* No disk has space */
}

/* ================================================================
 *  TMDB Verification (single job)
 * ================================================================ */

/* Returns 1 if found in TMDB, 0 if not */
static int verify_tmdb_single(TranscodeJob *job) {
    if (job->type == 2) {
        TmdbTvShow *show = tmdb_search_tvshow(job->show_name, job->year);
        if (show) {
            job->tmdb_verified = 1;
            tmdb_free_tvshow(show);
            return 1;
        }
        job->tmdb_verified = -1;
        return 0;
    } else {
        char search_title[256];
        strncpy(search_title, job->title, sizeof(search_title) - 1);
        search_title[sizeof(search_title) - 1] = '\0';
        int year = strip_year_from_title(search_title);

        TmdbMovie *movie = tmdb_search_movie(search_title, year);
        if (movie) {
            job->tmdb_verified = 1;
            tmdb_free_movie(movie);
            return 1;
        }
        job->tmdb_verified = -1;
        return 0;
    }
}

/* ================================================================
 *  FFmpeg Execution  (DASH output)
 * ================================================================ */

/* Probe total duration of input file (seconds) */
static double probe_duration(const char *filepath) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, filepath, NULL, NULL) < 0) return 0;
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return 0;
    }
    double dur = (fmt->duration > 0) ? (double)fmt->duration / AV_TIME_BASE : 0;
    avformat_close_input(&fmt);
    return dur;
}

/* Probe duration of a partially-encoded tmp file using ffprobe.
 * Returns duration in seconds, 0 on failure. */
static double probe_tmp_duration(const char *filepath) {
    int pipefd[2];
    if (pipe(pipefd) < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return 0;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        /* Redirect stderr to /dev/null */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp("ffprobe", "ffprobe",
               "-v", "error",
               "-show_entries", "format=duration",
               "-of", "default=nw=1:nk=1",
               filepath, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    char buf[64] = {0};
    ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 0;

    return atof(buf);
}

/* Concatenate two files with stream copy using ffmpeg concat demuxer.
 * Returns 0 on success, -1 on failure. */
static int concat_copy(const char *part1, const char *part2, const char *output) {
    char list_path[4096];
    snprintf(list_path, sizeof(list_path), "%s.concat.txt", part1);

    FILE *f = fopen(list_path, "w");
    if (!f) {
        fprintf(stderr, "  concat_copy: cannot create list file %s\n", list_path);
        return -1;
    }
    fprintf(f, "file '%s'\nfile '%s'\n", part1, part2);
    fclose(f);

    printf("  Concat: %s + %s → %s\n", part1, part2, output);

    pid_t pid = fork();
    if (pid < 0) {
        unlink(list_path);
        return -1;
    }

    if (pid == 0) {
        /* Redirect stderr to /dev/null to avoid noise */
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
        /* Clean up parts */
        unlink(part1);
        unlink(part2);
        return 0;
    }

    fprintf(stderr, "  concat_copy: ffmpeg exited with status %d\n",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
}

static int run_ffmpeg(const char *input, const char *input2,
                      const char *output, double ss_offset,
                      AudioProbe *audio, VideoProbe *video,
                      SubtitleProbe *subs) {
    const char *argv[192];
    int argc = 0;
    char concat_list_path[256] = {0};
    char ss_buf[32];

    argv[argc++] = "ffmpeg";
    argv[argc++] = "-y";
    argv[argc++] = "-nostdin";
    argv[argc++] = "-progress";
    argv[argc++] = "pipe:2";

    /* Seek offset for resume (placed before -i for input-level seeking) */
    if (ss_offset > 0) {
        snprintf(ss_buf, sizeof(ss_buf), "%.3f", ss_offset);
        argv[argc++] = "-ss";
        argv[argc++] = ss_buf;
        printf("  Resume: seeking to %.1f seconds\n", ss_offset);
    }

    /* Multi-part: use concat demuxer to join parts seamlessly */
    if (input2 && input2[0]) {
        snprintf(concat_list_path, sizeof(concat_list_path),
                 "/tmp/nixly_concat_%d.txt", (int)getpid());
        FILE *cl = fopen(concat_list_path, "w");
        if (!cl) {
            fprintf(stderr, "  Error: cannot create concat list %s\n", concat_list_path);
            return -1;
        }
        fprintf(cl, "file '%s'\nfile '%s'\n", input, input2);
        fclose(cl);

        argv[argc++] = "-f";
        argv[argc++] = "concat";
        argv[argc++] = "-safe";
        argv[argc++] = "0";
        argv[argc++] = "-i";
        argv[argc++] = concat_list_path;

        printf("  Concat: part1 + part2 via demuxer\n");
    } else {
        argv[argc++] = "-i";
        argv[argc++] = input;
    }

    /* Add external subtitle files as additional inputs */
    /* Input 0 = video, Input 1+ = external subtitles */
    for (int i = 0; i < subs->ext_count; i++) {
        argv[argc++] = "-i";
        argv[argc++] = subs->ext_files[i];
    }

    /* Video: x265 CRF 14, 10-bit */
    argv[argc++] = "-map";
    argv[argc++] = "0:v:0";
    argv[argc++] = "-c:v";
    argv[argc++] = "libx265";
    argv[argc++] = "-crf";
    argv[argc++] = "14";
    argv[argc++] = "-preset";
    argv[argc++] = "medium";
    argv[argc++] = "-pix_fmt";
    argv[argc++] = "yuv420p10le";

    /* Build x265-params: closed-GOP keyint 48 + HDR if present
     * no-open-gop for clean seek points aligned to cluster boundaries */
    static char x265_params[1024];
    int pos = 0;
    pos += snprintf(x265_params + pos, sizeof(x265_params) - pos,
                    "keyint=48:min-keyint=48:no-open-gop=1");

    if (video->is_hdr) {
        const char *transfer = (video->color_trc == AVCOL_TRC_SMPTE2084)
                               ? "smpte2084" : "arib-std-b67";

        if (video->color_trc == AVCOL_TRC_SMPTE2084) {
            pos += snprintf(x265_params + pos, sizeof(x265_params) - pos,
                ":hdr-opt=1:repeat-headers=1:colorprim=bt2020:transfer=%s:colormatrix=bt2020nc",
                transfer);
        } else {
            /* HLG */
            pos += snprintf(x265_params + pos, sizeof(x265_params) - pos,
                ":repeat-headers=1:colorprim=bt2020:transfer=%s:colormatrix=bt2020nc",
                transfer);
        }

        if (video->has_master_display) {
            pos += snprintf(x265_params + pos, sizeof(x265_params) - pos,
                ":master-display=%s", video->master_display);
        }
        if (video->has_cll) {
            pos += snprintf(x265_params + pos, sizeof(x265_params) - pos,
                ":max-cll=%d,%d", video->max_cll, video->max_fall);
        }
    }

    argv[argc++] = "-x265-params";
    argv[argc++] = x265_params;

    /* Color properties for HDR passthrough */
    if (video->is_hdr) {
        argv[argc++] = "-color_primaries";
        argv[argc++] = "bt2020";
        argv[argc++] = "-color_trc";
        argv[argc++] = (video->color_trc == AVCOL_TRC_SMPTE2084)
                        ? "smpte2084" : "arib-std-b67";
        argv[argc++] = "-colorspace";
        argv[argc++] = "bt2020nc";
    }

    /* Audio track 1: Original (best) as FLAC lossless */
    int audio_out_idx = 0;
    char flac_map[32];
    snprintf(flac_map, sizeof(flac_map), "0:a:%d",
             audio->best_audio_idx >= 0 ? audio->best_audio_idx : 0);
    argv[argc++] = "-map";
    argv[argc++] = flac_map;

    static char flac_codec_spec[32];
    snprintf(flac_codec_spec, sizeof(flac_codec_spec), "-c:a:%d", audio_out_idx);
    argv[argc++] = flac_codec_spec;
    argv[argc++] = "flac";

    static char flac_title[64];
    int ch = audio->best_channels;
    if (ch >= 8)
        snprintf(flac_title, sizeof(flac_title), "title=Lossless 7.1");
    else if (ch >= 6)
        snprintf(flac_title, sizeof(flac_title), "title=Lossless 5.1");
    else if (ch >= 3)
        snprintf(flac_title, sizeof(flac_title), "title=Lossless %d.1", ch - 1);
    else if (ch == 2)
        snprintf(flac_title, sizeof(flac_title), "title=Lossless 2.0");
    else
        snprintf(flac_title, sizeof(flac_title), "title=Lossless 1.0");
    static char flac_meta_spec[32];
    snprintf(flac_meta_spec, sizeof(flac_meta_spec), "-metadata:s:a:%d", audio_out_idx);
    argv[argc++] = flac_meta_spec;
    argv[argc++] = flac_title;
    audio_out_idx++;

    /* Audio track 2: Norwegian audio as FLAC (if available and not same as original) */
    static char nor_map[32];
    static char nor_codec_spec[32];
    static char nor_meta_spec[32];
    static char nor_title[64];
    if (audio->nor_audio_idx >= 0 && audio->nor_audio_idx != audio->best_audio_idx) {
        snprintf(nor_map, sizeof(nor_map), "0:a:%d", audio->nor_audio_idx);
        argv[argc++] = "-map";
        argv[argc++] = nor_map;

        snprintf(nor_codec_spec, sizeof(nor_codec_spec), "-c:a:%d", audio_out_idx);
        argv[argc++] = nor_codec_spec;
        argv[argc++] = "flac";

        int nch = audio->nor_channels;
        if (nch >= 8)
            snprintf(nor_title, sizeof(nor_title), "title=Norsk 7.1");
        else if (nch >= 6)
            snprintf(nor_title, sizeof(nor_title), "title=Norsk 5.1");
        else if (nch >= 3)
            snprintf(nor_title, sizeof(nor_title), "title=Norsk %d.1", nch - 1);
        else if (nch == 2)
            snprintf(nor_title, sizeof(nor_title), "title=Norsk 2.0");
        else
            snprintf(nor_title, sizeof(nor_title), "title=Norsk 1.0");
        snprintf(nor_meta_spec, sizeof(nor_meta_spec), "-metadata:s:a:%d", audio_out_idx);
        argv[argc++] = nor_meta_spec;
        argv[argc++] = nor_title;
        audio_out_idx++;
    }

    /* Atmos (TrueHD) lossless passthrough if available */
    static char atmos_map[32];
    static char atmos_codec_spec[32];
    static char atmos_meta_spec[32];
    if (audio->atmos_idx >= 0) {
        snprintf(atmos_map, sizeof(atmos_map), "0:a:%d", audio->atmos_idx);
        argv[argc++] = "-map";
        argv[argc++] = atmos_map;

        snprintf(atmos_codec_spec, sizeof(atmos_codec_spec), "-c:a:%d", audio_out_idx);
        argv[argc++] = atmos_codec_spec;
        argv[argc++] = "copy";
        snprintf(atmos_meta_spec, sizeof(atmos_meta_spec), "-metadata:s:a:%d", audio_out_idx);
        argv[argc++] = atmos_meta_spec;
        argv[argc++] = "title=Atmos Passthrough";
        audio_out_idx++;
    }

    /* Subtitles: only English and Norwegian, with proper names
     * Includes both embedded subtitles (from input 0) and external files (inputs 1+) */
    static char sub_maps[MAX_SUBTITLES + MAX_EXTERNAL_SUBS][32];
    static char sub_titles[MAX_SUBTITLES + MAX_EXTERNAL_SUBS][64];
    static char meta_specs[MAX_SUBTITLES + MAX_EXTERNAL_SUBS][32];
    int total_subs = 0;

    /* Map embedded subtitles from main video (input 0) */
    for (int i = 0; i < subs->count; i++) {
        snprintf(sub_maps[total_subs], sizeof(sub_maps[0]), "0:s:%d", subs->stream_idx[i]);
        argv[argc++] = "-map";
        argv[argc++] = sub_maps[total_subs];
        total_subs++;
    }

    /* Map external subtitle files (inputs 1, 2, 3, ...) */
    for (int i = 0; i < subs->ext_count; i++) {
        snprintf(sub_maps[total_subs], sizeof(sub_maps[0]), "%d:0", i + 1);
        argv[argc++] = "-map";
        argv[argc++] = sub_maps[total_subs];
        total_subs++;
    }

    if (total_subs > 0) {
        argv[argc++] = "-c:s";
        argv[argc++] = "copy";

        /* Set title metadata for embedded subtitles */
        int sub_out_idx = 0;
        for (int i = 0; i < subs->count; i++) {
            snprintf(sub_titles[sub_out_idx], sizeof(sub_titles[0]),
                     "title=%s", subs->is_english[i] ? "English" : "Norwegian");
            snprintf(meta_specs[sub_out_idx], sizeof(meta_specs[0]),
                     "-metadata:s:s:%d", sub_out_idx);
            argv[argc++] = meta_specs[sub_out_idx];
            argv[argc++] = sub_titles[sub_out_idx];
            sub_out_idx++;
        }

        /* Set title metadata for external subtitles */
        for (int i = 0; i < subs->ext_count; i++) {
            snprintf(sub_titles[sub_out_idx], sizeof(sub_titles[0]),
                     "title=%s", subs->ext_is_english[i] ? "English" : "Norwegian");
            snprintf(meta_specs[sub_out_idx], sizeof(meta_specs[0]),
                     "-metadata:s:s:%d", sub_out_idx);
            argv[argc++] = meta_specs[sub_out_idx];
            argv[argc++] = sub_titles[sub_out_idx];
            sub_out_idx++;
        }
    }

    /* MKV muxer optimized for HTTP streaming:
     * - reserve_index_space: puts Cues (seek index) at the front of the file
     *   so clients can seek without reading the entire file first
     * - cluster_size_limit: smaller clusters = more frequent seek points */
    argv[argc++] = "-f";
    argv[argc++] = "matroska";
    argv[argc++] = "-reserve_index_space";
    argv[argc++] = "524288";
    argv[argc++] = "-cluster_size_limit";
    argv[argc++] = "2097152";
    argv[argc++] = "-cluster_time_limit";
    argv[argc++] = "2000";

    argv[argc++] = output;
    argv[argc] = NULL;

    /* Print command for debugging */
    printf("  ffmpeg");
    for (int i = 1; i < argc; i++) printf(" %s", argv[i]);
    printf("\n");

    /* Pipe for reading ffmpeg stderr (progress output) */
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        perror("transcoder: pipe");
        return -1;
    }

    /* Fork and exec */
    pid_t pid = fork();
    if (pid < 0) {
        perror("transcoder: fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: lowest possible priority — yield to everything else */
        nice(19);
        setpriority(PRIO_PROCESS, 0, 19);

        /* SCHED_IDLE: kernel treats this as below SCHED_OTHER */
        struct sched_param sp = { .sched_priority = 0 };
        sched_setscheduler(0, SCHED_IDLE, &sp);

        /* I/O scheduling: idle class (only uses disk when nobody else needs it) */
        /* ioprio_set(IOPRIO_WHO_PROCESS, 0, IOPRIO_PRIO_VALUE(IOPRIO_CLASS_IDLE, 0)) */
        syscall(SYS_ioprio_set, 1 /* WHO_PROCESS */, 0,
                (3 << 13) /* IOPRIO_CLASS_IDLE */);

        /* Redirect stderr to pipe */
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execvp("ffmpeg", (char *const *)argv);
        perror("transcoder: execvp ffmpeg");
        _exit(127);
    }

    /* Parent: read stderr for progress, track PID */
    close(pipefd[1]);
    tc.ffmpeg_pid = pid;

    FILE *fp = fdopen(pipefd[0], "r");
    if (fp) {
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            /* -progress pipe:2 outputs key=value lines
             * Older ffmpeg: out_time_ms= (value in microseconds despite name)
             * Newer ffmpeg (5.0+): out_time_us= (value in microseconds) */
            if (strncmp(line, "out_time_us=", 12) == 0 ||
                strncmp(line, "out_time_ms=", 12) == 0) {
                long long us = atoll(line + 12);
                if (us > 0) {
                    pthread_mutex_lock(&tc.lock);
                    tc.progress_secs = (double)us / 1000000.0;
                    pthread_mutex_unlock(&tc.lock);
                }
            }
            /* Print actual errors (not progress key=value lines) */
            else if (!strchr(line, '=') || strncmp(line, "[", 1) == 0) {
                fprintf(stderr, "  ffmpeg: %s", line);
            }
        }
        fclose(fp);
    } else {
        close(pipefd[0]);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            tc.ffmpeg_pid = 0;
            return -1;
        }
    }

    tc.ffmpeg_pid = 0;

    /* Clean up concat list temp file */
    if (concat_list_path[0])
        unlink(concat_list_path);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }

    fprintf(stderr, "  ffmpeg exited with status %d\n",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
}

/* ================================================================
 *  Source directory cleanup
 *  After a source file/dir is removed, walk up the directory tree
 *  and delete any parent that has no remaining video files or
 *  subdirectories (i.e. only leftover .nfo, .jpg, .txt etc.).
 *  Stops at unprocessed_path roots.
 * ================================================================ */

static void cleanup_source_dir(const char *removed_path) {
    char dir[4096];
    strncpy(dir, removed_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    while (1) {
        /* Get parent directory */
        char *slash = strrchr(dir, '/');
        if (!slash) return;
        *slash = '\0';

        /* Never delete an unprocessed_path root */
        int is_root = 0;
        for (int i = 0; i < server_config.unprocessed_path_count; i++) {
            if (strcmp(dir, server_config.unprocessed_paths[i]) == 0) {
                is_root = 1;
                break;
            }
        }
        if (is_root) return;

        /* Check if any video files or subdirectories remain */
        DIR *d = opendir(dir);
        if (!d) return;

        int has_content = 0;
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;

            char fullpath[4096];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, entry->d_name);

            struct stat st;
            if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
                has_content = 1; /* subdirectory still exists */
                break;
            }

            if (is_video_file(entry->d_name)) {
                has_content = 1; /* video file still exists */
                break;
            }
        }
        closedir(d);

        if (has_content) return;

        /* Only leftover junk remains — delete the whole directory */
        printf("  Cleaning up source dir: %s\n", dir);
        rmdir_r(dir);
        /* Continue checking parent */
    }
}

/* ================================================================
 *  Duplicate detection: check if episode/movie already exists
 *  in any nixly_ready_media directory. If so, delete the source.
 *  Returns 1 if duplicate found (source deleted), 0 otherwise.
 * ================================================================ */

static int check_and_delete_duplicate(TranscodeJob *job) {
    char norm[256];

    for (int d = 0; d < server_config.converted_path_count; d++) {
        char search_dir[4096];

        if (job->type == 2) {
            /* Episode: look for *S<ss>E<ee>*.x265.CRF14*.mkv in
             * <disk>/nixly_ready_media/TV/<show>[.year]/Season<N>/ */
            normalize_title(job->show_name, norm, sizeof(norm));

            if (job->year > 0)
                snprintf(search_dir, sizeof(search_dir),
                         "%s/nixly_ready_media/TV/%s.%d/Season%d",
                         server_config.converted_paths[d], norm, job->year, job->season);
            else
                snprintf(search_dir, sizeof(search_dir),
                         "%s/nixly_ready_media/TV/%s/Season%d",
                         server_config.converted_paths[d], norm, job->season);

            char pattern[32];
            snprintf(pattern, sizeof(pattern), ".S%02dE%02d.", job->season, job->episode);

            DIR *dir = opendir(search_dir);
            if (!dir) continue;

            struct dirent *entry;
            int found = 0;
            while ((entry = readdir(dir)) != NULL) {
                if (strstr(entry->d_name, pattern) &&
                    strstr(entry->d_name, ".x265.CRF14")) {
                    found = 1;
                    break;
                }
            }
            closedir(dir);

            if (found) {
                if (server_config.delete_after_conversion) {
                    printf("Transcoder: Duplicate — %s S%02dE%02d already converted, deleting source\n",
                           job->show_name, job->season, job->episode);
                    if (job->source_dir[0]) {
                        rmdir_r(job->source_dir);
                        cleanup_source_dir(job->source_dir);
                        if (job->source_dir2[0]) {
                            rmdir_r(job->source_dir2);
                            cleanup_source_dir(job->source_dir2);
                        }
                    } else {
                        unlink(job->filepath);
                        cleanup_source_dir(job->filepath);
                    }
                } else {
                    printf("Transcoder: Duplicate — %s S%02dE%02d already converted, keeping source\n",
                           job->show_name, job->season, job->episode);
                }
                return 1;
            }
        } else {
            /* Movie: look for <norm>[.year].*.x265.CRF14*.mkv in
             * <disk>/nixly_ready_media/Movies/ */
            char raw_title[256];
            strncpy(raw_title, job->title, sizeof(raw_title) - 1);
            raw_title[sizeof(raw_title) - 1] = '\0';
            strip_year_from_title(raw_title);
            normalize_title(raw_title, norm, sizeof(norm));

            snprintf(search_dir, sizeof(search_dir), "%s/nixly_ready_media/Movies",
                     server_config.converted_paths[d]);

            DIR *dir = opendir(search_dir);
            if (!dir) continue;

            size_t norm_len = strlen(norm);
            struct dirent *entry;
            int found = 0;
            while ((entry = readdir(dir)) != NULL) {
                if (strncmp(entry->d_name, norm, norm_len) == 0 &&
                    strstr(entry->d_name, ".x265.CRF14")) {
                    found = 1;
                    break;
                }
            }
            closedir(dir);

            if (found) {
                if (server_config.delete_after_conversion) {
                    printf("Transcoder: Duplicate — \"%s\" already converted, deleting source\n",
                           job->title);
                    if (job->source_dir[0]) {
                        rmdir_r(job->source_dir);
                        cleanup_source_dir(job->source_dir);
                        if (job->source_dir2[0]) {
                            rmdir_r(job->source_dir2);
                            cleanup_source_dir(job->source_dir2);
                        }
                    } else {
                        unlink(job->filepath);
                        cleanup_source_dir(job->filepath);
                    }
                } else {
                    printf("Transcoder: Duplicate — \"%s\" already converted, keeping source\n",
                           job->title);
                }
                return 1;
            }
        }
    }

    return 0;
}

/* Check if a job's output already exists in nixly_ready_media (without deleting source).
 * Used to filter the queue display on the status page. */
static int is_already_in_ready_media(TranscodeJob *job) {
    char norm[256];

    for (int d = 0; d < server_config.converted_path_count; d++) {
        char search_dir[4096];

        if (job->type == 2) {
            normalize_title(job->show_name, norm, sizeof(norm));

            if (job->year > 0)
                snprintf(search_dir, sizeof(search_dir),
                         "%s/nixly_ready_media/TV/%s.%d/Season%d",
                         server_config.converted_paths[d], norm, job->year, job->season);
            else
                snprintf(search_dir, sizeof(search_dir),
                         "%s/nixly_ready_media/TV/%s/Season%d",
                         server_config.converted_paths[d], norm, job->season);

            char pattern[32];
            snprintf(pattern, sizeof(pattern), ".S%02dE%02d.", job->season, job->episode);

            DIR *dir = opendir(search_dir);
            if (!dir) continue;

            struct dirent *entry;
            int found = 0;
            while ((entry = readdir(dir)) != NULL) {
                if (strstr(entry->d_name, pattern) &&
                    strstr(entry->d_name, ".x265.CRF14")) {
                    found = 1;
                    break;
                }
            }
            closedir(dir);
            if (found) return 1;
        } else {
            char raw_title[256];
            strncpy(raw_title, job->title, sizeof(raw_title) - 1);
            raw_title[sizeof(raw_title) - 1] = '\0';
            strip_year_from_title(raw_title);
            normalize_title(raw_title, norm, sizeof(norm));

            snprintf(search_dir, sizeof(search_dir), "%s/nixly_ready_media/Movies",
                     server_config.converted_paths[d]);

            DIR *dir = opendir(search_dir);
            if (!dir) continue;

            size_t norm_len = strlen(norm);
            struct dirent *entry;
            int found = 0;
            while ((entry = readdir(dir)) != NULL) {
                if (strncmp(entry->d_name, norm, norm_len) == 0 &&
                    strstr(entry->d_name, ".x265.CRF14")) {
                    found = 1;
                    break;
                }
            }
            closedir(dir);
            if (found) return 1;
        }
    }

    return 0;
}

/* ================================================================
 *  Process a single transcode job
 *  Returns:  0 = success
 *            1 = skipped (already exists / unstable / duplicate)
 *           -1 = ffmpeg error
 *           -2 = no disk space
 * ================================================================ */

static int process_single_job(TranscodeJob *job) {
    /* Check if already converted — if so, just delete the source */
    if (check_and_delete_duplicate(job)) {
        pthread_mutex_lock(&tc.lock);
        tc.total_jobs++;
        tc.completed_jobs++;
        pthread_mutex_unlock(&tc.lock);
        return 1;
    }

    pthread_mutex_lock(&tc.lock);
    tc.total_jobs++;
    strncpy(tc.current_file, job->title, sizeof(tc.current_file) - 1);
    pthread_mutex_unlock(&tc.lock);

    /* Select output disk */
    int disk_idx = select_output_disk(job->show_name, job->type == 2);
    if (disk_idx < 0) {
        fprintf(stderr, "Transcoder: No disk with enough space for \"%s\"\n", job->title);
        return -2;
    }

    char output_base[4096];
    snprintf(output_base, sizeof(output_base), "%s/nixly_ready_media",
             server_config.converted_paths[disk_idx]);

    /* Probe audio and video FIRST to include info in filename */
    AudioProbe audio;
    probe_audio(job->filepath, &audio);

    VideoProbe video;
    probe_video(job->filepath, &video);

    /* Build channel layout string */
    char audio_layout[8];
    int ch = audio.best_channels;
    if (ch >= 8)       snprintf(audio_layout, sizeof(audio_layout), "7.1");
    else if (ch >= 6)  snprintf(audio_layout, sizeof(audio_layout), "5.1");
    else if (ch >= 3)  snprintf(audio_layout, sizeof(audio_layout), "%d.1", ch - 1);
    else if (ch == 2)  snprintf(audio_layout, sizeof(audio_layout), "2.0");
    else               snprintf(audio_layout, sizeof(audio_layout), "1.0");

    /* Build resolution string */
    char resolution[8];
    int h = video.height;
    if (h >= 2160)      snprintf(resolution, sizeof(resolution), "4K");
    else if (h >= 1080) snprintf(resolution, sizeof(resolution), "1080p");
    else if (h >= 720)  snprintf(resolution, sizeof(resolution), "720p");
    else if (h >= 480)  snprintf(resolution, sizeof(resolution), "480p");
    else                snprintf(resolution, sizeof(resolution), "%dp", h);

    const char *hdr_tag = video.is_hdr ? ".HDR" : "";

    char bit_depth_tag[16];
    snprintf(bit_depth_tag, sizeof(bit_depth_tag), "%dbit", video.bit_depth);

    /* Build MKV output path */
    char out_dir[4096];
    char mkv_path[4096];
    char norm[256];

    if (job->type == 2) {
        normalize_title(job->show_name, norm, sizeof(norm));
        if (job->year > 0) {
            snprintf(out_dir, sizeof(out_dir), "%s/TV/%s.%d/Season%d",
                     output_base, norm, job->year, job->season);
            snprintf(mkv_path, sizeof(mkv_path), "%s/%s.%d.S%02dE%02d.%s.%s.x265.CRF14%s.%s.mkv",
                     out_dir, norm, job->year, job->season, job->episode,
                     resolution, bit_depth_tag, hdr_tag, audio_layout);
        } else {
            snprintf(out_dir, sizeof(out_dir), "%s/TV/%s/Season%d",
                     output_base, norm, job->season);
            snprintf(mkv_path, sizeof(mkv_path), "%s/%s.S%02dE%02d.%s.%s.x265.CRF14%s.%s.mkv",
                     out_dir, norm, job->season, job->episode,
                     resolution, bit_depth_tag, hdr_tag, audio_layout);
        }
    } else {
        char raw_title[256];
        strncpy(raw_title, job->title, sizeof(raw_title) - 1);
        raw_title[sizeof(raw_title) - 1] = '\0';

        int year = strip_year_from_title(raw_title);
        normalize_title(raw_title, norm, sizeof(norm));

        snprintf(out_dir, sizeof(out_dir), "%s/Movies", output_base);
        if (year > 0) {
            snprintf(mkv_path, sizeof(mkv_path), "%s/%s.%d.%s.%s.x265.CRF14%s.%s.mkv",
                     out_dir, norm, year, resolution, bit_depth_tag, hdr_tag, audio_layout);
        } else {
            snprintf(mkv_path, sizeof(mkv_path), "%s/%s.%s.%s.x265.CRF14%s.%s.mkv",
                     out_dir, norm, resolution, bit_depth_tag, hdr_tag, audio_layout);
        }
    }

    mkdir_p(out_dir);

    /* Skip if MKV already exists */
    struct stat st;
    if (stat(mkv_path, &st) == 0 && st.st_size > 0) {
        printf("Transcoder: [%d/%d] Skip (exists): %s\n",
               tc.completed_jobs + 1, tc.total_jobs, job->title);
        /* Ensure skipped files are in the database (they may be missing after
         * a DB reset, config change, or if the initial scan missed them) */
        scanner_scan_file(mkv_path, 1);
        pthread_mutex_lock(&tc.lock);
        tc.completed_jobs++;
        pthread_mutex_unlock(&tc.lock);
        return 1;
    }

    printf("\n========================================\n");
    printf("Transcoder: [%d/%d] %s\n",
           tc.completed_jobs + 1, tc.total_jobs, job->title);
    printf("  Input:  %s\n", job->filepath);
    if (job->filepath2[0])
        printf("  Input2: %s (multi-part)\n", job->filepath2);
    printf("  Output: %s\n", mkv_path);

    if (!wait_for_stable_file(job->filepath)) {
        printf("  Skipping: file not stable or disappeared\n");
        printf("========================================\n");
        pthread_mutex_lock(&tc.lock);
        tc.completed_jobs++;
        pthread_mutex_unlock(&tc.lock);
        return 1;
    }

    /* For multi-part, also wait for part 2 to be stable */
    if (job->filepath2[0] && !wait_for_stable_file(job->filepath2)) {
        printf("  Skipping: part 2 not stable or disappeared\n");
        printf("========================================\n");
        pthread_mutex_lock(&tc.lock);
        tc.completed_jobs++;
        pthread_mutex_unlock(&tc.lock);
        return 1;
    }

    double dur = probe_duration(job->filepath);
    if (job->filepath2[0])
        dur += probe_duration(job->filepath2);
    pthread_mutex_lock(&tc.lock);
    tc.duration_secs = dur;
    tc.progress_secs = 0;
    pthread_mutex_unlock(&tc.lock);
    printf("  Duration: %.1f seconds%s\n", dur,
           job->filepath2[0] ? " (combined)" : "");

    printf("  Audio: best stream=%d (%s), norsk=%s, atmos=%s\n",
           audio.best_audio_idx, audio_layout,
           audio.nor_audio_idx >= 0 ? "yes" : "no",
           audio.atmos_idx >= 0 ? "yes" : "no");

    if (video.is_hdr) {
        printf("  Video: %s %dx%d %dbit HDR (%s)%s%s\n",
               resolution, video.width, video.height, video.bit_depth,
               video.color_trc == AVCOL_TRC_SMPTE2084 ? "HDR10" : "HLG",
               video.has_master_display ? " +mastering-display" : "",
               video.has_cll ? " +content-light" : "");
    } else {
        printf("  Video: %s %dx%d %dbit SDR\n", resolution, video.width, video.height, video.bit_depth);
    }

    SubtitleProbe subs;
    probe_subtitles(job->filepath, &subs);
    int total_subs = subs.count + subs.ext_count;
    if (total_subs > 0) {
        printf("  Subtitles: %d embedded", subs.count);
        if (subs.count > 0) {
            printf(" (");
            for (int j = 0; j < subs.count; j++)
                printf("%s%s", j > 0 ? ", " : "",
                       subs.is_english[j] ? "English" : "Norwegian");
            printf(")");
        }
        if (subs.ext_count > 0) {
            printf(", %d external (", subs.ext_count);
            for (int j = 0; j < subs.ext_count; j++)
                printf("%s%s", j > 0 ? ", " : "",
                       subs.ext_is_english[j] ? "English" : "Norwegian");
            printf(")");
        }
        printf("\n");
    } else {
        printf("  Subtitles: none (English/Norwegian)\n");
    }

    /* Build temp file path: sibling of nixly_ready_media */
    char temp_dir[4096];
    snprintf(temp_dir, sizeof(temp_dir), "%s/nixly_transcode_tmp",
             server_config.converted_paths[disk_idx]);
    mkdir_p(temp_dir);

    const char *mkv_base = strrchr(mkv_path, '/');
    mkv_base = mkv_base ? mkv_base + 1 : mkv_path;

    char tmp_path[4096];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s.tmp", temp_dir, mkv_base);

    char resume_path[4096];
    snprintf(resume_path, sizeof(resume_path), "%s.resume", tmp_path);

    int ret;

    /* Check for existing partial encode (resume support) */
    if (stat(tmp_path, &st) == 0 && st.st_size > 0) {
        double tmp_dur = probe_tmp_duration(tmp_path);
        printf("  Resume: found partial encode (%.1f seconds)\n", tmp_dur);

        if (tmp_dur > 30.0) {
            /* Resume: encode remainder to .tmp.resume, then concat */
            printf("  Resume: encoding from %.1f seconds onward\n", tmp_dur);
            ret = run_ffmpeg(job->filepath, job->filepath2, resume_path,
                             tmp_dur, &audio, &video, &subs);

            if (ret == 0) {
                /* Concat .tmp + .tmp.resume → final mkv_path */
                ret = concat_copy(tmp_path, resume_path, mkv_path);
            } else {
                /* FFmpeg failed on resume part — keep .tmp for next attempt */
                fprintf(stderr, "  Error: FFmpeg failed during resume for %s\n", job->title);
                unlink(resume_path);
            }
        } else {
            /* Too short to resume — start fresh */
            printf("  Resume: partial too short (%.1fs), restarting\n", tmp_dur);
            unlink(tmp_path);
            ret = run_ffmpeg(job->filepath, job->filepath2, tmp_path,
                             0, &audio, &video, &subs);
            if (ret == 0)
                rename(tmp_path, mkv_path);
        }
    } else {
        /* No partial — encode from scratch to temp */
        ret = run_ffmpeg(job->filepath, job->filepath2, tmp_path,
                         0, &audio, &video, &subs);
        if (ret == 0)
            rename(tmp_path, mkv_path);
    }

    if (ret == 0) {
        if (stat(mkv_path, &st) == 0 && st.st_size > 0) {
            printf("  Success! Output: %s\n", mkv_path);

            database_delete_by_path(job->filepath);
            if (job->filepath2[0])
                database_delete_by_path(job->filepath2);

            if (server_config.delete_after_conversion) {
                if (job->source_dir[0]) {
                    rmdir_r(job->source_dir);
                    printf("  Deleted Blu-ray source: %s\n", job->source_dir);
                    cleanup_source_dir(job->source_dir);
                    /* Also clean part 2 BDR directory */
                    if (job->source_dir2[0]) {
                        rmdir_r(job->source_dir2);
                        printf("  Deleted Blu-ray source (part 2): %s\n", job->source_dir2);
                        cleanup_source_dir(job->source_dir2);
                    }
                } else if (unlink(job->filepath) == 0) {
                    printf("  Deleted source: %s\n", job->filepath);
                    cleanup_source_dir(job->filepath);
                } else {
                    fprintf(stderr, "  Warning: Could not delete source: %s\n",
                            strerror(errno));
                }

                for (int j = 0; j < subs.ext_count; j++) {
                    if (unlink(subs.ext_files[j]) == 0)
                        printf("  Deleted subtitle: %s\n", subs.ext_files[j]);
                    else
                        fprintf(stderr, "  Warning: Could not delete subtitle: %s (%s)\n",
                                subs.ext_files[j], strerror(errno));
                }
            } else {
                printf("  Keeping source: %s\n", job->filepath);
            }

            scanner_scan_file(mkv_path, 1);
        } else {
            fprintf(stderr, "  Error: MKV missing after conversion, keeping source\n");
            ret = -1;
        }
    } else if (ret != 0) {
        /* FFmpeg error — keep .tmp for potential resume, only clean .resume */
        fprintf(stderr, "  Error: FFmpeg failed for %s, keeping source\n", job->title);
        unlink(resume_path);
    }

    printf("========================================\n");

    pthread_mutex_lock(&tc.lock);
    tc.completed_jobs++;
    pthread_mutex_unlock(&tc.lock);

    return ret;
}

/* Helper: scan all unprocessed paths into a job list */
static void scan_all_sources(JobList *list) {
    for (int i = 0; i < server_config.unprocessed_path_count; i++)
        collect_from_dir(server_config.unprocessed_paths[i], list);
}

/* ================================================================
 *  Background Thread
 *
 *  Queue logic:
 *    1. Scan sources, pick newest file (by mtime)
 *    2. If movie → convert it, then re-scan (step 1)
 *    3. If episode → start that series (season-by-season, S01E01 first)
 *       After EACH episode, re-scan and check for interrupts:
 *       - If a file exists that is NOT part of the current series
 *         and has mtime newer than when the series batch started,
 *         process that one file immediately, then resume the series.
 *    4. When the series is done → re-scan (step 1)
 * ================================================================ */

static void *transcode_thread(void *arg) {
    (void)arg;

    printf("\n");
    printf("========================================\n");
    printf("  Transcoder starting (x265 CRF 14, MKV)\n");
    for (int i = 0; i < server_config.converted_path_count; i++) {
        char td[4096];
        snprintf(td, sizeof(td), "%s/nixly_transcode_tmp",
                 server_config.converted_paths[i]);
        mkdir_p(td);
        printf("  Temp dir: %s\n", td);
    }
    printf("========================================\n");

    disk_map_reset();

    while (!tc.stop_requested) {
        /* Scan all sources */
        JobList list = {0};
        scan_all_sources(&list);

        /* Merge multi-part Blu-ray rips (p1+p2, part1+part2) into single jobs */
        merge_multipart_bluray_jobs(&list);

        if (list.count == 0) {
            free(list.jobs);
            break;
        }

        /* Sort newest first */
        qsort(list.jobs, list.count, sizeof(TranscodeJob), mtime_compare_desc);

        /* Check for priority override */
        TranscodeJob *chosen = NULL;
        pthread_mutex_lock(&priority_override.lock);
        if (priority_override.title[0]) {
            for (int i = 0; i < list.count; i++) {
                if (strcmp(list.jobs[i].title, priority_override.title) == 0) {
                    if (verify_tmdb_single(&list.jobs[i])) {
                        chosen = &list.jobs[i];
                        printf("Transcoder: Priority override → \"%s\"\n", chosen->title);
                    }
                    break;
                }
            }
            priority_override.title[0] = '\0';
        }
        pthread_mutex_unlock(&priority_override.lock);

        /* Find newest TMDB-verified, non-skipped job */
        if (!chosen) {
            for (int i = 0; i < list.count; i++) {
                if (is_skipped(list.jobs[i].title)) {
                    printf("  Skipped (user): \"%s\"\n", list.jobs[i].title);
                    continue;
                }
                if (verify_tmdb_single(&list.jobs[i])) {
                    chosen = &list.jobs[i];
                    break;
                }
                printf("  TMDB: \"%s\" not found, skipping\n", list.jobs[i].title);
            }
        }

        if (!chosen) {
            printf("Transcoder: No TMDB-verified files remaining\n");
            free(list.jobs);
            break;
        }

        if (chosen->type == 0) {
            /* Movie: process it, then re-scan */
            printf("Transcoder: Newest → movie \"%s\"\n", chosen->title);
            int ret = process_single_job(chosen);
            free(list.jobs);
            if (ret == -2) break; /* no disk space */
            continue;
        }

        /* ---- Episode: series mode with interrupt support ---- */
        char series_name[256];
        strncpy(series_name, chosen->show_name, sizeof(series_name) - 1);
        series_name[sizeof(series_name) - 1] = '\0';
        time_t series_start_mtime = chosen->mtime; /* newest mtime when series was picked */

        printf("Transcoder: Newest → \"%s\" S%02dE%02d — starting series\n",
               series_name, chosen->season, chosen->episode);

        free(list.jobs);

        while (!tc.stop_requested) {
            /* Re-scan to get current state (source files are deleted after conversion) */
            JobList scan = {0};
            scan_all_sources(&scan);
            merge_multipart_bluray_jobs(&scan);

            if (scan.count == 0) {
                free(scan.jobs);
                break;
            }

            /* Check for interrupt: any non-series file newer than series_start_mtime? */
            qsort(scan.jobs, scan.count, sizeof(TranscodeJob), mtime_compare_desc);

            for (int i = 0; i < scan.count; i++) {
                TranscodeJob *candidate = &scan.jobs[i];

                /* Skip files from our current series */
                if (candidate->type == 2 &&
                    strcasecmp(candidate->show_name, series_name) == 0)
                    continue;

                /* Only interrupt if this file is newer than when we started the series */
                if (candidate->mtime <= series_start_mtime)
                    break; /* sorted desc, so nothing newer follows */

                if (!verify_tmdb_single(candidate)) {
                    printf("  TMDB: interrupt \"%s\" not found, skipping\n", candidate->title);
                    continue;
                }

                /* Interrupt! Process this one file, then resume series */
                printf("\n  >>> Interrupt: \"%s\" (newer than series start)\n", candidate->title);
                int ret = process_single_job(candidate);
                printf("  >>> Resuming series \"%s\"\n\n", series_name);
                if (ret == -2) { free(scan.jobs); goto done; }
                break; /* only handle one interrupt per episode */
            }

            /* Now find the next episode of our series (skip user-skipped) */
            JobList episodes = {0};
            for (int i = 0; i < scan.count; i++) {
                if (scan.jobs[i].type == 2 &&
                    strcasecmp(scan.jobs[i].show_name, series_name) == 0 &&
                    !is_skipped(scan.jobs[i].title))
                    joblist_add(&episodes, &scan.jobs[i]);
            }

            if (episodes.count == 0) {
                /* Series is done */
                printf("Transcoder: Series \"%s\" complete\n", series_name);
                free(episodes.jobs);
                free(scan.jobs);
                break;
            }

            /* Sort by season → episode, process the first one */
            qsort(episodes.jobs, episodes.count, sizeof(TranscodeJob), episode_compare);

            int ret = process_single_job(&episodes.jobs[0]);
            free(episodes.jobs);
            free(scan.jobs);

            if (ret == -2) goto done; /* no disk space */
        }
        /* Series done or stopped — loop back to re-scan for next work */
    }

done:
    disk_map_reset();

    pthread_mutex_lock(&tc.lock);
    /* Preserve STOPPED state so user's Stop action persists until Start is clicked.
     * Only reset to IDLE if the thread finished naturally (not via stop). */
    if (tc.state != TRANSCODE_STOPPED) {
        tc.state = TRANSCODE_IDLE;
    }
    tc.current_file[0] = '\0';
    pthread_mutex_unlock(&tc.lock);

    printf("\n");
    printf("========================================\n");
    printf("  Transcoder complete\n");
    printf("  Converted: %d / %d files\n", tc.completed_jobs, tc.total_jobs);
    printf("========================================\n\n");

    return NULL;
}

/* ================================================================
 *  Public API
 * ================================================================ */

int transcoder_init(void) {
    memset(&tc, 0, sizeof(tc));
    pthread_mutex_init(&tc.lock, NULL);
    tc.state = TRANSCODE_IDLE;

    printf("Transcoder: x265 CRF 14 MKV\n");
    for (int i = 0; i < server_config.unprocessed_path_count; i++)
        printf("  Source %d: %s\n", i, server_config.unprocessed_paths[i]);
    for (int i = 0; i < server_config.converted_path_count; i++)
        printf("  Dest %d: %s/nixly_ready_media/\n", i, server_config.converted_paths[i]);
    return 0;
}

int transcoder_start(void) {
    pthread_mutex_lock(&tc.lock);
    if (tc.state == TRANSCODE_RUNNING) {
        pthread_mutex_unlock(&tc.lock);
        printf("Transcoder: Already running\n");
        return -1;
    }

    /* Join previous thread before starting a new one to prevent
     * the old thread from overwriting state set by the new thread */
    if (tc.thread_active) {
        pthread_t old = tc.thread;
        tc.thread_active = 0;
        pthread_mutex_unlock(&tc.lock);
        pthread_join(old, NULL);
        pthread_mutex_lock(&tc.lock);
        /* Re-check after releasing lock */
        if (tc.state == TRANSCODE_RUNNING) {
            pthread_mutex_unlock(&tc.lock);
            return -1;
        }
    }

    tc.state = TRANSCODE_RUNNING;
    tc.stop_requested = 0;
    tc.total_jobs = 0;
    tc.completed_jobs = 0;
    pthread_mutex_unlock(&tc.lock);

    if (pthread_create(&tc.thread, NULL, transcode_thread, NULL) != 0) {
        perror("transcoder: pthread_create");
        pthread_mutex_lock(&tc.lock);
        tc.state = TRANSCODE_IDLE;
        pthread_mutex_unlock(&tc.lock);
        return -1;
    }

    pthread_mutex_lock(&tc.lock);
    tc.thread_active = 1;
    pthread_mutex_unlock(&tc.lock);
    return 0;
}

void transcoder_stop(void) {
    pthread_mutex_lock(&tc.lock);
    if (tc.state != TRANSCODE_RUNNING) {
        pthread_mutex_unlock(&tc.lock);
        return;
    }
    tc.stop_requested = 1;
    tc.state = TRANSCODE_STOPPED;
    pthread_mutex_unlock(&tc.lock);

    /* Kill running ffmpeg process if any */
    pid_t pid = tc.ffmpeg_pid;
    if (pid > 0) {
        kill(pid, SIGTERM);
    }
}

void transcoder_cleanup(void) {
    /* Force stop even if not running */
    tc.stop_requested = 1;
    pid_t pid = tc.ffmpeg_pid;
    if (pid > 0) kill(pid, SIGTERM);

    if (tc.thread_active) {
        pthread_join(tc.thread, NULL);
        tc.thread_active = 0;
    }
    pthread_mutex_destroy(&tc.lock);
}

TranscodeState transcoder_get_state(void) {
    return tc.state;
}

int transcoder_get_total_jobs(void) {
    return tc.total_jobs;
}

int transcoder_get_completed_jobs(void) {
    return tc.completed_jobs;
}

const char *transcoder_get_current_file(void) {
    return tc.current_file;
}

double transcoder_get_progress(void) {
    if (tc.state != TRANSCODE_RUNNING || tc.duration_secs <= 0)
        return 0;
    double pct = (tc.progress_secs / tc.duration_secs) * 100.0;
    if (pct > 100.0) pct = 100.0;
    if (pct < 0.0) pct = 0.0;
    return pct;
}

/* Escape a string for JSON output (handles \, ", control chars) */
static void json_escape(const char *src, char *dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 6 < dst_size; i++) {
        switch (src[i]) {
            case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
            case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
            case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
            case '\r': dst[j++] = '\\'; dst[j++] = 'r';  break;
            case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
            default:   dst[j++] = src[i]; break;
        }
    }
    dst[j] = '\0';
}

char *transcoder_get_queue_json(void) {
    JobList list = {0};
    scan_all_sources(&list);
    merge_multipart_bluray_jobs(&list);
    qsort(list.jobs, list.count, sizeof(TranscodeJob), mtime_compare_desc);

    /* Allocate generously: ~512 bytes per job */
    size_t buf_size = 512 + (size_t)list.count * 512;
    char *json = malloc(buf_size);
    if (!json) { free(list.jobs); return NULL; }

    int pos = 0;
    pos += snprintf(json + pos, buf_size - pos, "[");

    int first = 1;
    for (int i = 0; i < list.count; i++) {
        TranscodeJob *j = &list.jobs[i];

        /* Skip jobs whose output already exists in nixly_ready_media */
        if (is_already_in_ready_media(j)) continue;

        char esc_title[512], esc_show[512], esc_path[8192];
        json_escape(j->title, esc_title, sizeof(esc_title));
        json_escape(j->show_name, esc_show, sizeof(esc_show));
        json_escape(j->filepath, esc_path, sizeof(esc_path));

        int skipped = is_skipped(j->title);
        int is_current = (tc.state == TRANSCODE_RUNNING &&
                          strcmp(j->title, tc.current_file) == 0);

        if (!first) pos += snprintf(json + pos, buf_size - pos, ",");
        first = 0;

        pos += snprintf(json + pos, buf_size - pos,
            "{\"title\":\"%s\",\"type\":%d,\"show\":\"%s\","
            "\"season\":%d,\"episode\":%d,\"year\":%d,"
            "\"path\":\"%s\",\"skipped\":%s,\"current\":%s}",
            esc_title, j->type, esc_show,
            j->season, j->episode, j->year,
            esc_path, skipped ? "true" : "false",
            is_current ? "true" : "false");
    }

    pos += snprintf(json + pos, buf_size - pos, "]");
    free(list.jobs);
    return json;
}

int transcoder_skip_title(const char *title) {
    pthread_mutex_lock(&skip_list.lock);
    /* Check if already skipped */
    for (int i = 0; i < skip_list.count; i++) {
        if (strcmp(skip_list.titles[i], title) == 0) {
            pthread_mutex_unlock(&skip_list.lock);
            return 0;
        }
    }
    if (skip_list.count >= MAX_SKIP) {
        pthread_mutex_unlock(&skip_list.lock);
        return -1;
    }
    strncpy(skip_list.titles[skip_list.count], title, 255);
    skip_list.titles[skip_list.count][255] = '\0';
    skip_list.count++;
    printf("Transcoder: Skip added: \"%s\"\n", title);
    pthread_mutex_unlock(&skip_list.lock);
    return 0;
}

int transcoder_unskip_title(const char *title) {
    pthread_mutex_lock(&skip_list.lock);
    for (int i = 0; i < skip_list.count; i++) {
        if (strcmp(skip_list.titles[i], title) == 0) {
            /* Shift remaining entries down */
            memmove(&skip_list.titles[i], &skip_list.titles[i + 1],
                    (skip_list.count - i - 1) * sizeof(skip_list.titles[0]));
            skip_list.count--;
            printf("Transcoder: Skip removed: \"%s\"\n", title);
            pthread_mutex_unlock(&skip_list.lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&skip_list.lock);
    return -1;
}

int transcoder_prioritize_title(const char *title) {
    pthread_mutex_lock(&priority_override.lock);
    strncpy(priority_override.title, title, sizeof(priority_override.title) - 1);
    priority_override.title[sizeof(priority_override.title) - 1] = '\0';
    printf("Transcoder: Priority set: \"%s\"\n", title);
    pthread_mutex_unlock(&priority_override.lock);

    /* Start transcoder immediately if idle */
    if (transcoder_get_state() != TRANSCODE_RUNNING) {
        printf("Transcoder: Starting for prioritized title\n");
        transcoder_start();
    }
    return 0;
}
