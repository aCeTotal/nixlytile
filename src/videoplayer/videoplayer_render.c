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
    VideoPlayer *vp;               /* Back-reference for buffer pool recycling */
};

static void video_pixman_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct VideoPixmanBuffer *buf = wl_container_of(wlr_buffer, buf, base);

    if (buf->image)
        pixman_image_unref(buf->image);

    if (buf->owns_data && buf->data) {
        /* Try to return pixel data to the pool for reuse instead of freeing.
         * This avoids mmap/munmap system calls (8-33MB per frame) that cause
         * page faults, TLB flushes, and visible stutter. */
        int returned = 0;
        if (buf->vp) {
            size_t buf_size = (size_t)buf->base.height * buf->stride;
            pthread_mutex_lock(&buf->vp->buffer_pool.lock);
            if (buf->vp->buffer_pool.count < VP_BUFFER_POOL_SIZE &&
                buf->vp->buffer_pool.alloc_size == buf_size) {
                buf->vp->buffer_pool.data[buf->vp->buffer_pool.count++] = buf->data;
                returned = 1;
            }
            pthread_mutex_unlock(&buf->vp->buffer_pool.lock);
        }
        if (!returned)
            free(buf->data);
    }

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
 *  Color Management
 * ================================================================ */

/* Chroma 4:4:4 quality upsampling + accurate rounding */
#define SWS_QUALITY_FLAGS \
    (SWS_LANCZOS | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP | SWS_ACCURATE_RND)

/* Map FFmpeg color space to swscale coefficient table index */
static int get_sws_colorspace(enum AVColorSpace cs, int height)
{
    switch (cs) {
    case AVCOL_SPC_BT709:       return SWS_CS_ITU709;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:  return SWS_CS_BT2020;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:    return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M:  return SWS_CS_SMPTE240M;
    case AVCOL_SPC_FCC:        return SWS_CS_FCC;
    default:
        /* Heuristic: HD content (>=720p) is almost always BT.709 */
        return (height >= 720) ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

/* Configure correct colorspace handling on swscale context.
 * Sets the YUV→RGB matrix coefficients and input/output range. */
static void configure_sws_colorspace(struct SwsContext *sws, VideoPlayer *vp)
{
    int src_cs = get_sws_colorspace(vp->video.color_space, vp->video.height);
    int src_range = (vp->video.color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    int dst_range = 1;  /* Full range for RGB display output */

    const int *src_table = sws_getCoefficients(src_cs);
    const int *dst_table = sws_getCoefficients(SWS_CS_ITU709);

    sws_setColorspaceDetails(sws,
        src_table, src_range,
        dst_table, dst_range,
        0, 1 << 16, 1 << 16);  /* brightness=0, contrast=1.0, saturation=1.0 */
}

/* ================================================================
 *  HDR Tonemapping LUT
 * ================================================================ */

/* PQ (SMPTE ST 2084) constants */
#define PQ_M1  0.1593017578125       /* 2610/16384 */
#define PQ_M2  78.84375              /* 2523/32 * 128 */
#define PQ_C1  0.8359375             /* 3424/4096 */
#define PQ_C2  18.8515625            /* 2413/128 */
#define PQ_C3  18.6875               /* 2392/128 */

/* Hable (Uncharted 2) tonemap operator */
static inline float hable_partial(float x)
{
    float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

/*
 * Build HDR tonemapping lookup table for PQ (HDR10) content.
 * Maps 16-bit PQ signal → 16-bit sRGB value.
 * 65536 entries × 2 bytes = 128 KB (fits in L2 cache).
 * Called once per file when HDR content is detected.
 */
static void build_pq_lut(VideoPlayer *vp)
{
    float peak_nits = 1000.0f;

    /* Get peak luminance from content metadata */
    if (vp->video.has_content_light && vp->video.content_light.MaxCLL > 0)
        peak_nits = (float)vp->video.content_light.MaxCLL;
    else if (vp->video.has_mastering_display) {
        AVRational ml = vp->video.mastering.max_luminance;
        if (ml.num > 0 && ml.den > 0) {
            float lum = (float)ml.num / ml.den;
            if (lum > 100.0f)
                peak_nits = lum;
        }
    }

    if (peak_nits < 400.0f) peak_nits = 1000.0f;
    if (peak_nits > 10000.0f) peak_nits = 10000.0f;

    /* SDR reference white per BT.2408 */
    float sdr_white = 203.0f;
    float Lw = peak_nits / sdr_white;
    float hw = hable_partial(Lw);

    vp->hdr_lut = malloc(65536 * sizeof(uint16_t));
    if (!vp->hdr_lut) return;

    for (int i = 0; i < 65536; i++) {
        float N = i / 65535.0f;

        /* PQ EOTF: signal → linear light (cd/m²) */
        float Np = powf(N, 1.0f / PQ_M2);
        float num = Np - PQ_C1;
        if (num < 0.0f) num = 0.0f;
        float den = PQ_C2 - PQ_C3 * Np;
        if (den <= 0.0f) den = 1e-10f;
        float linear_nits = powf(num / den, 1.0f / PQ_M1) * 10000.0f;

        /* Normalize to SDR reference white */
        float L = linear_nits / sdr_white;

        /* Hable tonemapping */
        float mapped = hable_partial(L) / hw;
        if (mapped > 1.0f) mapped = 1.0f;
        if (mapped < 0.0f) mapped = 0.0f;

        /* sRGB OETF (gamma) */
        float srgb;
        if (mapped <= 0.0031308f)
            srgb = 12.92f * mapped;
        else
            srgb = 1.055f * powf(mapped, 1.0f / 2.4f) - 0.055f;

        /* Store as 16-bit for both 8-bit (>>8) and 10-bit (>>6) output */
        int val = (int)(srgb * 65535.0f + 0.5f);
        vp->hdr_lut[i] = (uint16_t)(val < 0 ? 0 : (val > 65535 ? 65535 : val));
    }

    fprintf(stderr, "[videoplayer] Built PQ tonemap LUT (peak=%.0f nits, Hable)\n", peak_nits);
}

/*
 * Build HDR tonemapping lookup table for HLG content.
 */
static void build_hlg_lut(VideoPlayer *vp)
{
    vp->hdr_lut = malloc(65536 * sizeof(uint16_t));
    if (!vp->hdr_lut) return;

    float system_gamma = 1.2f;  /* BT.2100 OOTF for SDR display */

    for (int i = 0; i < 65536; i++) {
        float E = i / 65535.0f;

        /* HLG inverse OETF → scene light */
        float scene;
        if (E <= 0.5f) {
            scene = E * E / 3.0f;
        } else {
            float a = 0.17883277f;
            float b = 0.28466892f;
            float c = 0.55991073f;
            scene = (expf((E - c) / a) + b) / 12.0f;
        }

        /* Apply OOTF system gamma for SDR display */
        float display = powf(scene, system_gamma);
        if (display > 1.0f) display = 1.0f;
        if (display < 0.0f) display = 0.0f;

        /* sRGB OETF */
        float srgb;
        if (display <= 0.0031308f)
            srgb = 12.92f * display;
        else
            srgb = 1.055f * powf(display, 1.0f / 2.4f) - 0.055f;

        int val = (int)(srgb * 65535.0f + 0.5f);
        vp->hdr_lut[i] = (uint16_t)(val < 0 ? 0 : (val > 65535 ? 65535 : val));
    }

    fprintf(stderr, "[videoplayer] Built HLG tonemap LUT\n");
}

void videoplayer_build_hdr_lut(VideoPlayer *vp)
{
    if (!vp || !vp->video.is_hdr) return;

    /* Free any existing LUT from previous file */
    free(vp->hdr_lut);
    vp->hdr_lut = NULL;

    if (vp->video.color_trc == AVCOL_TRC_ARIB_STD_B67)
        build_hlg_lut(vp);
    else
        build_pq_lut(vp);
}

void videoplayer_cleanup_hdr_lut(VideoPlayer *vp)
{
    if (!vp) return;
    free(vp->hdr_lut);
    vp->hdr_lut = NULL;
    free(vp->hdr_tmp_buffer);
    vp->hdr_tmp_buffer = NULL;
    vp->hdr_tmp_size = 0;
}

/* ================================================================
 *  HDR Tonemapping + Packing
 * ================================================================ */

/*
 * Apply HDR tonemapping LUT and pack RGBA64 → output format.
 * The LUT stores 16-bit sRGB values; we shift to 8-bit or 10-bit.
 */
static void tonemap_and_pack(const uint8_t *src, int src_stride,
                              void *dst, int dst_stride,
                              int width, int height,
                              const uint16_t *lut,
                              int output_10bit)
{
    for (int y = 0; y < height; y++) {
        const uint16_t *s = (const uint16_t *)(src + y * src_stride);

        if (output_10bit) {
            uint32_t *d = (uint32_t *)((uint8_t *)dst + y * dst_stride);
            for (int x = 0; x < width; x++) {
                uint32_t r10 = lut[s[0]] >> 6;
                uint32_t g10 = lut[s[1]] >> 6;
                uint32_t b10 = lut[s[2]] >> 6;
                d[x] = (3u << 30) | (r10 << 20) | (g10 << 10) | b10;
                s += 4;
            }
        } else {
            uint8_t *d = (uint8_t *)dst + y * dst_stride;
            for (int x = 0; x < width; x++) {
                d[0] = lut[s[2]] >> 8;  /* B */
                d[1] = lut[s[1]] >> 8;  /* G */
                d[2] = lut[s[0]] >> 8;  /* R */
                d[3] = 0xFF;            /* A */
                s += 4;
                d += 4;
            }
        }
    }
}

/*
 * Pack RGBA64LE → ARGB2101010 (10-bit output, no tonemapping for SDR).
 */
static void pack_rgba64_to_10bit(const uint8_t *src, int src_stride,
                                  void *dst, int dst_stride,
                                  int width, int height)
{
    for (int y = 0; y < height; y++) {
        const uint16_t *s = (const uint16_t *)(src + y * src_stride);
        uint32_t *d = (uint32_t *)((uint8_t *)dst + y * dst_stride);

        for (int x = 0; x < width; x++) {
            uint32_t r10 = s[0] >> 6;
            uint32_t g10 = s[1] >> 6;
            uint32_t b10 = s[2] >> 6;
            d[x] = (3u << 30) | (r10 << 20) | (g10 << 10) | b10;
            s += 4;
        }
    }
}

/* ================================================================
 *  Frame Conversion
 * ================================================================ */

struct wlr_buffer *videoplayer_create_frame_buffer(VideoPlayer *vp, AVFrame *frame)
{
    struct VideoPixmanBuffer *buf;
    int width = frame->width;
    int height = frame->height;
    int is_hdr = vp->video.is_hdr && vp->hdr_lut;
    int output_10bit = vp->output_10bit;
    int need_rgba64 = is_hdr || output_10bit;

    /* Output is always 32-bit packed (both ARGB8888 and ARGB2101010 are 4 bytes) */
    int stride = width * 4;
    size_t alloc_size = (size_t)height * stride;
    void *data = NULL;

    pixman_format_code_t pix_fmt = output_10bit
        ? PIXMAN_a2r10g10b10
        : PIXMAN_a8r8g8b8;
    uint32_t drm_fmt = output_10bit
        ? DRM_FORMAT_ARGB2101010
        : DRM_FORMAT_ARGB8888;

    /* Allocate buffer struct */
    buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    /* Try to recycle a pixel data buffer from the pool.
     * This avoids mmap/munmap system calls that cause stutter. */
    pthread_mutex_lock(&vp->buffer_pool.lock);
    if (vp->buffer_pool.count > 0 && vp->buffer_pool.alloc_size == alloc_size) {
        data = vp->buffer_pool.data[--vp->buffer_pool.count];
    } else {
        vp->buffer_pool.alloc_size = alloc_size;
    }
    pthread_mutex_unlock(&vp->buffer_pool.lock);

    if (!data) {
        data = malloc(alloc_size);
        if (!data) {
            free(buf);
            return NULL;
        }
    }

    /* Source pixel format */
    enum AVPixelFormat src_fmt = frame->format;
    if (src_fmt == AV_PIX_FMT_NONE)
        src_fmt = AV_PIX_FMT_YUV420P;

    if (need_rgba64) {
        /* ── High-precision path: YUV → RGBA64LE → pack to output ── */
        int rgba64_stride = width * 8;
        size_t rgba64_size = (size_t)height * rgba64_stride;

        /* Allocate/reuse temporary RGBA64 buffer */
        if (!vp->hdr_tmp_buffer || vp->hdr_tmp_size < rgba64_size) {
            free(vp->hdr_tmp_buffer);
            vp->hdr_tmp_buffer = malloc(rgba64_size);
            vp->hdr_tmp_size = rgba64_size;
        }
        if (!vp->hdr_tmp_buffer) {
            free(data); free(buf); return NULL;
        }

        /* swscale to RGBA64LE with Lanczos chroma upsampling */
        vp->sws_ctx = sws_getCachedContext(vp->sws_ctx,
            width, height, src_fmt,
            width, height, AV_PIX_FMT_RGBA64LE,
            SWS_QUALITY_FLAGS, NULL, NULL, NULL);
        if (!vp->sws_ctx) {
            free(data); free(buf); return NULL;
        }

        configure_sws_colorspace(vp->sws_ctx, vp);

        uint8_t *tmp_data[4] = { vp->hdr_tmp_buffer, NULL, NULL, NULL };
        int tmp_stride[4] = { rgba64_stride, 0, 0, 0 };

        sws_scale(vp->sws_ctx,
                  (const uint8_t *const *)frame->data, frame->linesize,
                  0, height, tmp_data, tmp_stride);

        /* Apply tonemapping LUT and/or pack to output format */
        if (is_hdr) {
            tonemap_and_pack(vp->hdr_tmp_buffer, rgba64_stride,
                             data, stride, width, height,
                             vp->hdr_lut, output_10bit);
        } else {
            /* SDR on 10-bit display: just pack to 10-bit */
            pack_rgba64_to_10bit(vp->hdr_tmp_buffer, rgba64_stride,
                                  data, stride, width, height);
        }
    } else {
        /* ── Fast path: SDR → direct BGRA ── */
        vp->sws_ctx = sws_getCachedContext(vp->sws_ctx,
            width, height, src_fmt,
            width, height, AV_PIX_FMT_BGRA,
            SWS_BILINEAR, NULL, NULL, NULL);
        if (!vp->sws_ctx) {
            free(data); free(buf); return NULL;
        }

        configure_sws_colorspace(vp->sws_ctx, vp);

        uint8_t *dst_data[4] = { data, NULL, NULL, NULL };
        int dst_linesize[4] = { stride, 0, 0, 0 };

        sws_scale(vp->sws_ctx,
                  (const uint8_t *const *)frame->data, frame->linesize,
                  0, height, dst_data, dst_linesize);
    }

    /* Create pixman image */
    buf->image = pixman_image_create_bits(pix_fmt,
                                           width, height,
                                           data, stride);
    if (!buf->image) {
        free(data);
        free(buf);
        return NULL;
    }

    buf->data = data;
    buf->drm_format = drm_fmt;
    buf->stride = stride;
    buf->owns_data = 1;
    buf->vp = vp;

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
                          SWS_QUALITY_FLAGS, NULL, NULL, NULL);
    if (!sws) {
        free(data);
        free(buf);
        return NULL;
    }

    /* Apply correct colorspace for thumbnails (heuristic: >=720p = BT.709) */
    {
        int cs = (frame->height >= 720) ? SWS_CS_ITU709 : SWS_CS_ITU601;
        const int *src_table = sws_getCoefficients(cs);
        const int *dst_table = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(sws, src_table, 0, dst_table, 1,
                                  0, 1 << 16, 1 << 16);
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
extern void videoplayer_play(VideoPlayer *vp);
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
        /* Non-clean ratio on fixed-refresh display.
         * Bresenham cadence: distribute the extra vsyncs evenly.
         * E.g., 23.976@300Hz -> base=12, frac~=0.52 -> pattern 12-13-12-13... */
        int base = (int)floorf(ratio);
        vp->frame_repeat_mode = 3;
        vp->frame_repeat_count = base;
        vp->cadence_base = base;
        vp->cadence_frac = ratio - (float)base;
        vp->cadence_accum = 0.0f;
    }

    if (vp->frame_repeat_mode == 3) {
        fprintf(stderr, "[videoplayer] Frame pacing: %.3f fps on %.2f Hz, mode=3 (cadence), base=%d, frac=%.4f\n",
                video_fps, display_hz, vp->cadence_base, vp->cadence_frac);
    } else {
        fprintf(stderr, "[videoplayer] Frame pacing: %.3f fps on %.2f Hz, mode=%d, count=%d, display_interval=%lu ns\n",
                video_fps, display_hz, vp->frame_repeat_mode, vp->frame_repeat_count,
                (unsigned long)vp->display_interval_ns);
    }
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
        vp->av_sync_established = 0;
        return 0;
    }

    /* During recovery after pause/resume or device change, wait for
     * audio clock to actually start progressing before enabling A/V sync.
     * This prevents stuttering when audio takes time to reactivate. */
    if (vp->audio.recovery_frames > 0) {
        int64_t rec_pts = videoplayer_audio_get_clock(vp);

        if (rec_pts > 0 && vp->audio.last_audio_pts > 0 &&
            rec_pts > vp->audio.last_audio_pts) {
            /* Audio is alive and progressing - end recovery early.
             * Clear sync so get_av_diff_us recaptures bases with the
             * now-valid audio clock for accurate relative sync. */
            vp->audio.recovery_frames = 0;
            vp->audio.stall_count = 0;
            vp->audio.last_audio_pts = rec_pts;
            vp->av_sync_established = 0;
            return 1;
        }

        if (rec_pts > 0)
            vp->audio.last_audio_pts = rec_pts;

        vp->audio.recovery_frames--;
        vp->av_sync_established = 0;
        return 0;
    }

    int64_t audio_pts_us = videoplayer_audio_get_clock(vp);

    /* Audio not started yet */
    if (audio_pts_us <= 0) {
        vp->av_sync_established = 0;
        return 0;
    }

    /* Check if audio is progressing */
    if (audio_pts_us == vp->audio.last_audio_pts) {
        vp->audio.stall_count++;
        /* If stalled for more than 10 frames, audio may be stuck.
         * Fall back to timing-based playback but do NOT clear
         * av_sync_established — when audio resumes, the existing bases
         * are still valid and sync continues smoothly. Clearing bases
         * here caused periodic ~30s A/V sync jumps during HTTP streaming
         * stalls, as re-establishment captured different offsets. */
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
 * Get the A/V sync difference in microseconds using RELATIVE elapsed times.
 * Positive = video ahead of audio, Negative = video behind audio.
 *
 * Uses elapsed time from a common sync-establishment point rather than
 * absolute PTS values. This cancels any constant PTS offset between
 * audio and video streams (common in HTTP/HLS streams, remuxed files,
 * and transport streams where audio base_pts != video base_pts).
 *
 * Without relative sync, a stream where audio starts at PTS=5s and video
 * at PTS=0s shows a permanent -5s offset that triggers continuous frame
 * skipping, reducing playback to ~2fps.
 */
static int64_t get_av_diff_us(VideoPlayer *vp, int64_t video_pts_us)
{
    int64_t audio_pts_us = videoplayer_audio_get_clock(vp);

    if (audio_pts_us <= 0)
        return 0;  /* Audio not started, no sync possible */

    if (!vp->av_sync_established) {
        /* First valid comparison — snapshot both clocks as reference.
         * All future comparisons measure elapsed time from this point. */
        vp->av_sync_video_base_us = video_pts_us;
        vp->av_sync_audio_base_us = audio_pts_us;
        vp->av_sync_established = 1;
        return 0;  /* First frame after sync establishment — show it */
    }

    int64_t video_elapsed = video_pts_us - vp->av_sync_video_base_us;
    int64_t audio_elapsed = audio_pts_us - vp->av_sync_audio_base_us;
    int64_t diff = video_elapsed - audio_elapsed;

    /* PTS discontinuity detection: if diff exceeds 5 seconds, the stream
     * has a timestamp jump (common in HTTP/HLS streams after remuxing or
     * server-side seeking).  Re-establish sync bases from the current
     * positions instead of trying to gradually correct a multi-second gap,
     * which would cause sustained frame-skipping or drift artifacts. */
    if (diff > 5000000 || diff < -5000000) {
        vp->av_sync_video_base_us = video_pts_us;
        vp->av_sync_audio_base_us = audio_pts_us;
        return 0;
    }

    /* Gradual drift correction: shift the video base by 2% of the current
     * offset each video frame.  This acts as a first-order low-pass filter
     * that absorbs systematic clock drift between the video frame-interval
     * timer (CLOCK_MONOTONIC) and PipeWire's sample-rate clock.
     *
     * Without this, any drift accumulates indefinitely — typically reaching
     * +55 to +170 ms over 20-50 seconds — until PipeWire's quantum stepping
     * causes a sudden correction that the user sees as periodic stuttering.
     *
     * 2% per frame at 24 fps ≈ 48% per second, giving a ~2-second time
     * constant.  This is fast enough to track hardware clock drift and
     * PipeWire quantum jitter, but slow enough to preserve smooth playback. */
    int64_t correction = diff / 50;
    vp->av_sync_video_base_us += correction;

    return diff;
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

    /* Never hold frames — holding prevents frame queue consumption, which blocks
     * the decode thread, which starves the audio ring buffer, which stalls the
     * audio clock, creating a positive-feedback loop that manifests as periodic
     * multi-second stuttering. Small A/V drift self-corrects via frame-skip. */
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
        vp->cadence_accum = 0.0f;
        return 1;
    }

    /* Guard against unsigned underflow: for non-integer fps/refresh ratios
     * (e.g., 23.976@120Hz), last_frame_ns advances by the exact frame interval
     * which can drift slightly ahead of vsync_time_ns. Without this check,
     * uint64_t subtraction wraps to a huge value, causing a frame to present
     * early followed by a long gap — visible as micro-stutter every ~4 seconds. */
    if (vsync_time_ns < vp->last_frame_ns)
        return 0;

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
    } else if (vp->frame_repeat_mode == 3 && vp->display_interval_ns > 0) {
        /* Bresenham cadence for non-integer ratios on fixed-refresh.
         * Peek at what the repeat count would be if we present now. */
        int repeats = vp->cadence_base;
        float test_accum = vp->cadence_accum + vp->cadence_frac;
        if (test_accum >= 1.0f) {
            repeats++;
        }
        step = (uint64_t)repeats * vp->display_interval_ns;

        if (elapsed_ns >= step - vp->display_interval_ns / 3) {
            time_to_present = 1;
        }
    } else {
        /* Frame-interval mode (VRR).
         * Uses video's exact frame interval as step. half-display-interval
         * tolerance finds nearest vsync. */
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
        /* Video is far behind audio — skip this frame.
         * Advance last_frame_ns by one step so timing stays accurate.
         * Multiple skips per vsync are still possible if the original
         * elapsed time covers multiple steps (e.g., after a stall). */
        vp->last_frame_ns += step;
        return 2;  /* Skip and get next frame immediately */
    }

    /* Present the frame */
    if (vp->frame_repeat_mode == 2) {
        vp->current_repeat++;
    } else if (vp->frame_repeat_mode == 3) {
        vp->cadence_accum += vp->cadence_frac;
        if (vp->cadence_accum >= 1.0f) {
            vp->cadence_accum -= 1.0f;
        }
    }
    vp->last_frame_ns += step;
    /* If we've fallen more than one step behind wall clock (e.g., after brief
     * queue exhaustion or a compositor hiccup), snap forward to avoid rapid
     * catch-up where multiple frames would be presented at consecutive vsyncs.
     * One step of slack allows the normal cadence to absorb minor jitter. */
    if (vsync_time_ns > vp->last_frame_ns + step) {
        if (vp->debug_log) {
            fprintf(vp->debug_log, "# SNAP  | gap=%lu us (>1 step behind wall clock)\n",
                    (unsigned long)((vsync_time_ns - vp->last_frame_ns) / 1000));
        }
        vp->debug_total_snaps++;
        vp->last_frame_ns = vsync_time_ns;
    }
    return 1;
}

/* ================================================================
 *  Frame Presentation with A/V Sync
 * ================================================================ */

void videoplayer_present_frame(VideoPlayer *vp, uint64_t vsync_time_ns)
{
    if (!vp || (vp->state != VP_STATE_PLAYING && vp->state != VP_STATE_BUFFERING))
        return;

    /* Pre-buffer: while in BUFFERING state (initial open), wait for the decode
     * thread to fill the frame queue before starting playback. This gives the
     * decode thread a head start so the queue has enough frames to absorb
     * decode jitter. Without this, frames are consumed as fast as produced,
     * keeping the queue at 0-1 frames where any jitter causes stutter.
     * Seeking naturally provides this head start (present_frame returns while
     * seek_requested is set), which is why seeking back fixes stutter. */
    if (vp->state == VP_STATE_BUFFERING) {
        /* Don't start playback while a seek is pending — the queue may
         * contain pre-seek frames that would flash wrong content. */
        if (vp->seek_requested)
            return;

        pthread_mutex_lock(&vp->frame_mutex);
        int queued = vp->frames_queued;
        pthread_mutex_unlock(&vp->frame_mutex);

        /* Also check audio ring buffer has enough data for smooth start.
         * Without this, PipeWire starts consuming immediately on PLAY while
         * the decode thread can barely keep up, causing 75%+ underruns.
         * Require ~0.5s of audio so the ring buffer can absorb decode jitter. */
        size_t audio_avail = 0;
        size_t audio_min = 0;
        if (vp->audio_track_count > 0 && vp->audio.sample_rate > 0) {
            pthread_mutex_lock(&vp->audio.ring.lock);
            audio_avail = vp->audio.ring.available;
            pthread_mutex_unlock(&vp->audio.ring.lock);
            audio_min = (size_t)vp->audio.sample_rate / 2 *
                        vp->audio.channels * sizeof(float);  /* 0.5 seconds */
        }

        if (vp->debug_log) {
            fprintf(vp->debug_log, "%6d | BUFFER | %2d/%2d | audio=%zu/%zu\n",
                    vp->debug_frame_count++, queued, VP_FRAME_QUEUE_SIZE * 3 / 4,
                    audio_avail, audio_min);
            fflush(vp->debug_log);
        }

        if (queued >= VP_FRAME_QUEUE_SIZE * 3 / 4 &&
            (audio_avail >= audio_min || vp->audio_track_count == 0)) {
            /* Buffer ready - start playback (audio + video together) */
            videoplayer_play(vp);
        } else {
            return;
        }
    }

    /* Check if audio thread requested a timing reset (e.g., after BT reconnection).
     * This ensures video frame presentation resumes immediately even if the old
     * timing state would have delayed presentation. */
    if (vp->audio.needs_timing_reset) {
        vp->audio.needs_timing_reset = 0;
        vp->last_frame_ns = 0;
        vp->current_repeat = 0;
        vp->cadence_accum = 0.0f;
        fprintf(stderr, "[videoplayer] Frame timing reset (audio state change)\n");
        if (vp->debug_log) {
            fprintf(vp->debug_log, "# RESET | timing reset (audio state change)\n");
            fflush(vp->debug_log);
        }
    }

    /* Periodically check if audio stream needs recovery (every ~0.5 second at 60Hz).
     * This handles Bluetooth controller reconnection etc. without blocking. */
    if (++vp->recovery_check_counter >= 30) {
        vp->recovery_check_counter = 0;
        videoplayer_audio_check_recovery(vp);
    }

    /* Don't present stale pre-seek frames. When seeking (e.g., resume playback),
     * the frame queue may contain frames from the old position. Presenting them
     * causes a brief flash of wrong content before the seek completes. */
    if (vp->seek_requested)
        return;

    /* Video stall detector: if frames are available but we haven't presented
     * any frame for >500ms, force-reset timing. This catches edge cases where
     * the frame pacing logic gets stuck (e.g., after audio device changes).
     * Note: the rebuffer logic (queue-empty → BUFFERING) handles the common
     * case of HTTP buffering stalls. This detector handles the rare case where
     * frames exist but timing is stuck. */
    if (vp->last_present_time_ns > 0 &&
        vsync_time_ns > vp->last_present_time_ns + 500000000ULL) {
        pthread_mutex_lock(&vp->frame_mutex);
        int has_frames = vp->frames_queued > 0;
        pthread_mutex_unlock(&vp->frame_mutex);

        if (has_frames) {
            fprintf(stderr, "[videoplayer] Video stall detected (>500ms), forcing timing reset\n");
            if (vp->debug_log) {
                fprintf(vp->debug_log, "# STALL | >500ms without presentation, forcing timing reset\n");
                fflush(vp->debug_log);
            }
            vp->last_frame_ns = 0;
            vp->current_repeat = 0;
            vp->cadence_accum = 0.0f;
            /* Reset A/V sync for clean restart */
            vp->av_sync_established = 0;
        }
    }

    struct wlr_buffer *buffer = NULL;
    int frames_consumed = 0;
    int max_skip = VP_FRAME_QUEUE_SIZE - 2;  /* Allow rapid catch-up, keep 2 frames in queue */

    vp->debug_vsync_count++;  /* Count vsyncs reaching frame logic */

present_next:
    pthread_mutex_lock(&vp->frame_mutex);

    if (vp->frames_queued == 0) {
        pthread_mutex_unlock(&vp->frame_mutex);

        /* If we're in PLAYING state and the queue is empty (HTTP buffering stall),
         * transition back to BUFFERING so the decode thread can refill the queue
         * before playback resumes.  This prevents the video from appearing frozen
         * for 25+ seconds while the queue stays at 0.  videoplayer_play() is called
         * again once enough frames are queued, which resets A/V sync bases for
         * a clean restart. Flush audio so it restarts in sync with video. */
        if (vp->state == VP_STATE_PLAYING && !vp->demux_eof) {
            vp->state = VP_STATE_BUFFERING;
            vp->last_frame_ns = 0;
            vp->last_present_time_ns = 0;
            vp->current_repeat = 0;
            vp->cadence_accum = 0.0f;

            /* Pause and flush audio so it restarts aligned with video */
            extern void videoplayer_audio_pause(VideoPlayer *vp);
            extern void videoplayer_audio_flush(VideoPlayer *vp);
            videoplayer_audio_pause(vp);
            videoplayer_audio_flush(vp);

            fprintf(stderr, "[videoplayer] Queue empty during playback, rebuffering\n");
            if (vp->debug_log) {
                fprintf(vp->debug_log, "# REBUFFER | queue empty, transitioning to BUFFERING\n");
                fflush(vp->debug_log);
            }
        }

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

    /* Take the pre-converted buffer (drop previous if any) */
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
    /* Buffer is already BGRA - just set it on the scene node */
    if (buffer) {
        videoplayer_update_frame_buffer(vp, buffer);
        wlr_buffer_drop(buffer);  /* Scene holds its own reference */
        vp->last_present_time_ns = vsync_time_ns;
    }

    /* Debug log: comprehensive per-frame metrics */
    if (vp->debug_log && vp->state == VP_STATE_PLAYING) {
        int q;
        pthread_mutex_lock(&vp->frame_mutex);
        q = vp->frames_queued;
        pthread_mutex_unlock(&vp->frame_mutex);

        if (buffer) {
            /* Frame presented — log cadence and timing */
            uint64_t actual_us = 0;
            if (vp->debug_last_present_ns > 0)
                actual_us = (vsync_time_ns - vp->debug_last_present_ns) / 1000;
            uint64_t expected_us = vp->frame_interval_ns / 1000;

            fprintf(vp->debug_log,
                    "%6d | PLAY   | q=%2d | avsync=%+7ld | pos=%7ld | v=%2d | dt=%5lu/%5lu us",
                    vp->debug_frame_count, q,
                    (long)vp->av_sync_offset_us,
                    (long)(vp->position_us / 1000),
                    vp->debug_vsync_count,
                    (unsigned long)actual_us,
                    (unsigned long)expected_us);

            if (frames_consumed > 1) {
                vp->debug_total_skips += frames_consumed - 1;
                fprintf(vp->debug_log, " | SKIP %d (total=%d)",
                        frames_consumed - 1, vp->debug_total_skips);
            }
            fprintf(vp->debug_log, "\n");

            vp->debug_last_present_ns = vsync_time_ns;
            vp->debug_vsync_count = 0;
        } else {
            /* Queue empty when frame was needed */
            vp->debug_total_empty++;
            fprintf(vp->debug_log,
                    "%6d | EMPTY  | q=%2d | avsync=%+7ld | pos=%7ld | v=%2d | total_empty=%d\n",
                    vp->debug_frame_count, q,
                    (long)vp->av_sync_offset_us,
                    (long)(vp->position_us / 1000),
                    vp->debug_vsync_count,
                    vp->debug_total_empty);
        }

        vp->debug_frame_count++;

        /* Flush on events and periodically (~1 sec at 24fps) */
        if (!buffer || frames_consumed > 1 || vp->debug_frame_count % 60 == 0)
            fflush(vp->debug_log);
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

    /* Create seek OSD overlay (above subtitles) */
    vp->seek_osd_tree = wlr_scene_tree_create(vp->tree);
    if (vp->seek_osd_tree) {
        wlr_scene_node_raise_to_top(&vp->seek_osd_tree->node);
        wlr_scene_node_set_enabled(&vp->seek_osd_tree->node, false);
    }

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

    /* Control bar is rendered in the compositor's playback OSD (output.c),
     * not as a separate videoplayer overlay. */
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
    vp->cadence_accum = 0.0f;
    vp->av_sync_offset_us = 0;

    /* Update debug log with actual display parameters (header was written
     * before setup_display_mode was called, showing 0.00 Hz). */
    if (vp->debug_log) {
        fprintf(vp->debug_log, "# Display updated: %.2f Hz, frame_interval=%lu ns, pacing_mode=%d\n",
                display_hz, (unsigned long)vp->frame_interval_ns, vp->frame_repeat_mode);
        fflush(vp->debug_log);
    }
}

/* ================================================================
 *  Cleanup
 * ================================================================ */

void videoplayer_cleanup_scene(VideoPlayer *vp)
{
    if (!vp)
        return;

    if (vp->seek_osd_timer) {
        wl_event_source_remove(vp->seek_osd_timer);
        vp->seek_osd_timer = NULL;
    }

    if (vp->tree) {
        wlr_scene_node_destroy(&vp->tree->node);
        vp->tree = NULL;
        vp->video_tree = NULL;
        vp->subtitle_tree = NULL;
        vp->seek_osd_tree = NULL;
        vp->frame_node = NULL;
    }
}
