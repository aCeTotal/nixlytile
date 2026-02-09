/*
 * Nixlytile Video Player - Rendering Module
 * Frame conversion and presentation to wlr_scene
 */

#define _GNU_SOURCE

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <pixman.h>
#include <drm_fourcc.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/interfaces/wlr_buffer.h>

#include "videoplayer.h"

/* ================================================================
 *  PixmanBuffer Implementation (matches nixlytile.c pattern)
 * ================================================================ */

struct VideoPixmanBuffer {
    struct wlr_buffer base;
    pixman_image_t *image;
    void *data;
    uint32_t drm_format;
    int stride;
    int owns_data;
};

static void video_pixman_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct VideoPixmanBuffer *buf = wl_container_of(wlr_buffer, buf, base);

    if (buf->image)
        pixman_image_unref(buf->image);
    if (buf->owns_data && buf->data)
        free(buf->data);
    free(buf);
}

static bool video_pixman_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
                                                       uint32_t flags,
                                                       void **data,
                                                       uint32_t *format,
                                                       size_t *stride)
{
    struct VideoPixmanBuffer *buf = wl_container_of(wlr_buffer, buf, base);

    if (data)
        *data = buf->data;
    if (format)
        *format = buf->drm_format;
    if (stride)
        *stride = buf->stride;

    return true;
}

static void video_pixman_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
    /* Nothing to do */
}

static const struct wlr_buffer_impl video_pixman_buffer_impl = {
    .destroy = video_pixman_buffer_destroy,
    .begin_data_ptr_access = video_pixman_buffer_begin_data_ptr_access,
    .end_data_ptr_access = video_pixman_buffer_end_data_ptr_access,
};

/* ================================================================
 *  Frame Conversion
 * ================================================================ */

struct wlr_buffer *videoplayer_create_frame_buffer(VideoPlayer *vp, AVFrame *frame)
{
    struct VideoPixmanBuffer *buf;
    int width = frame->width;
    int height = frame->height;
    int stride = width * 4;
    void *data;

    /* Allocate buffer */
    buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    data = calloc(height, stride);
    if (!data) {
        free(buf);
        return NULL;
    }

    /* Initialize or update sws context */
    enum AVPixelFormat src_fmt = frame->format;
    if (src_fmt == AV_PIX_FMT_NONE)
        src_fmt = AV_PIX_FMT_YUV420P;

    vp->sws_ctx = sws_getCachedContext(vp->sws_ctx,
                                        width, height, src_fmt,
                                        width, height, AV_PIX_FMT_BGRA,
                                        SWS_BILINEAR, NULL, NULL, NULL);
    if (!vp->sws_ctx) {
        free(data);
        free(buf);
        return NULL;
    }

    /* Convert to BGRA */
    uint8_t *dst_data[4] = { data, NULL, NULL, NULL };
    int dst_linesize[4] = { stride, 0, 0, 0 };

    sws_scale(vp->sws_ctx,
              (const uint8_t *const *)frame->data, frame->linesize,
              0, height,
              dst_data, dst_linesize);

    /* Create pixman image */
    buf->image = pixman_image_create_bits(PIXMAN_a8r8g8b8,
                                           width, height,
                                           data, stride);
    if (!buf->image) {
        free(data);
        free(buf);
        return NULL;
    }

    buf->data = data;
    buf->drm_format = DRM_FORMAT_ARGB8888;
    buf->stride = stride;
    buf->owns_data = 1;

    wlr_buffer_init(&buf->base, &video_pixman_buffer_impl, width, height);

    return &buf->base;
}

/* ================================================================
 *  Scaled Buffer Creation (for thumbnails/preview)
 * ================================================================ */

struct wlr_buffer *videoplayer_create_scaled_buffer(AVFrame *frame,
                                                     int target_width,
                                                     int target_height)
{
    struct VideoPixmanBuffer *buf;
    struct SwsContext *sws;
    int stride = target_width * 4;
    void *data;

    buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    data = calloc(target_height, stride);
    if (!data) {
        free(buf);
        return NULL;
    }

    enum AVPixelFormat src_fmt = frame->format;
    if (src_fmt == AV_PIX_FMT_NONE)
        src_fmt = AV_PIX_FMT_YUV420P;

    sws = sws_getContext(frame->width, frame->height, src_fmt,
                          target_width, target_height, AV_PIX_FMT_BGRA,
                          SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        free(data);
        free(buf);
        return NULL;
    }

    uint8_t *dst_data[4] = { data, NULL, NULL, NULL };
    int dst_linesize[4] = { stride, 0, 0, 0 };

    sws_scale(sws,
              (const uint8_t *const *)frame->data, frame->linesize,
              0, frame->height,
              dst_data, dst_linesize);

    sws_freeContext(sws);

    buf->image = pixman_image_create_bits(PIXMAN_a8r8g8b8,
                                           target_width, target_height,
                                           data, stride);
    if (!buf->image) {
        free(data);
        free(buf);
        return NULL;
    }

    buf->data = data;
    buf->drm_format = DRM_FORMAT_ARGB8888;
    buf->stride = stride;
    buf->owns_data = 1;

    wlr_buffer_init(&buf->base, &video_pixman_buffer_impl, target_width, target_height);

    return &buf->base;
}

/* ================================================================
 *  Frame Pacing with A/V Sync
 * ================================================================ */

/* External audio clock function */
extern int64_t videoplayer_audio_get_clock(VideoPlayer *vp);

/* Audio recovery function - check if stream needs reconnection */
extern int videoplayer_audio_check_recovery(VideoPlayer *vp);

/* Playback control */
extern void videoplayer_pause(VideoPlayer *vp);

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* A/V sync thresholds in microseconds */
#define AV_SYNC_THRESHOLD_MIN   20000    /* 20ms - below this, don't adjust */
#define AV_SYNC_THRESHOLD_MAX   200000   /* 200ms - above this, hard sync */
#define AV_SYNC_FRAMESKIP_THRESH 100000  /* 100ms - skip frame if behind by this much */
#define AV_SYNC_HOLD_THRESH      60000  /* 60ms - hold frame if video is ahead of audio */

/*
 * Calculate frame repeat pattern for non-VRR displays
 * Returns: 0 = VRR mode, 1+ = fixed repeat count, negative = 3:2 pulldown
 */
static void calculate_frame_pacing(VideoPlayer *vp, float display_hz)
{
    float video_fps = vp->video_fps;

    if (video_fps <= 0)
        video_fps = 24.0;

    /* Calculate frame interval in nanoseconds */
    vp->frame_interval_ns = (uint64_t)(1000000000.0 / video_fps);

    /* Store display refresh interval */
    vp->display_interval_ns = (uint64_t)(1000000000.0 / display_hz);

    float ratio = display_hz / video_fps;

    if (video_fps >= 23.9 && video_fps <= 24.1 &&
        display_hz >= 59.9 && display_hz <= 60.1) {
        /* 24fps on 60Hz - use 3:2 pulldown */
        vp->frame_repeat_mode = 2;
        vp->frame_repeat_count = 0;
    } else if (fabsf(ratio - roundf(ratio)) < 0.002f) {
        /* Exact integer multiple - e.g., 24.000@120Hz = 5x, 30.000@60Hz = 2x
         * Tight threshold (0.002) avoids misclassifying 23.976fps as clean. */
        vp->frame_repeat_mode = 1;
        vp->frame_repeat_count = (int)roundf(ratio);
    } else {
        /* Non-clean ratio (23.976@120Hz, 25@60Hz, etc.) - use frame interval
         * directly. Cadence-locked timing naturally produces the correct
         * variable-vsync pattern (e.g., 5-5-5-5-6 for 23.976@120Hz). */
        vp->frame_repeat_mode = 0;
        vp->frame_repeat_count = 0;
    }

    fprintf(stderr, "[videoplayer] Frame pacing: %.3f fps on %.2f Hz, mode=%d, count=%d, display_interval=%lu ns\n",
            video_fps, display_hz, vp->frame_repeat_mode, vp->frame_repeat_count,
            (unsigned long)vp->display_interval_ns);
}

/*
 * Check if audio clock is valid and progressing
 * Returns 1 if we should use A/V sync, 0 to fall back to timing-based
 *
 * NOTE: Audio sync is now OPTIONAL - video always plays smoothly based on timing.
 * Audio sync only provides gentle adjustments when audio is healthy.
 */
static int is_audio_clock_valid(VideoPlayer *vp)
{
    /* Check if audio stream is currently interrupted */
    if (vp->audio.stream_interrupted) {
        return 0;
    }

    /* During recovery after pause/resume or device change, wait for
     * audio clock to actually start progressing before enabling A/V sync.
     * This prevents stuttering when audio takes time to reactivate. */
    if (vp->audio.recovery_frames > 0) {
        int64_t rec_pts = videoplayer_audio_get_clock(vp);

        if (rec_pts > 0 && vp->audio.last_audio_pts > 0 &&
            rec_pts > vp->audio.last_audio_pts) {
            /* Audio is alive and progressing - end recovery early */
            vp->audio.recovery_frames = 0;
            vp->audio.stall_count = 0;
            vp->audio.last_audio_pts = rec_pts;
            return 1;
        }

        if (rec_pts > 0)
            vp->audio.last_audio_pts = rec_pts;

        vp->audio.recovery_frames--;
        return 0;
    }

    int64_t audio_pts_us = videoplayer_audio_get_clock(vp);

    /* Audio not started yet */
    if (audio_pts_us <= 0) {
        return 0;
    }

    /* Check if audio is progressing */
    if (audio_pts_us == vp->audio.last_audio_pts) {
        vp->audio.stall_count++;
        /* If stalled for more than 10 frames, audio may be stuck */
        if (vp->audio.stall_count > 10) {
            return 0;
        }
    } else {
        vp->audio.stall_count = 0;
        vp->audio.last_audio_pts = audio_pts_us;
    }

    return 1;
}

/*
 * Get the A/V sync difference in microseconds
 * Positive = video ahead of audio, Negative = video behind audio
 */
static int64_t get_av_diff_us(VideoPlayer *vp, int64_t video_pts_us)
{
    int64_t audio_pts_us = videoplayer_audio_get_clock(vp);

    /* If no audio clock yet, use position */
    if (audio_pts_us <= 0)
        audio_pts_us = vp->position_us;

    return video_pts_us - audio_pts_us;
}

/*
 * Determine frame presentation action based on A/V sync
 * Returns: 0 = show frame, 1 = skip frame (video behind), -1 = repeat current (video ahead)
 *
 * IMPORTANT: This is now very conservative - video playback is NEVER blocked.
 * We only skip frames if we're VERY far behind, and never repeat frames aggressively.
 */
static int get_frame_action(VideoPlayer *vp, int64_t video_pts_us)
{
    /* If audio clock is invalid/stalled, always show frame */
    if (!is_audio_clock_valid(vp)) {
        vp->av_sync_offset_us = 0;
        return 0;
    }

    int64_t av_diff = get_av_diff_us(vp, video_pts_us);

    /* Store for debugging/display */
    vp->av_sync_offset_us = av_diff;

    /* Only skip if we're VERY far behind (>100ms) */
    if (av_diff < -AV_SYNC_FRAMESKIP_THRESH) {
        return 1;  /* Skip frame to catch up */
    }

    /* Video is too far ahead of audio - hold frame to let audio catch up */
    if (av_diff > AV_SYNC_HOLD_THRESH) {
        return -1;
    }

    return 0;
}

/*
 * Check if we should present a frame at this vsync
 *
 * SIMPLIFIED: Always use timing-based presentation for rock-solid playback.
 * Audio sync only affects whether we skip frames when very far behind.
 */
static int should_present_frame_synced(VideoPlayer *vp, uint64_t vsync_time_ns, int64_t next_frame_pts_us)
{
    if (vp->last_frame_ns == 0) {
        /* First frame - always present */
        vp->last_frame_ns = vsync_time_ns;
        vp->current_repeat = 0;
        return 1;
    }

    uint64_t elapsed_ns = vsync_time_ns - vp->last_frame_ns;

    /* Safety: if frame_interval_ns is invalid, use 24fps default */
    uint64_t frame_interval = vp->frame_interval_ns;
    if (frame_interval == 0 || frame_interval > 1000000000) {
        frame_interval = 41666666;  /* ~24fps */
    }

    /*
     * Cadence-locked frame timing: advance last_frame_ns by the ideal step
     * instead of anchoring to vsync_time_ns. This prevents drift accumulation
     * and produces correct patterns for all refresh rate / fps combinations.
     *
     * After advancing, if the ideal time is too far behind wall clock
     * (pause/resume/seek), snap to present to avoid rapid catch-up.
     */

    uint64_t step;
    uint64_t display_interval_ns = vp->display_interval_ns > 0
        ? vp->display_interval_ns : frame_interval / 5;
    int time_to_present = 0;

    if (vp->frame_repeat_mode == 2) {
        /* 3:2 pulldown for 24fps on 60Hz */
        int target_repeats = (vp->current_repeat % 2 == 0) ? 2 : 3;
        step = vp->display_interval_ns * target_repeats;

        if (elapsed_ns >= step - vp->display_interval_ns / 4) {
            time_to_present = 1;
        }
    } else if (vp->frame_repeat_mode == 1 && vp->frame_repeat_count > 0 &&
               vp->display_interval_ns > 0) {
        /* Fixed repeat mode for exact integer ratios (24.000@120Hz, 30.000@60Hz).
         * Uses display_interval-aligned step for perfect vsync cadence. */
        step = (uint64_t)vp->frame_repeat_count * vp->display_interval_ns;

        if (elapsed_ns >= step - vp->display_interval_ns / 3) {
            time_to_present = 1;
        }
    } else {
        /* Frame-interval mode (VRR, and non-clean ratios like 23.976@120Hz).
         * Uses video's exact frame interval as step. Naturally produces the
         * correct variable-vsync pattern (e.g., mostly 5 vsyncs with occasional
         * 6 for 23.976@120Hz). half-display-interval tolerance finds nearest vsync. */
        step = frame_interval;
        uint64_t tolerance = display_interval_ns / 2;

        if (elapsed_ns + tolerance >= step) {
            time_to_present = 1;
        }
    }

    if (!time_to_present) {
        return 0;
    }

    /* Timing says it's time for a new frame.
     * Check A/V sync for frame skipping (only at video framerate, not every vsync,
     * so recovery_frames countdown is properly paced). */
    int action = get_frame_action(vp, next_frame_pts_us);
    if (action == 1) {
        /* Video is far behind audio - skip this frame */
        vp->last_frame_ns = vsync_time_ns;
        return 2;  /* Skip and get next frame immediately */
    }

    if (action == -1) {
        /* Video is ahead of audio - hold, let audio catch up */
        return 0;
    }

    /* Present the frame */
    if (vp->frame_repeat_mode == 2) {
        vp->current_repeat++;
    }
    vp->last_frame_ns += step;
    /* If we've fallen more than one step behind wall clock (e.g., after brief
     * queue exhaustion or a compositor hiccup), snap forward to avoid rapid
     * catch-up where multiple frames would be presented at consecutive vsyncs.
     * One step of slack allows the normal cadence to absorb minor jitter. */
    if (vsync_time_ns > vp->last_frame_ns + step)
        vp->last_frame_ns = vsync_time_ns;
    return 1;
}

/* ================================================================
 *  Frame Presentation with A/V Sync
 * ================================================================ */

void videoplayer_present_frame(VideoPlayer *vp, uint64_t vsync_time_ns)
{
    static int recovery_check_counter = 0;

    if (!vp || vp->state != VP_STATE_PLAYING)
        return;

    /* Check if audio thread requested a timing reset (e.g., after BT reconnection).
     * This ensures video frame presentation resumes immediately even if the old
     * timing state would have delayed presentation. */
    if (vp->audio.needs_timing_reset) {
        vp->audio.needs_timing_reset = 0;
        vp->last_frame_ns = 0;
        vp->current_repeat = 0;
        fprintf(stderr, "[videoplayer] Frame timing reset (audio state change)\n");
    }

    /* Auto-pause when audio device changes (e.g., Bluetooth connection) */
    if (vp->audio.needs_device_pause) {
        vp->audio.needs_device_pause = 0;
        fprintf(stderr, "[videoplayer] Audio device change detected, pausing\n");
        videoplayer_pause(vp);
        return;
    }

    /* Periodically check if audio stream needs recovery (every ~0.5 second at 60Hz).
     * This handles Bluetooth controller reconnection etc. without blocking. */
    if (++recovery_check_counter >= 30) {
        recovery_check_counter = 0;
        videoplayer_audio_check_recovery(vp);
    }

    /* Video stall detector: if frames are available but we haven't presented
     * any frame for >500ms, force-reset timing. This catches edge cases where
     * the frame pacing logic gets stuck (e.g., after audio device changes). */
    if (vp->last_present_time_ns > 0 &&
        vsync_time_ns > vp->last_present_time_ns + 500000000ULL) {
        pthread_mutex_lock(&vp->frame_mutex);
        int has_frames = vp->frames_queued > 0;
        pthread_mutex_unlock(&vp->frame_mutex);

        if (has_frames) {
            fprintf(stderr, "[videoplayer] Video stall detected (>500ms), forcing timing reset\n");
            vp->last_frame_ns = 0;
            vp->current_repeat = 0;
        }
    }

    struct wlr_buffer *buffer = NULL;
    int frames_consumed = 0;
    int max_skip = 3;  /* Maximum frames to skip per vsync to prevent stalls */

present_next:
    pthread_mutex_lock(&vp->frame_mutex);

    if (vp->frames_queued == 0) {
        pthread_mutex_unlock(&vp->frame_mutex);
        goto present_buffer;
    }

    VideoPlayerFrame *qf = &vp->frame_queue[vp->frame_read_idx];

    if (!qf->ready) {
        pthread_mutex_unlock(&vp->frame_mutex);
        goto present_buffer;
    }

    /* Check if we should present this frame based on A/V sync */
    int action = should_present_frame_synced(vp, vsync_time_ns, qf->pts_us);

    if (action == 0) {
        /* Not time yet - keep current frame */
        pthread_mutex_unlock(&vp->frame_mutex);
        return;
    }

    if (action == 2 && frames_consumed < max_skip) {
        /* Skip this frame - video is behind audio */
        /* Drop the buffer and advance to next frame */
        if (qf->buffer) {
            wlr_buffer_drop(qf->buffer);
            qf->buffer = NULL;
        }

        qf->ready = 0;
        vp->frame_read_idx = (vp->frame_read_idx + 1) % VP_FRAME_QUEUE_SIZE;
        vp->frames_queued--;
        frames_consumed++;

        pthread_cond_signal(&vp->frame_cond);
        pthread_mutex_unlock(&vp->frame_mutex);

        /* Try to get the next frame */
        goto present_next;
    }

    /* action == 1: Present this frame normally */

    /* Update position */
    vp->position_us = qf->pts_us;

    /* Get the buffer to display (drop previous if any) */
    if (buffer) {
        wlr_buffer_drop(buffer);
    }
    buffer = qf->buffer;
    qf->buffer = NULL;  /* Take ownership */

    /* Mark frame as consumed */
    qf->ready = 0;
    vp->frame_read_idx = (vp->frame_read_idx + 1) % VP_FRAME_QUEUE_SIZE;
    vp->frames_queued--;
    frames_consumed++;

    /* Signal decode thread */
    pthread_cond_signal(&vp->frame_cond);

    pthread_mutex_unlock(&vp->frame_mutex);

present_buffer:
    /* Update scene buffer with new frame */
    if (buffer) {
        videoplayer_update_frame_buffer(vp, buffer);
        wlr_buffer_drop(buffer);  /* Scene holds its own reference */
        vp->last_present_time_ns = vsync_time_ns;
    }
}

/* ================================================================
 *  Scene Integration
 * ================================================================ */

int videoplayer_init_scene(VideoPlayer *vp, struct wlr_scene_tree *parent)
{
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    if (!vp || !parent)
        return -1;

    /* Create main tree */
    vp->tree = wlr_scene_tree_create(parent);
    if (!vp->tree)
        return -1;

    /* Create black background (for letterboxing) */
    vp->bg_rect = wlr_scene_rect_create(vp->tree, 0, 0, black);
    if (!vp->bg_rect) {
        wlr_scene_node_destroy(&vp->tree->node);
        vp->tree = NULL;
        return -1;
    }

    /* Create video layer */
    vp->video_tree = wlr_scene_tree_create(vp->tree);
    if (!vp->video_tree) {
        wlr_scene_node_destroy(&vp->tree->node);
        vp->tree = NULL;
        return -1;
    }

    /* Create subtitle overlay */
    vp->subtitle_tree = wlr_scene_tree_create(vp->tree);
    if (!vp->subtitle_tree) {
        wlr_scene_node_destroy(&vp->tree->node);
        vp->tree = NULL;
        return -1;
    }

    /* Position subtitle tree above video */
    wlr_scene_node_raise_to_top(&vp->subtitle_tree->node);

    /* Create initial frame buffer node (empty) */
    vp->frame_node = wlr_scene_buffer_create(vp->video_tree, NULL);

    /* Start with scene hidden */
    wlr_scene_node_set_enabled(&vp->tree->node, false);

    return 0;
}

void videoplayer_update_frame_buffer(VideoPlayer *vp, struct wlr_buffer *buffer)
{
    if (!vp || !vp->frame_node)
        return;

    wlr_scene_buffer_set_buffer(vp->frame_node, buffer);
}

void videoplayer_set_position(VideoPlayer *vp, int x, int y)
{
    if (!vp || !vp->tree)
        return;

    wlr_scene_node_set_position(&vp->tree->node, x, y);
}

void videoplayer_set_visible(VideoPlayer *vp, int visible)
{
    if (!vp || !vp->tree)
        return;

    wlr_scene_node_set_enabled(&vp->tree->node, visible);
    if (visible)
        wlr_scene_node_raise_to_top(&vp->tree->node);
}

void videoplayer_set_fullscreen_size(VideoPlayer *vp, int width, int height)
{
    if (!vp)
        return;

    /* Resize black background to fill screen */
    if (vp->bg_rect) {
        wlr_scene_rect_set_size(vp->bg_rect, width, height);
    }

    /* Scale video to fill the screen */
    if (vp->frame_node) {
        wlr_scene_buffer_set_dest_size(vp->frame_node, width, height);
    }

    /* Store fullscreen dimensions for control bar positioning */
    vp->fullscreen_width = width;
    vp->fullscreen_height = height;
}

/* ================================================================
 *  Display Mode Integration
 * ================================================================ */

void videoplayer_setup_display_mode(VideoPlayer *vp, float display_hz, int vrr_capable)
{
    if (!vp)
        return;

    /* Store display parameters */
    vp->display_hz = display_hz;
    vp->display_interval_ns = (uint64_t)(1000000000.0 / display_hz);

    if (vrr_capable) {
        /* VRR mode - present at video framerate, synced to audio */
        float fps = vp->video_fps > 0 ? vp->video_fps : 24.0f;
        vp->frame_repeat_mode = 0;
        vp->frame_repeat_count = 0;
        vp->frame_interval_ns = (uint64_t)(1000000000.0 / fps);
        fprintf(stderr, "[videoplayer] VRR mode enabled, target %.3f fps, display %.2f Hz\n",
                fps, display_hz);
    } else {
        /* Fixed refresh - calculate optimal frame repeat pattern */
        calculate_frame_pacing(vp, display_hz);
    }

    /* Reset timing state for clean start */
    vp->last_frame_ns = 0;
    vp->last_present_time_ns = 0;
    vp->current_repeat = 0;
    vp->av_sync_offset_us = 0;
}

/* ================================================================
 *  Cleanup
 * ================================================================ */

void videoplayer_cleanup_scene(VideoPlayer *vp)
{
    if (!vp)
        return;

    if (vp->tree) {
        wlr_scene_node_destroy(&vp->tree->node);
        vp->tree = NULL;
        vp->video_tree = NULL;
        vp->subtitle_tree = NULL;
        vp->frame_node = NULL;
    }
}
