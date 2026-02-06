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
 * Output: <source_path>/Nixly_Media/TV|Movies/<title>.x265.CRF14.mkv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
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

    /* Progress */
    char current_file[4096];
    int total_jobs;
    int completed_jobs;
    double duration_secs;   /* Total duration of current file */
    double progress_secs;   /* Current encoding position */
} tc;

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
    if (strstr(path, "/Nixly_Media/") != NULL) return 1;
    /* Also skip the config override output path */
    if (server_config.output_path[0] &&
        strncmp(path, server_config.output_path, strlen(server_config.output_path)) == 0)
        return 1;
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
    int atmos_idx;          /* Audio-stream-relative index of TrueHD/Atmos (-1 = none) */
} AudioProbe;

static void probe_audio(const char *filepath, AudioProbe *info) {
    info->best_audio_idx = 0;
    info->best_channels = 0;
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

/* Sort: TV episodes first (show -> season -> episode), then movies by title */
static int job_compare(const void *a, const void *b) {
    const TranscodeJob *ja = (const TranscodeJob *)a;
    const TranscodeJob *jb = (const TranscodeJob *)b;

    /* Episodes before movies */
    if (ja->type != jb->type) {
        return (ja->type == 0) ? 1 : -1;
    }

    if (ja->type == 2) {
        int cmp = strcasecmp(ja->show_name, jb->show_name);
        if (cmp != 0) return cmp;
        if (ja->season != jb->season) return ja->season - jb->season;
        return ja->episode - jb->episode;
    }

    return strcasecmp(ja->title, jb->title);
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

static int run_ffmpeg(const char *input, const char *output,
                      AudioProbe *audio, VideoProbe *video,
                      SubtitleProbe *subs) {
    const char *argv[192];
    int argc = 0;

    argv[argc++] = "ffmpeg";
    argv[argc++] = "-y";
    argv[argc++] = "-nostdin";
    argv[argc++] = "-progress";
    argv[argc++] = "pipe:2";
    argv[argc++] = "-i";
    argv[argc++] = input;

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

    /* Audio track 1: FLAC lossless, preserve source channel layout */
    char flac_map[32];
    snprintf(flac_map, sizeof(flac_map), "0:a:%d",
             audio->best_audio_idx >= 0 ? audio->best_audio_idx : 0);
    argv[argc++] = "-map";
    argv[argc++] = flac_map;
    argv[argc++] = "-c:a:0";
    argv[argc++] = "flac";

    /* Name track based on source channel count */
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
    argv[argc++] = "-metadata:s:a:0";
    argv[argc++] = flac_title;

    /* Audio track 2: Atmos (TrueHD) lossless passthrough if available */
    char atmos_map[32];
    if (audio->atmos_idx >= 0) {
        snprintf(atmos_map, sizeof(atmos_map), "0:a:%d", audio->atmos_idx);
        argv[argc++] = "-map";
        argv[argc++] = atmos_map;
        argv[argc++] = "-c:a:1";
        argv[argc++] = "copy";
        argv[argc++] = "-metadata:s:a:1";
        argv[argc++] = "title=Atmos Passthrough";
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
            /* -progress pipe:2 outputs key=value lines, look for out_time_ms= */
            if (strncmp(line, "out_time_ms=", 12) == 0) {
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

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }

    fprintf(stderr, "  ffmpeg exited with status %d\n",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
}

/* ================================================================
 *  Process one source path
 * ================================================================ */

static void process_source(const char *source_path) {
    printf("\nTranscoder: Scanning source: %s\n", source_path);

    JobList list = {0};
    collect_from_dir(source_path, &list);

    if (list.count == 0) {
        printf("Transcoder: No files to convert in %s\n", source_path);
        return;
    }

    /* Sort: TV shows first (by show/season/episode), then movies */
    qsort(list.jobs, list.count, sizeof(TranscodeJob), job_compare);

    pthread_mutex_lock(&tc.lock);
    tc.total_jobs += list.count;
    pthread_mutex_unlock(&tc.lock);

    printf("Transcoder: Found %d files to convert\n", list.count);

    /* Output base: config override if set, otherwise Nixly_Media inside source path */
    char output_base[4096];
    if (server_config.output_path[0]) {
        snprintf(output_base, sizeof(output_base), "%s", server_config.output_path);
    } else {
        snprintf(output_base, sizeof(output_base), "%s/Nixly_Media", source_path);
    }

    for (int i = 0; i < list.count && !tc.stop_requested; i++) {
        TranscodeJob *job = &list.jobs[i];

        pthread_mutex_lock(&tc.lock);
        strncpy(tc.current_file, job->title, sizeof(tc.current_file) - 1);
        pthread_mutex_unlock(&tc.lock);

        /* Probe audio and video FIRST to include info in filename */
        AudioProbe audio;
        probe_audio(job->filepath, &audio);

        VideoProbe video;
        probe_video(job->filepath, &video);

        /* Build channel layout string (e.g., "7.1", "5.1", "2.0") */
        char audio_layout[8];
        int ch = audio.best_channels;
        if (ch >= 8)
            snprintf(audio_layout, sizeof(audio_layout), "7.1");
        else if (ch >= 6)
            snprintf(audio_layout, sizeof(audio_layout), "5.1");
        else if (ch >= 3)
            snprintf(audio_layout, sizeof(audio_layout), "%d.1", ch - 1);
        else if (ch == 2)
            snprintf(audio_layout, sizeof(audio_layout), "2.0");
        else
            snprintf(audio_layout, sizeof(audio_layout), "1.0");

        /* Build resolution string (e.g., "4K", "1080p", "720p") */
        char resolution[8];
        int h = video.height;
        if (h >= 2160)
            snprintf(resolution, sizeof(resolution), "4K");
        else if (h >= 1080)
            snprintf(resolution, sizeof(resolution), "1080p");
        else if (h >= 720)
            snprintf(resolution, sizeof(resolution), "720p");
        else if (h >= 480)
            snprintf(resolution, sizeof(resolution), "480p");
        else
            snprintf(resolution, sizeof(resolution), "%dp", h);

        /* Build HDR string (empty if SDR) */
        const char *hdr_tag = video.is_hdr ? ".HDR" : "";

        /* Build bit depth string (e.g., "10bit", "8bit") */
        char bit_depth_tag[16];
        snprintf(bit_depth_tag, sizeof(bit_depth_tag), "%dbit", video.bit_depth);

        /* Build MKV output path with resolution, bit depth, HDR and audio layout */
        char out_dir[4096];
        char mkv_path[4096];
        char norm[256];

        if (job->type == 2) {
            /* Episode: TV/show.name.year/SeasonN/show.name.year.S01E01.1080p.10bit.x265.CRF14.HDR.7.1.mkv */
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
            /* Movie: Movies/movie.title.year.1080p.10bit.x265.CRF14.HDR.7.1.mkv */
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

        /* Create output directory */
        mkdir_p(out_dir);

        /* Skip if MKV already exists */
        struct stat st;
        if (stat(mkv_path, &st) == 0 && st.st_size > 0) {
            printf("Transcoder: [%d/%d] Skip (exists): %s\n",
                   tc.completed_jobs + 1, tc.total_jobs, job->title);
            pthread_mutex_lock(&tc.lock);
            tc.completed_jobs++;
            pthread_mutex_unlock(&tc.lock);
            continue;
        }

        printf("\n========================================\n");
        printf("Transcoder: [%d/%d] %s\n",
               tc.completed_jobs + 1, tc.total_jobs, job->title);
        printf("  Input:  %s\n", job->filepath);
        printf("  Output: %s\n", mkv_path);

        /* Wait for file to be completely written before processing */
        if (!wait_for_stable_file(job->filepath)) {
            printf("  Skipping: file not stable or disappeared\n");
            printf("========================================\n");
            pthread_mutex_lock(&tc.lock);
            tc.completed_jobs++;
            pthread_mutex_unlock(&tc.lock);
            continue;
        }

        /* Probe duration and reset progress */
        double dur = probe_duration(job->filepath);
        pthread_mutex_lock(&tc.lock);
        tc.duration_secs = dur;
        tc.progress_secs = 0;
        pthread_mutex_unlock(&tc.lock);
        printf("  Duration: %.1f seconds\n", dur);

        printf("  Audio: best stream=%d (%s), atmos=%s\n",
               audio.best_audio_idx, audio_layout,
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

        /* Probe subtitles (only keep English and Norwegian) */
        SubtitleProbe subs;
        probe_subtitles(job->filepath, &subs);
        int total_subs = subs.count + subs.ext_count;
        if (total_subs > 0) {
            printf("  Subtitles: %d embedded", subs.count);
            if (subs.count > 0) {
                printf(" (");
                for (int i = 0; i < subs.count; i++) {
                    printf("%s%s", i > 0 ? ", " : "",
                           subs.is_english[i] ? "English" : "Norwegian");
                }
                printf(")");
            }
            if (subs.ext_count > 0) {
                printf(", %d external (", subs.ext_count);
                for (int i = 0; i < subs.ext_count; i++) {
                    printf("%s%s", i > 0 ? ", " : "",
                           subs.ext_is_english[i] ? "English" : "Norwegian");
                }
                printf(")");
            }
            printf("\n");
        } else {
            printf("  Subtitles: none (English/Norwegian)\n");
        }

        /* Run ffmpeg MKV conversion */
        int ret = run_ffmpeg(job->filepath, mkv_path, &audio, &video, &subs);

        if (ret == 0) {
            /* Verify MKV exists */
            if (stat(mkv_path, &st) == 0 && st.st_size > 0) {
                printf("  Success! Output: %s\n", mkv_path);

                /* Remove old entry from database */
                database_delete_by_path(job->filepath);

                /* Delete source: entire Blu-ray tree or single file */
                if (job->source_dir[0]) {
                    rmdir_r(job->source_dir);
                    printf("  Deleted Blu-ray source: %s\n", job->source_dir);
                } else if (unlink(job->filepath) == 0) {
                    printf("  Deleted source: %s\n", job->filepath);
                } else {
                    fprintf(stderr, "  Warning: Could not delete source: %s\n",
                            strerror(errno));
                }

                /* Delete external subtitle files */
                for (int i = 0; i < subs.ext_count; i++) {
                    if (unlink(subs.ext_files[i]) == 0) {
                        printf("  Deleted subtitle: %s\n", subs.ext_files[i]);
                    } else {
                        fprintf(stderr, "  Warning: Could not delete subtitle: %s (%s)\n",
                                subs.ext_files[i], strerror(errno));
                    }
                }

                /* Scan MKV into database (with TMDB fetch) */
                scanner_scan_file(mkv_path, 1);
            } else {
                fprintf(stderr, "  Error: MKV missing after conversion, keeping source\n");
            }
        } else {
            fprintf(stderr, "  Error: FFmpeg failed for %s, keeping source\n", job->title);
            unlink(mkv_path); /* Remove partial MKV output */
        }

        printf("========================================\n");

        pthread_mutex_lock(&tc.lock);
        tc.completed_jobs++;
        pthread_mutex_unlock(&tc.lock);
    }

    free(list.jobs);
}

/* ================================================================
 *  Background Thread
 * ================================================================ */

static void *transcode_thread(void *arg) {
    (void)arg;

    printf("\n");
    printf("========================================\n");
    printf("  Transcoder starting (x265 CRF 14, MKV)\n");
    printf("========================================\n");

    /* Process generic media paths */
    for (int i = 0; i < server_config.media_path_count && !tc.stop_requested; i++) {
        process_source(server_config.media_paths[i]);
    }

    /* Process dedicated movies paths */
    for (int i = 0; i < server_config.movies_path_count && !tc.stop_requested; i++) {
        process_source(server_config.movies_paths[i]);
    }

    /* Process dedicated TV show paths */
    for (int i = 0; i < server_config.tvshows_path_count && !tc.stop_requested; i++) {
        process_source(server_config.tvshows_paths[i]);
    }

    pthread_mutex_lock(&tc.lock);
    tc.state = TRANSCODE_IDLE;
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

    if (server_config.output_path[0])
        printf("Transcoder: x265 CRF 14 MKV, output: %s\n", server_config.output_path);
    else
        printf("Transcoder: x265 CRF 14 MKV, output: Nixly_Media/ per source\n");
    return 0;
}

int transcoder_start(void) {
    pthread_mutex_lock(&tc.lock);
    if (tc.state == TRANSCODE_RUNNING) {
        pthread_mutex_unlock(&tc.lock);
        printf("Transcoder: Already running\n");
        return -1;
    }
    tc.state = TRANSCODE_RUNNING;
    tc.stop_requested = 0;
    tc.total_jobs = 0;
    tc.completed_jobs = 0;
    pthread_mutex_unlock(&tc.lock);

    if (pthread_create(&tc.thread, NULL, transcode_thread, NULL) != 0) {
        perror("transcoder: pthread_create");
        tc.state = TRANSCODE_IDLE;
        return -1;
    }

    pthread_detach(tc.thread);
    return 0;
}

void transcoder_stop(void) {
    tc.stop_requested = 1;

    /* Kill running ffmpeg process if any */
    pid_t pid = tc.ffmpeg_pid;
    if (pid > 0) {
        kill(pid, SIGTERM);
    }

    pthread_mutex_lock(&tc.lock);
    tc.state = TRANSCODE_STOPPED;
    pthread_mutex_unlock(&tc.lock);
}

void transcoder_cleanup(void) {
    transcoder_stop();
    /* Give thread a moment to finish */
    usleep(100000);
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
