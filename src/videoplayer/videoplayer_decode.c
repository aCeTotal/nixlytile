/*
 * Nixlytile Video Player - Decoding Module
 * FFmpeg demuxing and decoding with VA-API/NVDEC/software fallback
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>

#include "videoplayer.h"

/* Hardware device paths */
#define VAAPI_DEVICE "/dev/dri/renderD128"
#define VAAPI_DEVICE_ALT "/dev/dri/renderD129"

/* ================================================================
 *  Forward Declarations
 * ================================================================ */

static int init_video_decoder(VideoPlayer *vp);
static int init_audio_decoder(VideoPlayer *vp);
static int init_subtitle_decoder(VideoPlayer *vp);
static int init_vaapi(VideoPlayer *vp);
static int init_nvdec(VideoPlayer *vp);
static int init_software_decoder(VideoPlayer *vp);
static enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);
static void *decode_thread_func(void *arg);
static void *demux_thread_func(void *arg);

/* External functions */
extern void videoplayer_play(VideoPlayer *vp);
extern int videoplayer_audio_queue_frame(VideoPlayer *vp, AVFrame *frame);
extern int videoplayer_audio_init(VideoPlayer *vp);
extern void videoplayer_audio_cleanup(VideoPlayer *vp);
extern void videoplayer_audio_cmd_shutdown(VideoPlayer *vp);
extern void videoplayer_audio_play(VideoPlayer *vp);
extern void videoplayer_audio_pause(VideoPlayer *vp);
extern void videoplayer_audio_flush(VideoPlayer *vp);
extern void videoplayer_hide_control_bar(VideoPlayer *vp);
extern void videoplayer_show_seek_osd(VideoPlayer *vp);

/* Subtitle processing */
static void videoplayer_process_subtitle_packet(VideoPlayer *vp, AVPacket *pkt)
{
    AVSubtitle sub;
    int got_sub = 0;
    int ret;

    if (!vp || !pkt || !vp->subtitle_codec_ctx)
        return;

    ret = avcodec_decode_subtitle2(vp->subtitle_codec_ctx, &sub, &got_sub, pkt);
    if (ret < 0 || !got_sub)
        return;

    /* Calculate timing */
    AVRational tb = vp->fmt_ctx->streams[vp->subtitle_tracks[vp->current_subtitle_track].stream_index]->time_base;
    int64_t start_ms = av_rescale_q(pkt->pts, tb, (AVRational){1, 1000});
    int64_t duration_ms = sub.end_display_time - sub.start_display_time;
    if (duration_ms <= 0)
        duration_ms = 5000;  /* Default 5 seconds */

    /* Process each subtitle rect */
    for (unsigned i = 0; i < sub.num_rects; i++) {
        AVSubtitleRect *rect = sub.rects[i];

        if (!rect)
            continue;

        /* Handle text-based subtitles (ASS/SRT) */
        if (rect->type == SUBTITLE_ASS && rect->ass && vp->subtitle.track) {
            /* Add ASS dialogue line to track */
            ass_process_data(vp->subtitle.track, rect->ass, strlen(rect->ass));
        }
        else if (rect->type == SUBTITLE_TEXT && rect->text && vp->subtitle.track) {
            /* Convert plain text to ASS format */
            char ass_line[2048];
            snprintf(ass_line, sizeof(ass_line),
                     "Dialogue: 0,%d:%02d:%02d.%02d,%d:%02d:%02d.%02d,Default,,0,0,0,,%s",
                     (int)(start_ms / 3600000),
                     (int)((start_ms / 60000) % 60),
                     (int)((start_ms / 1000) % 60),
                     (int)((start_ms / 10) % 100),
                     (int)((start_ms + duration_ms) / 3600000),
                     (int)(((start_ms + duration_ms) / 60000) % 60),
                     (int)(((start_ms + duration_ms) / 1000) % 60),
                     (int)(((start_ms + duration_ms) / 10) % 100),
                     rect->text);
            ass_process_data(vp->subtitle.track, ass_line, strlen(ass_line));
        }
        /* Bitmap subtitles (PGS, VOBSUB) would need different handling */
    }

    avsubtitle_free(&sub);
}

/* ================================================================
 *  Stream Detection and Probing
 * ================================================================ */

static void extract_track_title(AVDictionary *metadata, char *title, size_t len)
{
    AVDictionaryEntry *e;

    title[0] = '\0';

    e = av_dict_get(metadata, "title", NULL, 0);
    if (e && e->value) {
        snprintf(title, len, "%s", e->value);
        return;
    }

    e = av_dict_get(metadata, "handler_name", NULL, 0);
    if (e && e->value) {
        snprintf(title, len, "%s", e->value);
    }
}

static void extract_track_language(AVDictionary *metadata, char *lang, size_t len)
{
    AVDictionaryEntry *e;

    lang[0] = '\0';

    e = av_dict_get(metadata, "language", NULL, 0);
    if (e && e->value) {
        snprintf(lang, len, "%s", e->value);
    }
}

static int probe_video_stream(VideoPlayer *vp, int stream_idx)
{
    AVStream *stream = vp->fmt_ctx->streams[stream_idx];
    AVCodecParameters *cp = stream->codecpar;
    VideoTrack *video = &vp->video;

    video->stream_index = stream_idx;
    video->width = cp->width;
    video->height = cp->height;
    video->framerate = stream->avg_frame_rate;
    video->time_base = stream->time_base;
    video->pix_fmt = cp->format;
    video->color_primaries = cp->color_primaries;
    video->color_trc = cp->color_trc;
    video->color_space = cp->color_space;
    video->color_range = cp->color_range;

    /* Detect bit depth */
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(cp->format);
    if (pix_desc) {
        video->bit_depth = pix_desc->comp[0].depth;
    } else {
        video->bit_depth = 8;
    }

    /* Detect HDR */
    if (cp->color_primaries == AVCOL_PRI_BT2020 &&
        (cp->color_trc == AVCOL_TRC_SMPTE2084 ||    /* HDR10 PQ */
         cp->color_trc == AVCOL_TRC_ARIB_STD_B67))  /* HLG */
    {
        video->is_hdr = 1;
    }

    /* Extract mastering display and content light metadata */
    for (int j = 0; j < cp->nb_coded_side_data; j++) {
        AVPacketSideData *sd = &cp->coded_side_data[j];

        if (sd->type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA &&
            sd->size >= (int)sizeof(AVMasteringDisplayMetadata)) {
            const AVMasteringDisplayMetadata *mdm =
                (const AVMasteringDisplayMetadata *)sd->data;
            if (mdm->has_primaries && mdm->has_luminance) {
                video->has_mastering_display = 1;
                video->mastering = *mdm;
            }
        }

        if (sd->type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL &&
            sd->size >= (int)sizeof(AVContentLightMetadata)) {
            const AVContentLightMetadata *cll =
                (const AVContentLightMetadata *)sd->data;
            video->has_content_light = 1;
            video->content_light = *cll;
        }
    }

    /* Calculate video FPS */
    if (video->framerate.num > 0 && video->framerate.den > 0) {
        vp->video_fps = (float)av_q2d(video->framerate);
    } else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
        vp->video_fps = (float)av_q2d(stream->r_frame_rate);
        video->framerate = stream->r_frame_rate;
    } else {
        vp->video_fps = 24.0f;  /* Default fallback */
        video->framerate = (AVRational){24, 1};
    }

    fprintf(stderr, "[videoplayer] Video: %dx%d @ %.3f fps, %d-bit, HDR=%d\n",
            video->width, video->height, vp->video_fps, video->bit_depth, video->is_hdr);

    return 0;
}

static int probe_audio_stream(VideoPlayer *vp, int stream_idx)
{
    AVStream *stream = vp->fmt_ctx->streams[stream_idx];
    AVCodecParameters *cp = stream->codecpar;

    if (vp->audio_track_count >= VP_MAX_AUDIO_TRACKS)
        return -1;

    AudioTrack *track = &vp->audio_tracks[vp->audio_track_count];
    memset(track, 0, sizeof(*track));

    track->stream_index = stream_idx;
    track->index = vp->audio_track_count;
    track->channels = cp->ch_layout.nb_channels;
    track->sample_rate = cp->sample_rate;
    track->codec_id = cp->codec_id;

    extract_track_title(stream->metadata, track->title, sizeof(track->title));
    extract_track_language(stream->metadata, track->language, sizeof(track->language));

    /* Generate default title if empty */
    if (track->title[0] == '\0') {
        const char *codec_name = avcodec_get_name(cp->codec_id);
        if (track->channels >= 8)
            snprintf(track->title, sizeof(track->title), "%s 7.1", codec_name);
        else if (track->channels >= 6)
            snprintf(track->title, sizeof(track->title), "%s 5.1", codec_name);
        else if (track->channels >= 2)
            snprintf(track->title, sizeof(track->title), "%s Stereo", codec_name);
        else
            snprintf(track->title, sizeof(track->title), "%s Mono", codec_name);
    }

    /* Detect Atmos (TrueHD carrier) */
    if (cp->codec_id == AV_CODEC_ID_TRUEHD) {
        track->is_atmos = 1;
        track->is_lossless = 1;
    }

    /* Detect lossless formats */
    if (cp->codec_id == AV_CODEC_ID_FLAC ||
        cp->codec_id == AV_CODEC_ID_TRUEHD ||
        cp->codec_id == AV_CODEC_ID_DTS) {
        /* DTS-HD MA has profile DTS_HD_MA */
        track->is_lossless = 1;
    }

    /* Check default disposition */
    if (stream->disposition & AV_DISPOSITION_DEFAULT) {
        track->is_default = 1;
    }

    fprintf(stderr, "[videoplayer] Audio #%d: %s [%s] %d ch @ %d Hz, atmos=%d\n",
            track->index, track->title, track->language,
            track->channels, track->sample_rate, track->is_atmos);

    vp->audio_track_count++;
    return 0;
}

static int probe_subtitle_stream(VideoPlayer *vp, int stream_idx)
{
    AVStream *stream = vp->fmt_ctx->streams[stream_idx];
    AVCodecParameters *cp = stream->codecpar;

    if (vp->subtitle_track_count >= VP_MAX_SUBTITLE_TRACKS)
        return -1;

    SubtitleTrack *track = &vp->subtitle_tracks[vp->subtitle_track_count];
    memset(track, 0, sizeof(*track));

    track->stream_index = stream_idx;
    track->index = vp->subtitle_track_count;
    track->codec_id = cp->codec_id;

    extract_track_title(stream->metadata, track->title, sizeof(track->title));
    extract_track_language(stream->metadata, track->language, sizeof(track->language));

    /* Detect text-based subtitles */
    if (cp->codec_id == AV_CODEC_ID_ASS ||
        cp->codec_id == AV_CODEC_ID_SSA ||
        cp->codec_id == AV_CODEC_ID_SUBRIP ||
        cp->codec_id == AV_CODEC_ID_SRT ||
        cp->codec_id == AV_CODEC_ID_WEBVTT ||
        cp->codec_id == AV_CODEC_ID_TEXT ||
        cp->codec_id == AV_CODEC_ID_MOV_TEXT) {
        track->is_text_based = 1;
    }

    /* Check dispositions */
    if (stream->disposition & AV_DISPOSITION_DEFAULT) {
        track->is_default = 1;
    }
    if (stream->disposition & AV_DISPOSITION_FORCED) {
        track->is_forced = 1;
    }

    /* Generate default title if empty */
    if (track->title[0] == '\0') {
        const char *lang = track->language[0] ? track->language : "und";
        const char *type = track->is_text_based ? "Text" : "Bitmap";
        snprintf(track->title, sizeof(track->title), "%s (%s)", lang, type);
    }

    fprintf(stderr, "[videoplayer] Subtitle #%d: %s [%s] text=%d forced=%d\n",
            track->index, track->title, track->language,
            track->is_text_based, track->is_forced);

    vp->subtitle_track_count++;
    return 0;
}

/* ================================================================
 *  File Opening
 * ================================================================ */

int videoplayer_open(VideoPlayer *vp, const char *filepath)
{
    int ret;

    if (!vp || !filepath)
        return -1;

    /* Close any existing file */
    videoplayer_close(vp);

    /* Store filepath */
    snprintf(vp->filepath, sizeof(vp->filepath), "%s", filepath);
    vp->state = VP_STATE_LOADING;

    /* Detect local vs HTTP file for tuning probe/buffer parameters.
     * Localhost HTTP (Jellyfin, Plex, etc.) gets the same fast treatment as
     * local files: data is available instantly, no reconnect needed. */
    int is_http = (strncmp(filepath, "http://", 7) == 0 ||
                   strncmp(filepath, "https://", 8) == 0);
    int is_localhost = is_http &&
        (strstr(filepath, "://localhost") != NULL ||
         strstr(filepath, "://127.0.0.1") != NULL ||
         strstr(filepath, "://[::1]") != NULL);
    vp->is_local_file = !is_http || is_localhost;

    /* Open file - set HTTP-specific options for streaming resilience.
     * Localhost doesn't need reconnect/timeout — it only adds latency. */
    AVDictionary *opts = NULL;
    if (is_http && !is_localhost) {
        av_dict_set(&opts, "reconnect", "1", 0);
        av_dict_set(&opts, "reconnect_streamed", "1", 0);
        av_dict_set(&opts, "reconnect_delay_max", "2", 0);
        av_dict_set(&opts, "rw_timeout", "5000000", 0);  /* 5s I/O timeout */
        av_dict_set(&opts, "multiple_requests", "1", 0);  /* Reuse HTTP connection */
        fprintf(stderr, "[videoplayer] HTTP stream: enabling reconnect + buffering options\n");
    } else if (is_localhost) {
        av_dict_set(&opts, "multiple_requests", "1", 0);
        av_dict_set(&opts, "rw_timeout", "5000000", 0);  /* 5s I/O timeout */
        fprintf(stderr, "[videoplayer] Localhost stream: fast-start mode\n");
    }

    ret = avformat_open_input(&vp->fmt_ctx, filepath, NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        snprintf(vp->error_msg, sizeof(vp->error_msg),
                 "Failed to open file: %s", errbuf);
        vp->state = VP_STATE_ERROR;
        return -1;
    }

    /* For local files, reduce probe time. The default analyzeduration (5s)
     * is designed for unreliable network streams. Local files have all data
     * available instantly, so 500ms is plenty for format/codec detection. */
    if (vp->is_local_file) {
        vp->fmt_ctx->max_analyze_duration = 500000;  /* 0.5s */
        vp->fmt_ctx->probesize = 500000;              /* 500KB */
    }

    /* Find stream info */
    ret = avformat_find_stream_info(vp->fmt_ctx, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        snprintf(vp->error_msg, sizeof(vp->error_msg),
                 "Failed to find stream info: %s", errbuf);
        avformat_close_input(&vp->fmt_ctx);
        vp->state = VP_STATE_ERROR;
        return -1;
    }

    /* Get duration */
    if (vp->fmt_ctx->duration > 0) {
        vp->duration_us = vp->fmt_ctx->duration;
    } else {
        vp->duration_us = 0;
    }

    /* Find streams */
    int video_stream = -1;
    int best_audio_stream = -1;
    int best_audio_channels = 0;

    for (unsigned i = 0; i < vp->fmt_ctx->nb_streams; i++) {
        AVCodecParameters *cp = vp->fmt_ctx->streams[i]->codecpar;

        switch (cp->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (video_stream < 0) {
                video_stream = i;
                probe_video_stream(vp, i);
            }
            break;

        case AVMEDIA_TYPE_AUDIO:
            probe_audio_stream(vp, i);
            /* Track best audio for default selection */
            if (cp->ch_layout.nb_channels > best_audio_channels) {
                best_audio_channels = cp->ch_layout.nb_channels;
                best_audio_stream = vp->audio_track_count - 1;
            }
            break;

        case AVMEDIA_TYPE_SUBTITLE:
            probe_subtitle_stream(vp, i);
            break;

        default:
            break;
        }
    }

    if (video_stream < 0) {
        snprintf(vp->error_msg, sizeof(vp->error_msg), "No video stream found");
        avformat_close_input(&vp->fmt_ctx);
        vp->state = VP_STATE_ERROR;
        return -1;
    }

    /* Select default audio track */
    vp->current_audio_track = 0;
    for (int i = 0; i < vp->audio_track_count; i++) {
        if (vp->audio_tracks[i].is_default) {
            vp->current_audio_track = i;
            break;
        }
    }
    /* Fall back to best audio if no default */
    if (vp->current_audio_track == 0 && best_audio_stream >= 0) {
        vp->current_audio_track = best_audio_stream;
    }

    /* Select default subtitle track (-1 = off) */
    vp->current_subtitle_track = -1;
    for (int i = 0; i < vp->subtitle_track_count; i++) {
        if (vp->subtitle_tracks[i].is_forced) {
            vp->current_subtitle_track = i;
            break;
        }
    }

    /* Build HDR tonemapping LUT if content is HDR */
    if (vp->video.is_hdr) {
        videoplayer_build_hdr_lut(vp);
        fprintf(stderr, "[videoplayer] HDR content detected: %s, %d-bit\n",
                vp->video.color_trc == AVCOL_TRC_ARIB_STD_B67 ? "HLG" : "PQ/HDR10",
                vp->video.bit_depth);
    }

    /* Initialize decoders */
    ret = init_video_decoder(vp);
    if (ret < 0) {
        avformat_close_input(&vp->fmt_ctx);
        vp->state = VP_STATE_ERROR;
        return -1;
    }

    ret = init_audio_decoder(vp);
    if (ret < 0) {
        fprintf(stderr, "[videoplayer] Warning: Audio decoder init failed\n");
        /* Continue without audio */
    } else {
        /* Initialize PipeWire audio output */
        ret = videoplayer_audio_init(vp);
        if (ret < 0) {
            fprintf(stderr, "[videoplayer] Warning: PipeWire audio init failed\n");
        }
    }

    if (vp->current_subtitle_track >= 0) {
        ret = init_subtitle_decoder(vp);
        if (ret < 0) {
            fprintf(stderr, "[videoplayer] Warning: Subtitle decoder init failed\n");
            vp->current_subtitle_track = -1;
        }
    }

    /* Initialize frame queue mutex/cond */
    pthread_mutex_init(&vp->frame_mutex, NULL);
    pthread_cond_init(&vp->frame_cond, NULL);

    /* Initialize packet queue */
    pthread_mutex_init(&vp->pkt_mutex, NULL);
    pthread_cond_init(&vp->pkt_not_empty, NULL);
    pthread_cond_init(&vp->pkt_not_full, NULL);
    vp->pkt_read_idx = 0;
    vp->pkt_write_idx = 0;
    vp->pkts_queued = 0;
    vp->demux_eof = 0;
    vp->seek_flush_done = 0;
    for (int i = 0; i < VP_PACKET_QUEUE_SIZE; i++) {
        vp->packet_queue[i].pkt = av_packet_alloc();
        vp->packet_queue[i].ready = 0;
    }

    /* Start demux thread (I/O) */
    vp->demux_running = 1;
    ret = pthread_create(&vp->demux_thread, NULL, demux_thread_func, vp);
    if (ret != 0) {
        snprintf(vp->error_msg, sizeof(vp->error_msg),
                 "Failed to create demux thread: %s", strerror(ret));
        vp->demux_running = 0;
        vp->state = VP_STATE_ERROR;
        return -1;
    }

    /* Start decode thread */
    vp->decode_running = 1;
    ret = pthread_create(&vp->decode_thread, NULL, decode_thread_func, vp);
    if (ret != 0) {
        snprintf(vp->error_msg, sizeof(vp->error_msg),
                 "Failed to create decode thread: %s", strerror(ret));
        vp->decode_running = 0;
        /* Also stop demux thread */
        vp->demux_running = 0;
        pthread_cond_broadcast(&vp->pkt_not_full);
        pthread_join(vp->demux_thread, NULL);
        vp->state = VP_STATE_ERROR;
        return -1;
    }

    /* Set frame interval from actual video FPS now that it's known.
     * videoplayer_setup_display_mode() may have been called before open()
     * when video_fps was still 0, leaving frame_interval_ns at a fallback. */
    if (vp->video_fps > 0)
        vp->frame_interval_ns = (uint64_t)(1000000000.0 / vp->video_fps);

    /* Recalculate frame pacing with correct FPS if display Hz is known */
    if (vp->display_hz > 0)
        videoplayer_setup_display_mode(vp, vp->display_hz, vp->frame_repeat_mode == 0);

    fprintf(stderr, "[videoplayer] Opened: %s (duration: %.2fs)\n",
            filepath, vp->duration_us / 1000000.0);

    /* Start in BUFFERING state — present_frame() waits for the decode thread
     * to fill the frame queue to VP_FRAME_QUEUE_SIZE*3/4 before transitioning
     * to PLAYING. This gives the decode thread a large head start so the queue
     * has enough frames to absorb HTTP streaming jitter and decode variance.
     * Audio is also deferred until the buffer is ready, so A/V start together. */
    vp->state = VP_STATE_BUFFERING;

    /* Open debug log for this playback session */
    vp->debug_frame_count = 0;
    vp->debug_vsync_count = 0;
    vp->debug_last_present_ns = 0;
    vp->debug_total_skips = 0;
    vp->debug_total_empty = 0;
    vp->debug_total_snaps = 0;
    vp->debug_audio_underruns = 0;
    if (!vp->debug_log) {
        const char *home = getenv("HOME");
        if (home) {
            char logpath[PATH_MAX];
            snprintf(logpath, sizeof(logpath), "%s/nixlytile/videoplayer_debug.log", home);
            vp->debug_log = fopen(logpath, "w");
            if (vp->debug_log) {
                fprintf(vp->debug_log, "# Nixlytile Video Player Debug Log\n");
                fprintf(vp->debug_log, "# File: %s\n", filepath);
                fprintf(vp->debug_log, "# Video: %dx%d @ %.3f fps, HW=%d\n",
                        vp->video.width, vp->video.height, vp->video_fps, vp->hw_accel);
                fprintf(vp->debug_log, "# Display: %.2f Hz, frame_interval=%lu ns, pacing_mode=%d\n",
                        vp->display_hz, (unsigned long)vp->frame_interval_ns, vp->frame_repeat_mode);
                fprintf(vp->debug_log, "# frame | state  | q=queued | avsync | pos_ms | v=vsyncs | dt=actual/expected us\n");
                fflush(vp->debug_log);
                fprintf(stderr, "[videoplayer] Debug log: %s\n", logpath);
            }
        }
    }

    return 0;
}

void videoplayer_close(VideoPlayer *vp)
{
    if (!vp)
        return;

    /* Stop both threads. We check the flags before clearing to know
     * which threads were actually started and need joining. */
    {
        int had_demux = vp->demux_running;
        int had_decode = vp->decode_running;
        vp->demux_running = 0;
        vp->decode_running = 0;

        /* Wake up all waiting threads */
        pthread_cond_broadcast(&vp->frame_cond);
        pthread_cond_broadcast(&vp->pkt_not_empty);
        pthread_cond_broadcast(&vp->pkt_not_full);

        if (had_demux)
            pthread_join(vp->demux_thread, NULL);
        if (had_decode)
            pthread_join(vp->decode_thread, NULL);
    }

    /* Release the scene node's buffer reference BEFORE freeing queue buffers.
     * This ensures the buffer's destroy callback can return pixel data to the
     * pool while the pool is still valid. */
    if (vp->frame_node)
        wlr_scene_buffer_set_buffer(vp->frame_node, NULL);

    /* Free any remaining frame queue buffers before destroying the mutex */
    for (int i = 0; i < VP_FRAME_QUEUE_SIZE; i++) {
        if (vp->frame_queue[i].buffer) {
            wlr_buffer_drop(vp->frame_queue[i].buffer);
            vp->frame_queue[i].buffer = NULL;
        }
        vp->frame_queue[i].ready = 0;
    }

    /* Free buffer pool (after all buffers have been returned/freed above) */
    pthread_mutex_lock(&vp->buffer_pool.lock);
    for (int i = 0; i < vp->buffer_pool.count; i++) {
        free(vp->buffer_pool.data[i]);
    }
    vp->buffer_pool.count = 0;
    vp->buffer_pool.alloc_size = 0;
    pthread_mutex_unlock(&vp->buffer_pool.lock);

    /* Clean up frame queue */
    pthread_mutex_destroy(&vp->frame_mutex);
    pthread_cond_destroy(&vp->frame_cond);

    /* Clean up packet queue */
    for (int i = 0; i < VP_PACKET_QUEUE_SIZE; i++) {
        if (vp->packet_queue[i].pkt) {
            av_packet_free(&vp->packet_queue[i].pkt);
        }
        vp->packet_queue[i].ready = 0;
    }
    vp->pkts_queued = 0;
    vp->pkt_read_idx = 0;
    vp->pkt_write_idx = 0;
    pthread_mutex_destroy(&vp->pkt_mutex);
    pthread_cond_destroy(&vp->pkt_not_empty);
    pthread_cond_destroy(&vp->pkt_not_full);

    /* Cleanup audio output (PipeWire, ring buffer, resampler).
     * Must shut down cmd thread first — it holds PipeWire locks. */
    videoplayer_audio_cmd_shutdown(vp);
    videoplayer_audio_cleanup(vp);

    /* Free frames */
    av_frame_free(&vp->decode_frame);
    av_frame_free(&vp->sw_frame);
    av_frame_free(&vp->audio_frame);

    /* Free sws context */
    if (vp->sws_ctx) {
        sws_freeContext(vp->sws_ctx);
        vp->sws_ctx = NULL;
    }

    /* Free HDR tonemapping resources */
    videoplayer_cleanup_hdr_lut(vp);

    /* Free codec contexts */
    avcodec_free_context(&vp->video_codec_ctx);
    avcodec_free_context(&vp->audio_codec_ctx);
    avcodec_free_context(&vp->subtitle_codec_ctx);

    /* Free HW contexts */
    av_buffer_unref(&vp->hw_device_ctx);
    av_buffer_unref(&vp->hw_frames_ctx);

    /* Close format context */
    avformat_close_input(&vp->fmt_ctx);

    /* Close debug log */
    if (vp->debug_log) {
        fprintf(vp->debug_log, "# Closed after %d frames | skips=%d empty=%d snaps=%d underruns=%d\n",
                vp->debug_frame_count, vp->debug_total_skips, vp->debug_total_empty,
                vp->debug_total_snaps, vp->debug_audio_underruns);
        fclose(vp->debug_log);
        vp->debug_log = NULL;
    }

    /* Reset state */
    vp->state = VP_STATE_IDLE;
    vp->filepath[0] = '\0';
    vp->duration_us = 0;
    vp->position_us = 0;
    vp->audio_track_count = 0;
    vp->subtitle_track_count = 0;
    vp->current_audio_track = 0;
    vp->current_subtitle_track = -1;
    vp->frames_queued = 0;
    vp->frame_read_idx = 0;
    vp->frame_write_idx = 0;
    vp->vaapi_format_rejected = 0;
    vp->hw_verified = 0;
    vp->demux_eof = 0;
    vp->seek_flush_done = 0;
    memset(&vp->video, 0, sizeof(vp->video));
}

/* ================================================================
 *  Video Decoder Initialization
 * ================================================================ */

static enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx,
                                            const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;
    VideoPlayer *vp = (VideoPlayer *)ctx->opaque;

    fprintf(stderr, "[videoplayer] VA-API format negotiation, offered formats:");
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        fprintf(stderr, " %s", av_get_pix_fmt_name(*p));
    }
    fprintf(stderr, "\n");

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI) {
            fprintf(stderr, "[videoplayer] VA-API format accepted\n");
            return *p;
        }
    }

    fprintf(stderr, "[videoplayer] VA-API format NOT in list, rejecting\n");
    if (vp)
        vp->vaapi_format_rejected = 1;
    return AV_PIX_FMT_NONE;
}

static int init_vaapi(VideoPlayer *vp)
{
    int ret;
    struct stat st;
    const char *device = NULL;

    /* Check which DRI render node exists */
    if (stat(VAAPI_DEVICE, &st) == 0) {
        device = VAAPI_DEVICE;
        fprintf(stderr, "[videoplayer] Found render node: %s\n", VAAPI_DEVICE);
    } else if (stat(VAAPI_DEVICE_ALT, &st) == 0) {
        device = VAAPI_DEVICE_ALT;
        fprintf(stderr, "[videoplayer] Found render node: %s\n", VAAPI_DEVICE_ALT);
    } else {
        fprintf(stderr, "[videoplayer] No DRI render node found (%s, %s)\n",
                VAAPI_DEVICE, VAAPI_DEVICE_ALT);
        return -1;
    }

    /* Set opaque for get_vaapi_format callback communication */
    vp->video_codec_ctx->opaque = vp;
    vp->vaapi_format_rejected = 0;
    vp->hw_verified = 0;

    /* Create HW device context */
    ret = av_hwdevice_ctx_create(&vp->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                  device, NULL, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[videoplayer] VA-API init failed on %s: %s\n", device, errbuf);
        return -1;
    }

    vp->video_codec_ctx->hw_device_ctx = av_buffer_ref(vp->hw_device_ctx);
    vp->video_codec_ctx->get_format = get_vaapi_format;
    vp->hw_accel = VP_HW_VAAPI;

    fprintf(stderr, "[videoplayer] VA-API initialized on %s\n", device);
    return 0;
}

static int init_nvdec(VideoPlayer *vp)
{
    int ret;

    ret = av_hwdevice_ctx_create(&vp->hw_device_ctx, AV_HWDEVICE_TYPE_CUDA,
                                  NULL, NULL, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[videoplayer] NVDEC init failed: %s\n", errbuf);
        return -1;
    }

    vp->video_codec_ctx->hw_device_ctx = av_buffer_ref(vp->hw_device_ctx);
    vp->hw_accel = VP_HW_NVDEC;

    fprintf(stderr, "[videoplayer] NVDEC initialized\n");
    return 0;
}

static int init_software_decoder(VideoPlayer *vp)
{
    vp->hw_accel = VP_HW_NONE;
    fprintf(stderr, "[videoplayer] Using software decoding\n");
    return 0;
}

static int init_video_decoder(VideoPlayer *vp)
{
    int ret;
    VideoTrack *video = &vp->video;
    AVStream *stream = vp->fmt_ctx->streams[video->stream_index];
    const AVCodec *codec;

    /* Find decoder */
    codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        snprintf(vp->error_msg, sizeof(vp->error_msg),
                 "Unsupported video codec: %d", stream->codecpar->codec_id);
        return -1;
    }

    /* Allocate context */
    vp->video_codec_ctx = avcodec_alloc_context3(codec);
    if (!vp->video_codec_ctx) {
        snprintf(vp->error_msg, sizeof(vp->error_msg), "Failed to allocate video codec context");
        return -1;
    }

    ret = avcodec_parameters_to_context(vp->video_codec_ctx, stream->codecpar);
    if (ret < 0) {
        snprintf(vp->error_msg, sizeof(vp->error_msg), "Failed to copy codec parameters");
        return -1;
    }

    /* Try hardware acceleration: VA-API -> NVDEC -> Software */
    if (init_vaapi(vp) != 0) {
        if (init_nvdec(vp) != 0) {
            init_software_decoder(vp);
        }
    }

    /* Set thread count for software decoding */
    if (vp->hw_accel == VP_HW_NONE) {
        vp->video_codec_ctx->thread_count = 0;  /* Auto-detect */
        vp->video_codec_ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }

    /* Open decoder */
    ret = avcodec_open2(vp->video_codec_ctx, codec, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        snprintf(vp->error_msg, sizeof(vp->error_msg),
                 "Failed to open video decoder: %s", errbuf);
        return -1;
    }

    /* Store software pixel format for HW decoding */
    if (vp->hw_accel != VP_HW_NONE) {
        video->sw_pix_fmt = vp->video_codec_ctx->sw_pix_fmt;
    }

    /* Allocate frames */
    vp->decode_frame = av_frame_alloc();
    vp->sw_frame = av_frame_alloc();
    if (!vp->decode_frame || !vp->sw_frame) {
        snprintf(vp->error_msg, sizeof(vp->error_msg), "Failed to allocate video frames");
        return -1;
    }

    return 0;
}

/* ================================================================
 *  Audio Decoder Initialization
 * ================================================================ */

static int init_audio_decoder(VideoPlayer *vp)
{
    int ret;

    if (vp->audio_track_count == 0)
        return -1;

    AudioTrack *track = &vp->audio_tracks[vp->current_audio_track];
    AVStream *stream = vp->fmt_ctx->streams[track->stream_index];
    const AVCodec *codec;

    codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "[videoplayer] Unsupported audio codec: %d\n",
                stream->codecpar->codec_id);
        return -1;
    }

    vp->audio_codec_ctx = avcodec_alloc_context3(codec);
    if (!vp->audio_codec_ctx)
        return -1;

    ret = avcodec_parameters_to_context(vp->audio_codec_ctx, stream->codecpar);
    if (ret < 0)
        return -1;

    ret = avcodec_open2(vp->audio_codec_ctx, codec, NULL);
    if (ret < 0)
        return -1;

    vp->audio_frame = av_frame_alloc();
    if (!vp->audio_frame)
        return -1;

    fprintf(stderr, "[videoplayer] Audio decoder initialized: %s\n",
            avcodec_get_name(stream->codecpar->codec_id));

    return 0;
}

/* ================================================================
 *  Subtitle Decoder Initialization
 * ================================================================ */

/* Forward declaration for subtitle init */
extern int videoplayer_subtitle_init(VideoPlayer *vp);

static int init_subtitle_decoder(VideoPlayer *vp)
{
    int ret;

    if (vp->current_subtitle_track < 0)
        return -1;

    SubtitleTrack *track = &vp->subtitle_tracks[vp->current_subtitle_track];
    AVStream *stream = vp->fmt_ctx->streams[track->stream_index];
    const AVCodec *codec;

    codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "[videoplayer] Unsupported subtitle codec: %d\n",
                stream->codecpar->codec_id);
        return -1;
    }

    vp->subtitle_codec_ctx = avcodec_alloc_context3(codec);
    if (!vp->subtitle_codec_ctx)
        return -1;

    ret = avcodec_parameters_to_context(vp->subtitle_codec_ctx, stream->codecpar);
    if (ret < 0)
        return -1;

    ret = avcodec_open2(vp->subtitle_codec_ctx, codec, NULL);
    if (ret < 0)
        return -1;

    /* Initialize libass */
    if (videoplayer_subtitle_init(vp) < 0) {
        fprintf(stderr, "[videoplayer] Warning: libass init failed\n");
    }

    /* Create libass track and load codec extradata (ASS header) */
    if (vp->subtitle.library && vp->subtitle.renderer) {
        vp->subtitle.track = ass_new_track(vp->subtitle.library);
        if (vp->subtitle.track) {
            /* Load ASS header from codec extradata */
            if (stream->codecpar->extradata && stream->codecpar->extradata_size > 0) {
                ass_process_codec_private(vp->subtitle.track,
                                          (char *)stream->codecpar->extradata,
                                          stream->codecpar->extradata_size);
            }

            /* Set default style for non-ASS subtitles */
            if (stream->codecpar->codec_id == AV_CODEC_ID_SUBRIP ||
                stream->codecpar->codec_id == AV_CODEC_ID_SRT ||
                stream->codecpar->codec_id == AV_CODEC_ID_TEXT) {
                /* Create default ASS header for SRT */
                const char *default_header =
                    "[Script Info]\n"
                    "ScriptType: v4.00+\n"
                    "PlayResX: 1920\n"
                    "PlayResY: 1080\n"
                    "\n"
                    "[V4+ Styles]\n"
                    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
                    "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
                    "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
                    "Alignment, MarginL, MarginR, MarginV, Encoding\n"
                    "Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
                    "0,0,0,0,100,100,0,0,1,2,1,2,20,20,40,1\n"
                    "\n"
                    "[Events]\n"
                    "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
                ass_process_codec_private(vp->subtitle.track,
                                          (char *)default_header,
                                          strlen(default_header));
            }
        }
    }

    fprintf(stderr, "[videoplayer] Subtitle decoder initialized: %s\n",
            avcodec_get_name(stream->codecpar->codec_id));

    return 0;
}

/* ================================================================
 *  Packet Queue (demux → decode)
 * ================================================================ */

/* Put a packet into the queue. Called from demux thread.
 * Returns 0 on success, -1 if shutting down. */
static int packet_queue_put(VideoPlayer *vp, AVPacket *pkt)
{
    pthread_mutex_lock(&vp->pkt_mutex);

    /* Wait until space is available.
     * Also break out when a seek is requested — otherwise the demux thread
     * stays stuck here while the decode thread waits for seek_flush_done,
     * causing a deadlock (decode waits for demux to seek, demux waits for
     * decode to drain packets). */
    while (vp->pkts_queued >= VP_PACKET_QUEUE_SIZE &&
           vp->demux_running && !vp->seek_requested) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000;  /* 50ms timeout */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&vp->pkt_not_full, &vp->pkt_mutex, &ts);
    }

    if (!vp->demux_running || vp->seek_requested) {
        pthread_mutex_unlock(&vp->pkt_mutex);
        return -1;
    }

    PacketQueueEntry *e = &vp->packet_queue[vp->pkt_write_idx];
    av_packet_move_ref(e->pkt, pkt);
    e->ready = 1;

    vp->pkt_write_idx = (vp->pkt_write_idx + 1) % VP_PACKET_QUEUE_SIZE;
    vp->pkts_queued++;

    pthread_cond_signal(&vp->pkt_not_empty);
    pthread_mutex_unlock(&vp->pkt_mutex);
    return 0;
}

/* Get a packet from the queue. Called from decode thread.
 * Returns 0 on success with pkt filled, 1 on EOF, -1 on shutdown. */
static int packet_queue_get(VideoPlayer *vp, AVPacket *pkt)
{
    pthread_mutex_lock(&vp->pkt_mutex);

    while (vp->pkts_queued == 0 && vp->decode_running && !vp->seek_requested) {
        if (vp->demux_eof) {
            pthread_mutex_unlock(&vp->pkt_mutex);
            return 1;  /* EOF */
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000;  /* 50ms timeout */
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&vp->pkt_not_empty, &vp->pkt_mutex, &ts);
    }

    if (!vp->decode_running) {
        pthread_mutex_unlock(&vp->pkt_mutex);
        return -1;
    }

    /* Re-check EOF after wait (demux may have set it while we were waiting) */
    if (vp->pkts_queued == 0 && vp->demux_eof) {
        pthread_mutex_unlock(&vp->pkt_mutex);
        return 1;
    }

    if (vp->pkts_queued == 0) {
        pthread_mutex_unlock(&vp->pkt_mutex);
        return 0;  /* Spurious wakeup, caller retries */
    }

    PacketQueueEntry *e = &vp->packet_queue[vp->pkt_read_idx];
    av_packet_move_ref(pkt, e->pkt);
    e->ready = 0;

    vp->pkt_read_idx = (vp->pkt_read_idx + 1) % VP_PACKET_QUEUE_SIZE;
    vp->pkts_queued--;

    pthread_cond_signal(&vp->pkt_not_full);
    pthread_mutex_unlock(&vp->pkt_mutex);
    return 0;
}

/* Flush all packets in the queue. Called with pkt_mutex held. */
static void packet_queue_flush_locked(VideoPlayer *vp)
{
    while (vp->pkts_queued > 0) {
        PacketQueueEntry *e = &vp->packet_queue[vp->pkt_read_idx];
        if (e->ready) {
            av_packet_unref(e->pkt);
            e->ready = 0;
        }
        vp->pkt_read_idx = (vp->pkt_read_idx + 1) % VP_PACKET_QUEUE_SIZE;
        vp->pkts_queued--;
    }
    vp->pkt_read_idx = 0;
    vp->pkt_write_idx = 0;
    pthread_cond_broadcast(&vp->pkt_not_full);
}

/* ================================================================
 *  Demux Thread (I/O)
 * ================================================================ */

static void *demux_thread_func(void *arg)
{
    VideoPlayer *vp = arg;
    AVPacket *pkt = av_packet_alloc();
    int ret;
    int read_error_count = 0;

    if (!pkt) {
        fprintf(stderr, "[videoplayer] Demux: failed to allocate packet\n");
        return NULL;
    }

    fprintf(stderr, "[videoplayer] Demux thread started\n");

    while (vp->demux_running) {
        /* Handle seek request — demux thread owns av_seek_frame.
         * After completing the seek, wait for the decode thread to clear
         * seek_requested before continuing. Without this, the demux thread
         * loops back and re-enters the seek block (seek_requested is still
         * set because the decode thread hasn't processed it yet), causing
         * redundant seeks that flush the newly-read post-seek packets. */
        if (vp->seek_requested && !vp->seek_flush_done) {
            int64_t seek_target = vp->seek_target_us;
            int64_t seek_ts = av_rescale_q(seek_target, AV_TIME_BASE_Q,
                                           vp->fmt_ctx->streams[vp->video.stream_index]->time_base);

            /* Flush packet queue before seeking */
            pthread_mutex_lock(&vp->pkt_mutex);
            packet_queue_flush_locked(vp);
            pthread_mutex_unlock(&vp->pkt_mutex);

            ret = av_seek_frame(vp->fmt_ctx, vp->video.stream_index, seek_ts,
                                AVSEEK_FLAG_BACKWARD);
            if (ret < 0) {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                fprintf(stderr, "[videoplayer] Demux: seek failed: %s\n", errbuf);
            }

            vp->demux_eof = 0;
            read_error_count = 0;

            /* Signal decode thread that seek flush is done */
            vp->seek_flush_done = 1;
            pthread_cond_broadcast(&vp->pkt_not_empty);
        }

        /* Wait for decode thread to finish processing the seek before
         * reading new packets. This prevents redundant seeks and ensures
         * the decode thread has flushed its codecs and frame queue. */
        if (vp->seek_requested) {
            usleep(1000);
            continue;
        }

        /* If at EOF, wait for seek or shutdown */
        if (vp->demux_eof) {
            usleep(50000);
            continue;
        }

        /* Wait if packet queue is full */
        pthread_mutex_lock(&vp->pkt_mutex);
        if (vp->pkts_queued >= VP_PACKET_QUEUE_SIZE) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&vp->pkt_not_full, &vp->pkt_mutex, &ts);
        }
        pthread_mutex_unlock(&vp->pkt_mutex);

        if (!vp->demux_running)
            break;

        /* Read packet — the I/O-heavy operation, now isolated from decode */
        ret = av_read_frame(vp->fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                fprintf(stderr, "[videoplayer] Demux: EOF reached\n");
                vp->demux_eof = 1;
                pthread_cond_broadcast(&vp->pkt_not_empty);
                continue;
            }
            read_error_count++;
            if (read_error_count >= 100) {
                fprintf(stderr, "[videoplayer] Demux: too many read errors (%d), stopping\n",
                        read_error_count);
                vp->demux_eof = 1;
                pthread_cond_broadcast(&vp->pkt_not_empty);
                break;
            }
            usleep(1000);
            continue;
        }

        read_error_count = 0;

        /* Put packet into queue for decode thread.
         * Returns -1 on shutdown OR seek request (to unblock the demux
         * thread so it can process the seek). Only exit the thread on
         * actual shutdown; on seek, discard the packet and loop back. */
        if (packet_queue_put(vp, pkt) < 0) {
            av_packet_unref(pkt);
            if (!vp->demux_running)
                break;
            continue;  /* Seek requested — loop back to handle it */
        }
    }

    av_packet_free(&pkt);
    fprintf(stderr, "[videoplayer] Demux thread stopped\n");
    return NULL;
}

/* ================================================================
 *  Decode Thread
 * ================================================================ */

static int transfer_hw_frame(VideoPlayer *vp, AVFrame *hw_frame, AVFrame *sw_frame)
{
    int ret;

    sw_frame->format = vp->video.sw_pix_fmt;
    ret = av_hwframe_transfer_data(sw_frame, hw_frame, 0);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[videoplayer] HW frame transfer failed: %s\n", errbuf);
        return -1;
    }

    ret = av_frame_copy_props(sw_frame, hw_frame);
    if (ret < 0)
        return -1;

    return 0;
}

/* Helper: drain all available video frames from the decoder and queue them.
 * BLOCKING: waits for frame queue space (up to 50ms) when the queue is full.
 * This throttles the decode thread to the render thread's consumption rate,
 * preventing the VAAPI decoder from racing ahead and discarding frames.
 * The audio ring buffer has 0.5-4s of pre-buffered data, so the ~42ms wait
 * (one frame at 24fps) won't cause PipeWire underruns. */
static void drain_video_frames(VideoPlayer *vp)
{
    int ret;

    while (1) {
        ret = avcodec_receive_frame(vp->video_codec_ctx, vp->decode_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[videoplayer] receive_frame error: %s\n", errbuf);
            break;
        }

        /* Post-first-frame HW acceleration verification */
        if (!vp->hw_verified && vp->hw_accel == VP_HW_VAAPI) {
            vp->hw_verified = 1;
            if (vp->decode_frame->format != AV_PIX_FMT_VAAPI) {
                fprintf(stderr, "[videoplayer] VA-API NOT active! "
                        "Frame format=%s (expected vaapi), falling back to software\n",
                        av_get_pix_fmt_name(vp->decode_frame->format));
                vp->hw_accel = VP_HW_NONE;
                av_buffer_unref(&vp->hw_device_ctx);
            } else {
                fprintf(stderr, "[videoplayer] VA-API confirmed active "
                        "(frame format=%s)\n",
                        av_get_pix_fmt_name(vp->decode_frame->format));
            }
        }
        if (!vp->hw_verified && vp->hw_accel == VP_HW_NVDEC) {
            vp->hw_verified = 1;
            if (vp->decode_frame->format != AV_PIX_FMT_CUDA) {
                fprintf(stderr, "[videoplayer] NVDEC NOT active! "
                        "Frame format=%s (expected cuda), falling back to software\n",
                        av_get_pix_fmt_name(vp->decode_frame->format));
                vp->hw_accel = VP_HW_NONE;
                av_buffer_unref(&vp->hw_device_ctx);
            } else {
                fprintf(stderr, "[videoplayer] NVDEC confirmed active "
                        "(frame format=%s)\n",
                        av_get_pix_fmt_name(vp->decode_frame->format));
            }
        }

        AVFrame *frame_to_use = vp->decode_frame;

        /* Transfer HW frame to system memory */
        if (vp->hw_accel != VP_HW_NONE &&
            (vp->decode_frame->format == AV_PIX_FMT_VAAPI ||
             vp->decode_frame->format == AV_PIX_FMT_CUDA)) {
            if (transfer_hw_frame(vp, vp->decode_frame, vp->sw_frame) < 0) {
                av_frame_unref(vp->decode_frame);
                continue;
            }
            frame_to_use = vp->sw_frame;
        }

        /* Wait for frame queue space BEFORE the expensive YUV→BGRA conversion.
         * Blocking here is essential: without it, the VAAPI decoder races
         * through packets much faster than the render thread consumes frames,
         * discarding ~23 of every 24 decoded frames. This causes ~1-second
         * PTS gaps in the frame queue, making video play at ~24x speed.
         *
         * The wait is typically <42ms (one render frame at 24fps). The audio
         * ring buffer has 0.5-4s of pre-buffered data, so a brief wait here
         * won't cause PipeWire underruns. This also naturally throttles the
         * decode thread to the video framerate, keeping the packet queue
         * buffered against I/O jitter. */
        pthread_mutex_lock(&vp->frame_mutex);
        while (vp->frames_queued >= VP_FRAME_QUEUE_SIZE &&
               vp->decode_running && !vp->seek_requested) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 50000000;  /* 50ms timeout */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&vp->frame_cond, &vp->frame_mutex, &ts);
        }
        int has_space = vp->frames_queued < VP_FRAME_QUEUE_SIZE;
        pthread_mutex_unlock(&vp->frame_mutex);

        if (!has_space) {
            /* Still full — shutting down or seeking, discard to unblock */
            av_frame_unref(vp->decode_frame);
            av_frame_unref(vp->sw_frame);
            continue;
        }

        /* Calculate PTS in microseconds */
        int64_t pts_us = 0;
        if (frame_to_use->pts != AV_NOPTS_VALUE) {
            AVRational tb = vp->fmt_ctx->streams[vp->video.stream_index]->time_base;
            pts_us = av_rescale_q(frame_to_use->pts, tb, AV_TIME_BASE_Q);
        }

        /* Convert YUV→BGRA on the decode thread */
        struct wlr_buffer *buffer = videoplayer_create_frame_buffer(vp, frame_to_use);
        if (!buffer) {
            av_frame_unref(vp->decode_frame);
            av_frame_unref(vp->sw_frame);
            continue;
        }

        /* Queue the frame (non-blocking). Since only we write to the queue
         * and we confirmed space above, this always succeeds. */
        pthread_mutex_lock(&vp->frame_mutex);
        if (vp->frames_queued < VP_FRAME_QUEUE_SIZE) {
            VideoPlayerFrame *qf = &vp->frame_queue[vp->frame_write_idx];

            if (qf->buffer)
                wlr_buffer_drop(qf->buffer);

            qf->buffer = buffer;
            qf->pts_us = pts_us;
            qf->width = frame_to_use->width;
            qf->height = frame_to_use->height;
            qf->ready = 1;

            vp->frame_write_idx = (vp->frame_write_idx + 1) % VP_FRAME_QUEUE_SIZE;
            vp->frames_queued++;
            buffer = NULL;  /* Ownership transferred */
        }
        pthread_cond_signal(&vp->frame_cond);
        pthread_mutex_unlock(&vp->frame_mutex);

        /* Drop buffer if queue filled between check and lock (shouldn't happen
         * since only we write, but defensive). */
        if (buffer)
            wlr_buffer_drop(buffer);

        av_frame_unref(vp->decode_frame);
        av_frame_unref(vp->sw_frame);
    }
}

/* Drain all immediately available audio packets from the packet queue.
 * Called after processing a video packet to prevent sws_scale (which takes
 * several ms for 1080p) from starving the audio ring buffer.  Without this,
 * PipeWire's on_process callback finds the ring buffer empty ~75% of the time,
 * causing clicking/stutter.
 *
 * Only dequeues consecutive audio packets at the head of the queue; stops at
 * the first non-audio packet (video, subtitle) to preserve packet ordering. */
static void drain_pending_audio(VideoPlayer *vp, AVPacket *scratch_pkt)
{
    if (!vp->audio_codec_ctx || vp->audio_track_count == 0)
        return;

    int audio_stream = vp->audio_tracks[vp->current_audio_track].stream_index;

    while (vp->decode_running && !vp->seek_requested) {
        pthread_mutex_lock(&vp->pkt_mutex);

        if (vp->pkts_queued == 0) {
            pthread_mutex_unlock(&vp->pkt_mutex);
            break;
        }

        PacketQueueEntry *e = &vp->packet_queue[vp->pkt_read_idx];
        if (!e->ready || e->pkt->stream_index != audio_stream) {
            pthread_mutex_unlock(&vp->pkt_mutex);
            break;
        }

        /* Dequeue the audio packet */
        av_packet_move_ref(scratch_pkt, e->pkt);
        e->ready = 0;
        vp->pkt_read_idx = (vp->pkt_read_idx + 1) % VP_PACKET_QUEUE_SIZE;
        vp->pkts_queued--;
        pthread_cond_signal(&vp->pkt_not_full);
        pthread_mutex_unlock(&vp->pkt_mutex);

        /* Decode and queue to ring buffer */
        int ret = avcodec_send_packet(vp->audio_codec_ctx, scratch_pkt);
        if (ret >= 0) {
            while (avcodec_receive_frame(vp->audio_codec_ctx, vp->audio_frame) >= 0) {
                int qr = videoplayer_audio_queue_frame(vp, vp->audio_frame);
                if (qr == 1 && vp->decode_running) {
                    usleep(1000);
                    videoplayer_audio_queue_frame(vp, vp->audio_frame);
                }
                av_frame_unref(vp->audio_frame);
            }
        }
        av_packet_unref(scratch_pkt);
    }
}

static void *decode_thread_func(void *arg)
{
    VideoPlayer *vp = arg;
    AVPacket *pkt = av_packet_alloc();
    AVPacket *audio_scratch = av_packet_alloc();
    int ret;

    if (!pkt || !audio_scratch) {
        fprintf(stderr, "[videoplayer] Decode: failed to allocate packet\n");
        av_packet_free(&pkt);
        av_packet_free(&audio_scratch);
        return NULL;
    }

    fprintf(stderr, "[videoplayer] Decode thread started\n");

    while (vp->decode_running) {
        /* Handle seek: wait for demux thread to flush packet queue and seek */
        if (vp->seek_requested) {
            /* Wait for demux thread to complete the seek */
            while (!vp->seek_flush_done && vp->decode_running)
                usleep(1000);

            if (!vp->decode_running)
                break;

            /* Flush codecs */
            avcodec_flush_buffers(vp->video_codec_ctx);
            if (vp->audio_codec_ctx)
                avcodec_flush_buffers(vp->audio_codec_ctx);

            /* Clear frame queue */
            pthread_mutex_lock(&vp->frame_mutex);
            for (int i = 0; i < VP_FRAME_QUEUE_SIZE; i++) {
                if (vp->frame_queue[i].buffer) {
                    wlr_buffer_drop(vp->frame_queue[i].buffer);
                    vp->frame_queue[i].buffer = NULL;
                }
                vp->frame_queue[i].ready = 0;
            }
            vp->frames_queued = 0;
            vp->frame_read_idx = 0;
            vp->frame_write_idx = 0;
            pthread_mutex_unlock(&vp->frame_mutex);

            /* Pause audio and flush buffer for clean restart from new position.
             * audio_flush must come first to clear stream_interrupted, otherwise
             * audio_pause checks it and skips the PipeWire deactivation.
             * videoplayer_play() will re-activate audio after the buffer refills. */
            videoplayer_audio_flush(vp);
            videoplayer_audio_pause(vp);
            vp->av_sync_established = 0;

            vp->position_us = vp->seek_target_us;
            vp->seek_requested = 0;

            fprintf(stderr, "[videoplayer] Seek complete to %.2fs, rebuffering\n",
                    vp->seek_target_us / 1000000.0);
            continue;
        }

        /* Get packet from demux thread's packet queue.
         * IMPORTANT: We always get the packet first, regardless of frame queue
         * state. Only video packets need frame queue space; audio and subtitle
         * packets must always be processed to keep the audio ring buffer fed
         * and prevent A/V desync. */
        ret = packet_queue_get(vp, pkt);
        if (ret < 0)
            break;  /* Shutdown */
        if (ret == 1) {
            /* EOF — drain remaining frames, then pause */
            fprintf(stderr, "[videoplayer] Decode: EOF from demux\n");
            drain_video_frames(vp);
            if (vp->state == VP_STATE_BUFFERING)
                videoplayer_play(vp);
            while (vp->frames_queued > 0 && vp->decode_running) {
                usleep(50000);
            }
            vp->state = VP_STATE_PAUSED;
            fprintf(stderr, "[videoplayer] Playback finished, state -> paused\n");
            /* Wait for seek or shutdown at EOF */
            while (vp->decode_running && !vp->seek_requested)
                usleep(50000);
            continue;
        }
        if (pkt->size == 0) {
            av_packet_unref(pkt);
            continue;  /* Spurious wakeup with empty packet */
        }

        /* Video packet */
        if (pkt->stream_index == vp->video.stream_index) {
            /* Send packet to decoder with retry loop for EAGAIN.
             * EAGAIN means the decoder's output buffer is full — drain pending
             * frames and retry. Without proper retry, packets are silently lost,
             * causing only keyframes to be decoded (visible as ~500ms PTS gaps
             * in the frame queue). A single retry is NOT sufficient: VAAPI
             * decoders may need multiple drain cycles before accepting input
             * (e.g., when the hardware surface pool is temporarily exhausted). */
            int sent = 0;
            for (int attempts = 0; attempts < 50 && vp->decode_running && !vp->seek_requested; attempts++) {
                ret = avcodec_send_packet(vp->video_codec_ctx, pkt);
                if (ret == 0) {
                    sent = 1;
                    break;
                }
                if (ret == AVERROR(EAGAIN)) {
                    /* Decoder output buffer full — drain frames and retry */
                    drain_video_frames(vp);
                    continue;
                }
                /* Actual error — log and skip this packet */
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                fprintf(stderr, "[videoplayer] video send_packet error: %s (pts=%ld)\n",
                        errbuf, pkt->pts != AV_NOPTS_VALUE ? (long)pkt->pts : -1);
                break;
            }
            if (!sent) {
                av_packet_unref(pkt);
                continue;
            }

            /* Drain all available output frames */
            drain_video_frames(vp);

            /* After the CPU-intensive sws_scale, immediately drain any queued
             * audio packets so the ring buffer stays fed.  This is critical:
             * sws_scale for 1080p takes several ms, during which PipeWire
             * keeps consuming from the ring buffer with no new data arriving. */
            drain_pending_audio(vp, audio_scratch);
        }
        /* Audio packet — always processed, never blocked by frame queue */
        else if (vp->audio_codec_ctx &&
                 pkt->stream_index == vp->audio_tracks[vp->current_audio_track].stream_index) {
            ret = avcodec_send_packet(vp->audio_codec_ctx, pkt);
            if (ret >= 0) {
                while (avcodec_receive_frame(vp->audio_codec_ctx, vp->audio_frame) >= 0) {
                    int queue_ret = videoplayer_audio_queue_frame(vp, vp->audio_frame);
                    if (queue_ret == 1 && vp->decode_running) {
                        usleep(1000);
                        videoplayer_audio_queue_frame(vp, vp->audio_frame);
                    }
                    av_frame_unref(vp->audio_frame);
                }
            }
        }
        /* Subtitle packet */
        else if (vp->current_subtitle_track >= 0 &&
                 vp->subtitle_codec_ctx &&
                 pkt->stream_index == vp->subtitle_tracks[vp->current_subtitle_track].stream_index) {
            videoplayer_process_subtitle_packet(vp, pkt);
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    av_packet_free(&audio_scratch);
    fprintf(stderr, "[videoplayer] Decode thread stopped\n");

    return NULL;
}

/* ================================================================
 *  Seeking
 * ================================================================ */

void videoplayer_seek(VideoPlayer *vp, int64_t position_us)
{
    if (!vp || vp->state == VP_STATE_IDLE || vp->state == VP_STATE_LOADING ||
        vp->state == VP_STATE_ERROR)
        return;

    /* Clamp to valid range */
    if (position_us < 0)
        position_us = 0;
    if (vp->duration_us > 0 && position_us > vp->duration_us)
        position_us = vp->duration_us;

    /* Transition to BUFFERING immediately.  This prevents the present_frame
     * REBUFFER path from triggering when the frame queue empties during seek
     * processing.  The REBUFFER path pauses and flushes audio from the
     * compositor thread, which races with the decode thread's swr_convert()
     * and audio_flush().  By entering BUFFERING here, present_frame simply
     * waits for the buffer to refill after the seek completes.
     * The decode thread handles audio pause/flush safely in its seek handler. */
    vp->state = VP_STATE_BUFFERING;
    vp->last_frame_ns = 0;
    vp->last_present_time_ns = 0;
    vp->current_repeat = 0;
    vp->cadence_accum = 0.0f;

    vp->seek_target_us = position_us;
    vp->seek_flush_done = 0;
    vp->seek_requested = 1;

    /* Show control bar with seek position (time display uses seek_target_us) */
    videoplayer_show_control_bar(vp);

    if (vp->debug_log) {
        fprintf(vp->debug_log, "# SEEK | target=%.2fs (from pos=%.2fs)\n",
                position_us / 1000000.0, vp->position_us / 1000000.0);
        fflush(vp->debug_log);
    }

    /* Wake up both threads */
    pthread_cond_signal(&vp->frame_cond);
    pthread_cond_broadcast(&vp->pkt_not_empty);
    pthread_cond_broadcast(&vp->pkt_not_full);
}

void videoplayer_seek_relative(VideoPlayer *vp, int64_t offset_us)
{
    if (!vp)
        return;

    videoplayer_seek(vp, vp->position_us + offset_us);
}

/* ================================================================
 *  Track Selection
 * ================================================================ */

void videoplayer_set_audio_track(VideoPlayer *vp, int index)
{
    if (!vp || index < 0 || index >= vp->audio_track_count)
        return;

    if (index == vp->current_audio_track)
        return;

    /* Free old audio decoder */
    avcodec_free_context(&vp->audio_codec_ctx);
    av_frame_free(&vp->audio_frame);

    vp->current_audio_track = index;

    /* Initialize new audio decoder */
    init_audio_decoder(vp);
}

void videoplayer_set_subtitle_track(VideoPlayer *vp, int index)
{
    if (!vp)
        return;

    if (index < -1 || index >= vp->subtitle_track_count)
        return;

    if (index == vp->current_subtitle_track)
        return;

    /* Free old subtitle decoder */
    avcodec_free_context(&vp->subtitle_codec_ctx);

    vp->current_subtitle_track = index;

    /* Initialize new subtitle decoder */
    if (index >= 0) {
        init_subtitle_decoder(vp);
    }
}

void videoplayer_cycle_audio_track(VideoPlayer *vp)
{
    if (!vp || vp->audio_track_count == 0)
        return;

    int next = (vp->current_audio_track + 1) % vp->audio_track_count;
    videoplayer_set_audio_track(vp, next);
}

void videoplayer_cycle_subtitle_track(VideoPlayer *vp)
{
    if (!vp)
        return;

    int next = vp->current_subtitle_track + 1;
    if (next >= vp->subtitle_track_count)
        next = -1;  /* Off */

    videoplayer_set_subtitle_track(vp, next);
}

/* ================================================================
 *  Utility
 * ================================================================ */

const char *videoplayer_state_str(VideoPlayerState state)
{
    switch (state) {
    case VP_STATE_IDLE:      return "idle";
    case VP_STATE_LOADING:   return "loading";
    case VP_STATE_PLAYING:   return "playing";
    case VP_STATE_PAUSED:    return "paused";
    case VP_STATE_SEEKING:   return "seeking";
    case VP_STATE_BUFFERING: return "buffering";
    case VP_STATE_ERROR:     return "error";
    default:                 return "unknown";
    }
}

void videoplayer_format_time(char *buf, size_t len, int64_t us)
{
    if (us < 0)
        us = 0;

    int64_t secs = us / 1000000;
    int hours = secs / 3600;
    int mins = (secs % 3600) / 60;
    int s = secs % 60;

    if (hours > 0) {
        snprintf(buf, len, "%d:%02d:%02d", hours, mins, s);
    } else {
        snprintf(buf, len, "%d:%02d", mins, s);
    }
}
