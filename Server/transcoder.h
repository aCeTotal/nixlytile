/*
 * Nixly Media Server - Transcoder Module
 * Automatic x265 CRF 14 conversion to MKV with FLAC 7.1 + Atmos passthrough
 *
 * Processes all configured source paths sequentially:
 *   - One source at a time
 *   - TV shows first, completing each show/season before the next
 *   - Then movies
 *   - Source files are deleted after successful conversion
 *   - HDR (HDR10/HLG) metadata is preserved automatically
 *
 * Output structure (nixly_ready_media inside each converted_path disk):
 *   <converted_path>/nixly_ready_media/
 *     TV/
 *       show.name.year/
 *         Season1/
 *           show.name.year.S01E01.1080p.10bit.x265.CRF14.HDR.7.1.mkv
 *     Movies/
 *       movie.title.year.4K.10bit.x265.CRF14.HDR.5.1.mkv
 *
 * Filename includes: resolution (4K/1080p/720p), bit depth (8bit/10bit/12bit), HDR (if present), audio layout
 *
 * Audio tracks:
 *   Track 1: "Lossless 7.1" - FLAC 7.1 (from best available audio stream)
 *   Track 2: "Atmos Passthrough" - TrueHD passthrough (if available)
 *
 * MKV optimized for HTTP streaming (Cues at file start, 2s clusters).
 */

#ifndef TRANSCODER_H
#define TRANSCODER_H

#include <time.h>

#define TRANSCODER_CRF 14

typedef enum {
    TRANSCODE_IDLE = 0,
    TRANSCODE_RUNNING,
    TRANSCODE_STOPPED
} TranscodeState;

typedef struct {
    char filepath[4096];
    char filepath2[4096];       /* Part 2 input (multi-part BDR: p1/p2, part1/part2) */
    char source_dir[4096];  /* Non-empty = Blu-ray root dir (delete whole tree on success) */
    char source_dir2[4096];     /* Part 2 BDR root dir (for cleanup) */
    int type;               /* 0=movie, 2=episode */
    char show_name[256];
    char title[256];
    int season;
    int episode;
    int year;               /* Release year (from filename or TMDB) */
    time_t mtime;           /* File modification time for priority sorting */
    int tmdb_verified;      /* 1 = found in TMDB, 0 = not checked, -1 = not found */
} TranscodeJob;

/* Initialize transcoder (call after config is loaded) */
int transcoder_init(void);

/* Start background transcoding thread */
int transcoder_start(void);

/* Stop transcoding gracefully */
void transcoder_stop(void);

/* Cleanup resources */
void transcoder_cleanup(void);

/* Status queries */
TranscodeState transcoder_get_state(void);
int transcoder_get_total_jobs(void);
int transcoder_get_completed_jobs(void);
const char *transcoder_get_current_file(void);
double transcoder_get_progress(void);

#endif /* TRANSCODER_H */
