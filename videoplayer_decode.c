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

#include <wlr/types/wlr_buffer.h>

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

/* External audio functions */
extern int videoplayer_audio_queue_frame(VideoPlayer *vp, AVFrame *frame);
extern int videoplayer_audio_init(VideoPlayer *vp);
extern void videoplayer_audio_play(VideoPlayer *vp);
extern void videoplayer_audio_flush(VideoPlayer *vp);

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

    /* Open file */
    ret = avformat_open_input(&vp->fmt_ctx, filepath, NULL, NULL);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        snprintf(vp->error_msg, sizeof(vp->error_msg),
                 "Failed to open file: %s", errbuf);
        vp->state = VP_STATE_ERROR;
        return -1;
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

    /* Start decode thread */
    vp->decode_running = 1;
    ret = pthread_create(&vp->decode_thread, NULL, decode_thread_func, vp);
    if (ret != 0) {
        snprintf(vp->error_msg, sizeof(vp->error_msg),
                 "Failed to create decode thread: %s", strerror(ret));
        vp->decode_running = 0;
        vp->state = VP_STATE_ERROR;
        return -1;
    }

    vp->state = VP_STATE_PAUSED;
    fprintf(stderr, "[videoplayer] Opened: %s (duration: %.2fs)\n",
            filepath, vp->duration_us / 1000000.0);

    return 0;
}

void videoplayer_close(VideoPlayer *vp)
{
    if (!vp)
        return;

    /* Stop decode thread */
    if (vp->decode_running) {
        vp->decode_running = 0;
        pthread_cond_broadcast(&vp->frame_cond);
        pthread_join(vp->decode_thread, NULL);
    }

    /* Clean up frame queue */
    pthread_mutex_destroy(&vp->frame_mutex);
    pthread_cond_destroy(&vp->frame_cond);

    /* Free frames */
    av_frame_free(&vp->decode_frame);
    av_frame_free(&vp->sw_frame);
    av_frame_free(&vp->audio_frame);

    /* Free sws context */
    if (vp->sws_ctx) {
        sws_freeContext(vp->sws_ctx);
        vp->sws_ctx = NULL;
    }

    /* Free codec contexts */
    avcodec_free_context(&vp->video_codec_ctx);
    avcodec_free_context(&vp->audio_codec_ctx);
    avcodec_free_context(&vp->subtitle_codec_ctx);

    /* Free HW contexts */
    av_buffer_unref(&vp->hw_device_ctx);
    av_buffer_unref(&vp->hw_frames_ctx);

    /* Close format context */
    avformat_close_input(&vp->fmt_ctx);

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
    memset(&vp->video, 0, sizeof(vp->video));
}

/* ================================================================
 *  Video Decoder Initialization
 * ================================================================ */

static enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx,
                                            const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI)
            return *p;
    }

    fprintf(stderr, "[videoplayer] VA-API format not available, falling back\n");
    return AV_PIX_FMT_NONE;
}

static int init_vaapi(VideoPlayer *vp)
{
    int ret;
    const char *device = VAAPI_DEVICE;

    /* Try primary device */
    ret = av_hwdevice_ctx_create(&vp->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                  device, NULL, 0);
    if (ret < 0) {
        /* Try alternate device */
        device = VAAPI_DEVICE_ALT;
        ret = av_hwdevice_ctx_create(&vp->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                      device, NULL, 0);
    }

    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[videoplayer] VA-API init failed: %s\n", errbuf);
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

static void *decode_thread_func(void *arg)
{
    VideoPlayer *vp = arg;
    AVPacket *pkt = av_packet_alloc();
    int ret;

    if (!pkt) {
        fprintf(stderr, "[videoplayer] Failed to allocate packet\n");
        return NULL;
    }

    fprintf(stderr, "[videoplayer] Decode thread started\n");

    while (vp->decode_running) {
        /* Handle seek request */
        if (vp->seek_requested) {
            int64_t seek_target = vp->seek_target_us;
            int64_t seek_ts = av_rescale_q(seek_target, AV_TIME_BASE_Q,
                                           vp->fmt_ctx->streams[vp->video.stream_index]->time_base);

            ret = av_seek_frame(vp->fmt_ctx, vp->video.stream_index, seek_ts,
                                AVSEEK_FLAG_BACKWARD);
            if (ret >= 0) {
                avcodec_flush_buffers(vp->video_codec_ctx);
                if (vp->audio_codec_ctx)
                    avcodec_flush_buffers(vp->audio_codec_ctx);

                /* Clear frame queue */
                pthread_mutex_lock(&vp->frame_mutex);
                vp->frames_queued = 0;
                vp->frame_read_idx = 0;
                vp->frame_write_idx = 0;
                pthread_mutex_unlock(&vp->frame_mutex);

                /* Flush audio buffer */
                videoplayer_audio_flush(vp);

                vp->position_us = seek_target;
            }
            vp->seek_requested = 0;
        }

        /* Check if frame queue is full */
        pthread_mutex_lock(&vp->frame_mutex);
        while (vp->frames_queued >= VP_FRAME_QUEUE_SIZE && vp->decode_running) {
            pthread_cond_wait(&vp->frame_cond, &vp->frame_mutex);
        }
        pthread_mutex_unlock(&vp->frame_mutex);

        if (!vp->decode_running)
            break;

        /* Read packet */
        ret = av_read_frame(vp->fmt_ctx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                /* End of file - loop or stop */
                fprintf(stderr, "[videoplayer] End of file\n");
                usleep(10000);
                continue;
            }
            /* Error */
            usleep(1000);
            continue;
        }

        /* Video packet */
        if (pkt->stream_index == vp->video.stream_index) {
            ret = avcodec_send_packet(vp->video_codec_ctx, pkt);
            if (ret < 0) {
                av_packet_unref(pkt);
                continue;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(vp->video_codec_ctx, vp->decode_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0)
                    break;

                AVFrame *frame_to_use = vp->decode_frame;

                /* Transfer HW frame to system memory */
                if (vp->hw_accel != VP_HW_NONE &&
                    vp->decode_frame->format == AV_PIX_FMT_VAAPI) {
                    if (transfer_hw_frame(vp, vp->decode_frame, vp->sw_frame) < 0) {
                        av_frame_unref(vp->decode_frame);
                        continue;
                    }
                    frame_to_use = vp->sw_frame;
                }

                /* Calculate PTS in microseconds */
                int64_t pts_us = 0;
                if (frame_to_use->pts != AV_NOPTS_VALUE) {
                    AVRational tb = vp->fmt_ctx->streams[vp->video.stream_index]->time_base;
                    pts_us = av_rescale_q(frame_to_use->pts, tb, AV_TIME_BASE_Q);
                }

                /* Create wlr_buffer from frame */
                struct wlr_buffer *buffer = videoplayer_create_frame_buffer(vp, frame_to_use);
                if (!buffer) {
                    av_frame_unref(vp->decode_frame);
                    av_frame_unref(vp->sw_frame);
                    continue;
                }

                /* Queue frame for rendering */
                pthread_mutex_lock(&vp->frame_mutex);
                if (vp->frames_queued < VP_FRAME_QUEUE_SIZE) {
                    VideoPlayerFrame *qf = &vp->frame_queue[vp->frame_write_idx];

                    /* Drop old buffer if present */
                    if (qf->buffer) {
                        wlr_buffer_drop(qf->buffer);
                    }

                    qf->buffer = buffer;
                    qf->pts_us = pts_us;
                    qf->width = frame_to_use->width;
                    qf->height = frame_to_use->height;
                    qf->ready = 1;

                    vp->frame_write_idx = (vp->frame_write_idx + 1) % VP_FRAME_QUEUE_SIZE;
                    vp->frames_queued++;
                    vp->position_us = pts_us;
                } else {
                    /* Queue full, drop buffer */
                    wlr_buffer_drop(buffer);
                }
                pthread_mutex_unlock(&vp->frame_mutex);

                av_frame_unref(vp->decode_frame);
                av_frame_unref(vp->sw_frame);
            }
        }
        /* Audio packet */
        else if (vp->audio_codec_ctx &&
                 pkt->stream_index == vp->audio_tracks[vp->current_audio_track].stream_index) {
            ret = avcodec_send_packet(vp->audio_codec_ctx, pkt);
            if (ret >= 0) {
                while (avcodec_receive_frame(vp->audio_codec_ctx, vp->audio_frame) >= 0) {
                    /* Queue audio frame to PipeWire ring buffer with retry */
                    int queue_ret;
                    int retry_count = 0;
                    do {
                        queue_ret = videoplayer_audio_queue_frame(vp, vp->audio_frame);
                        if (queue_ret == 1) {
                            /* Buffer full, wait and retry */
                            usleep(5000);
                            retry_count++;
                        }
                    } while (queue_ret == 1 && retry_count < 20 && vp->decode_running);
                    av_frame_unref(vp->audio_frame);
                }
            }
        }
        /* Subtitle packet */
        else if (vp->current_subtitle_track >= 0 &&
                 vp->subtitle_codec_ctx &&
                 pkt->stream_index == vp->subtitle_tracks[vp->current_subtitle_track].stream_index) {
            /* Process subtitle packet */
            videoplayer_process_subtitle_packet(vp, pkt);
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    fprintf(stderr, "[videoplayer] Decode thread stopped\n");

    return NULL;
}

/* ================================================================
 *  Seeking
 * ================================================================ */

void videoplayer_seek(VideoPlayer *vp, int64_t position_us)
{
    if (!vp || vp->state == VP_STATE_IDLE)
        return;

    /* Clamp to valid range */
    if (position_us < 0)
        position_us = 0;
    if (vp->duration_us > 0 && position_us > vp->duration_us)
        position_us = vp->duration_us;

    vp->seek_target_us = position_us;
    vp->seek_requested = 1;

    /* Wake up decode thread */
    pthread_cond_signal(&vp->frame_cond);
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
