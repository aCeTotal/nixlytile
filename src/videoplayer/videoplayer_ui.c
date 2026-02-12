/*
 * Nixlytile Video Player - UI Module
 * Control bar rendering with Play/Pause, progress, volume, audio, subtitles
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <pixman.h>
#include <cairo/cairo.h>
#include <drm_fourcc.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/interfaces/wlr_buffer.h>

#include "videoplayer.h"

/* ================================================================
 *  Cairo Buffer for Text Rendering
 * ================================================================ */

struct CairoBuffer {
    struct wlr_buffer base;
    cairo_surface_t *surface;
    void *data;
    int stride;
};

static void cairo_buffer_destroy(struct wlr_buffer *wlr_buffer)
{
    struct CairoBuffer *buf = wl_container_of(wlr_buffer, buf, base);

    if (buf->surface)
        cairo_surface_destroy(buf->surface);
    if (buf->data)
        free(buf->data);
    free(buf);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
                                                uint32_t flags,
                                                void **data,
                                                uint32_t *format,
                                                size_t *stride)
{
    struct CairoBuffer *buf = wl_container_of(wlr_buffer, buf, base);

    if (data)
        *data = buf->data;
    if (format)
        *format = DRM_FORMAT_ARGB8888;
    if (stride)
        *stride = buf->stride;

    return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer)
{
    /* Nothing to do */
}

static const struct wlr_buffer_impl cairo_buffer_impl = {
    .destroy = cairo_buffer_destroy,
    .begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
    .end_data_ptr_access = cairo_buffer_end_data_ptr_access,
};

static struct wlr_buffer *create_text_buffer(const char *text, int width, int height,
                                              const float color[4], const char *font,
                                              double font_size)
{
    struct CairoBuffer *buf;
    cairo_t *cr;
    cairo_text_extents_t extents;

    buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    buf->stride = width * 4;
    buf->data = calloc(height, buf->stride);
    if (!buf->data) {
        free(buf);
        return NULL;
    }

    buf->surface = cairo_image_surface_create_for_data(buf->data,
                                                        CAIRO_FORMAT_ARGB32,
                                                        width, height, buf->stride);
    if (cairo_surface_status(buf->surface) != CAIRO_STATUS_SUCCESS) {
        free(buf->data);
        free(buf);
        return NULL;
    }

    cr = cairo_create(buf->surface);

    /* Clear background */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Set font */
    cairo_select_font_face(cr, font, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_size);

    /* Get text extents for centering */
    cairo_text_extents(cr, text, &extents);

    /* Center text */
    double x = (width - extents.width) / 2 - extents.x_bearing;
    double y = (height - extents.height) / 2 - extents.y_bearing;

    /* Draw text */
    cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);

    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);

    wlr_buffer_init(&buf->base, &cairo_buffer_impl, width, height);

    return &buf->base;
}

/* External declarations */
extern struct wl_event_loop *event_loop;
extern void render_playback_osd(void);

/* Hold-to-seek timing constants */
#define SEEK_HOLD_TICK_MS      100   /* Timer interval during hold */
#define SEEK_HOLD_HIDE_MS      1500  /* Auto-hide after seek release */
#define UI_UPDATE_TICK_MS      250   /* Real-time time display update */

/* ================================================================
 *  Colors
 * ================================================================ */

static const float color_bar_bg[4] = { 0.1f, 0.1f, 0.1f, 0.85f };
static const float color_button[4] = { 0.9f, 0.9f, 0.9f, 1.0f };
static const float color_button_hover[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static const float color_progress_bg[4] = { 0.3f, 0.3f, 0.3f, 1.0f };
static const float color_progress_fg[4] = { 0.8f, 0.2f, 0.2f, 1.0f };
static const float color_progress_buffer[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
static const float color_text[4] = { 0.9f, 0.9f, 0.9f, 1.0f };

/* ================================================================
 *  Timing
 * ================================================================ */

static uint64_t get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}

/* ================================================================
 *  Auto-hide Timer
 * ================================================================ */

static int control_bar_hide_timer_cb(void *data)
{
    VideoPlayer *vp = data;

    if (!vp)
        return 0;

    VideoPlayerControlBar *bar = &vp->control_bar;

    /* Don't hide if paused, hovered, or dragging */
    if (vp->state != VP_STATE_PLAYING || bar->hovered || bar->dragging_progress) {
        bar->last_activity_ms = get_time_ms();
        return 0;
    }

    /* Hide the control bar */
    videoplayer_hide_control_bar(vp);

    return 0;
}

static void reset_hide_timer(VideoPlayer *vp)
{
    VideoPlayerControlBar *bar = &vp->control_bar;

    bar->last_activity_ms = get_time_ms();

    if (!bar->hide_timer && event_loop) {
        bar->hide_timer = wl_event_loop_add_timer(event_loop,
                                                   control_bar_hide_timer_cb, vp);
    }

    if (bar->hide_timer) {
        wl_event_source_timer_update(bar->hide_timer, CONTROL_BAR_AUTO_HIDE_MS);
    }
}

/* Forward declarations */
static int ui_update_timer_cb(void *data);

/* ================================================================
 *  Control Bar Visibility
 * ================================================================ */

void videoplayer_show_control_bar(VideoPlayer *vp)
{
    if (!vp)
        return;

    VideoPlayerControlBar *bar = &vp->control_bar;

    if (bar->visible)
        goto reset_timer;

    bar->visible = 1;

    if (bar->tree) {
        wlr_scene_node_set_enabled(&bar->tree->node, 1);
    }

    /* Render the control bar */
    videoplayer_render_control_bar(vp);

reset_timer:
    reset_hide_timer(vp);

    /* Start UI update timer for real-time time display */
    if (!bar->ui_update_timer && event_loop) {
        bar->ui_update_timer = wl_event_loop_add_timer(event_loop,
                                                        ui_update_timer_cb, vp);
    }
    if (bar->ui_update_timer &&
        (vp->state == VP_STATE_PLAYING || bar->seek_hold_active)) {
        wl_event_source_timer_update(bar->ui_update_timer, UI_UPDATE_TICK_MS);
    }
}

void videoplayer_hide_control_bar(VideoPlayer *vp)
{
    if (!vp)
        return;

    VideoPlayerControlBar *bar = &vp->control_bar;

    if (!bar->visible)
        return;

    bar->visible = 0;

    if (bar->tree) {
        wlr_scene_node_set_enabled(&bar->tree->node, 0);
    }

    /* Stop UI update timer */
    if (bar->ui_update_timer)
        wl_event_source_timer_update(bar->ui_update_timer, 0);
}

/* ================================================================
 *  Control Bar Initialization
 * ================================================================ */

int videoplayer_init_control_bar(VideoPlayer *vp, struct wlr_scene_tree *parent,
                                  int screen_width, int screen_height)
{
    VideoPlayerControlBar *bar = &vp->control_bar;

    memset(bar, 0, sizeof(*bar));

    /* Calculate dimensions */
    bar->width = screen_width - 2 * CONTROL_BAR_MARGIN;
    bar->height = CONTROL_BAR_HEIGHT;
    bar->x = CONTROL_BAR_MARGIN;
    bar->y = screen_height - CONTROL_BAR_HEIGHT - CONTROL_BAR_MARGIN;

    /* Create scene tree */
    bar->tree = wlr_scene_tree_create(parent);
    if (!bar->tree)
        return -1;

    wlr_scene_node_set_position(&bar->tree->node, bar->x, bar->y);
    wlr_scene_node_set_enabled(&bar->tree->node, 0);

    /* Create background tree */
    bar->bg = wlr_scene_tree_create(bar->tree);
    if (!bar->bg) {
        wlr_scene_node_destroy(&bar->tree->node);
        bar->tree = NULL;
        return -1;
    }

    /* Calculate element positions */
    int padding = 10;
    int button_size = CONTROL_BAR_BUTTON_SIZE;

    /* Play button - left side */
    bar->play_btn_x = padding;
    bar->play_btn_w = button_size;

    /* Right side buttons (from right to left) */
    int right_x = bar->width - padding;

    /* Subtitle button (wide enough for language list like "English, Norwegian") */
    bar->subtitle_w = 200;
    bar->subtitle_x = right_x - bar->subtitle_w;
    right_x = bar->subtitle_x - padding;

    /* Audio button (wide enough for "Atmos Passthrough") */
    bar->audio_w = 170;
    bar->audio_x = right_x - bar->audio_w;
    right_x = bar->audio_x - padding;

    /* Volume button */
    bar->volume_w = button_size;
    bar->volume_x = right_x - bar->volume_w;
    right_x = bar->volume_x - padding;

    /* Time display (position / duration  -remaining) */
    bar->time_w = 260;
    bar->time_x = right_x - bar->time_w;
    right_x = bar->time_x - padding;

    /* Progress bar - fills remaining space */
    bar->progress_x = bar->play_btn_x + bar->play_btn_w + padding * 2;
    bar->progress_w = right_x - bar->progress_x - padding;

    bar->visible = 0;
    bar->hovered = 0;
    bar->dragging_progress = 0;
    bar->last_activity_ms = get_time_ms();

    return 0;
}

void videoplayer_cleanup_control_bar(VideoPlayer *vp)
{
    if (!vp)
        return;

    VideoPlayerControlBar *bar = &vp->control_bar;

    if (bar->hide_timer) {
        wl_event_source_remove(bar->hide_timer);
        bar->hide_timer = NULL;
    }

    if (bar->seek_hold_timer) {
        wl_event_source_remove(bar->seek_hold_timer);
        bar->seek_hold_timer = NULL;
    }
    bar->seek_hold_active = 0;

    if (bar->ui_update_timer) {
        wl_event_source_remove(bar->ui_update_timer);
        bar->ui_update_timer = NULL;
    }

    if (bar->tree) {
        wlr_scene_node_destroy(&bar->tree->node);
        bar->tree = NULL;
        bar->bg = NULL;
    }
}

/* ================================================================
 *  Drawing Helpers
 * ================================================================ */

static void draw_rect(struct wlr_scene_tree *parent, int x, int y,
                      int width, int height, const float color[4])
{
    struct wlr_scene_rect *rect;

    if (!parent || width <= 0 || height <= 0)
        return;

    rect = wlr_scene_rect_create(parent, width, height, color);
    if (rect) {
        wlr_scene_node_set_position(&rect->node, x, y);
    }
}

static void draw_rounded_rect(struct wlr_scene_tree *parent, int x, int y,
                               int width, int height, const float color[4], int radius)
{
    /* Simple rounded rect using horizontal strips */
    int yy, inset_top, inset_bottom, inset, start, h, w;

    if (!parent || width <= 0 || height <= 0)
        return;

    if (radius > height / 2)
        radius = height / 2;
    if (radius > width / 2)
        radius = width / 2;

    yy = 0;
    while (yy < height) {
        inset_top = (radius > 0) ? (int)fmax(0, radius - yy) : 0;
        inset_bottom = (radius > 0) ? (int)fmax(0, radius - ((height - 1) - yy)) : 0;
        inset = (inset_top > inset_bottom) ? inset_top : inset_bottom;
        start = yy;

        while (yy < height) {
            int it = (radius > 0) ? (int)fmax(0, radius - yy) : 0;
            int ib = (radius > 0) ? (int)fmax(0, radius - ((height - 1) - yy)) : 0;
            if ((it > ib ? it : ib) != inset)
                break;
            yy++;
        }

        h = yy - start;
        w = width - 2 * inset;
        if (h > 0 && w > 0) {
            draw_rect(parent, x + inset, y + start, w, h, color);
        }
    }
}

/* ================================================================
 *  Control Bar Rendering
 * ================================================================ */

static const char *audio_codec_display_name(AudioTrack *track)
{
    if (track->is_atmos) return "Atmos";
    switch (track->codec_id) {
    case AV_CODEC_ID_TRUEHD: return "TrueHD";
    case AV_CODEC_ID_AC3: return "AC3";
    case AV_CODEC_ID_EAC3: return "EAC3";
    case AV_CODEC_ID_DTS: return "DTS";
    case AV_CODEC_ID_AAC: return "AAC";
    case AV_CODEC_ID_FLAC: return "FLAC";
    case AV_CODEC_ID_OPUS: return "Opus";
    case AV_CODEC_ID_VORBIS: return "Vorbis";
    case AV_CODEC_ID_MP3: return "MP3";
    default: return avcodec_get_name(track->codec_id);
    }
}

static const char *language_full_name(const char *code)
{
    if (!code || !code[0])
        return NULL;

    static const struct { const char *code; const char *name; } langs[] = {
        {"eng", "English"}, {"en", "English"},
        {"nor", "Norwegian"}, {"nob", "Norwegian"}, {"nno", "Norwegian"}, {"no", "Norwegian"},
        {"swe", "Swedish"}, {"sv", "Swedish"},
        {"dan", "Danish"}, {"da", "Danish"},
        {"fin", "Finnish"}, {"fi", "Finnish"},
        {"deu", "German"}, {"ger", "German"}, {"de", "German"},
        {"fra", "French"}, {"fre", "French"}, {"fr", "French"},
        {"spa", "Spanish"}, {"es", "Spanish"},
        {"ita", "Italian"}, {"it", "Italian"},
        {"por", "Portuguese"}, {"pt", "Portuguese"},
        {"jpn", "Japanese"}, {"ja", "Japanese"},
        {"kor", "Korean"}, {"ko", "Korean"},
        {"zho", "Chinese"}, {"chi", "Chinese"}, {"zh", "Chinese"},
        {"rus", "Russian"}, {"ru", "Russian"},
        {"ara", "Arabic"}, {"ar", "Arabic"},
        {"hin", "Hindi"}, {"hi", "Hindi"},
        {"pol", "Polish"}, {"pl", "Polish"},
        {"nld", "Dutch"}, {"dut", "Dutch"}, {"nl", "Dutch"},
        {"tur", "Turkish"}, {"tr", "Turkish"},
        {"tha", "Thai"}, {"th", "Thai"},
        {"vie", "Vietnamese"}, {"vi", "Vietnamese"},
        {"ces", "Czech"}, {"cze", "Czech"}, {"cs", "Czech"},
        {"hun", "Hungarian"}, {"hu", "Hungarian"},
        {"ron", "Romanian"}, {"rum", "Romanian"}, {"ro", "Romanian"},
        {"ell", "Greek"}, {"gre", "Greek"}, {"el", "Greek"},
        {"heb", "Hebrew"}, {"he", "Hebrew"},
        {"ind", "Indonesian"}, {"id", "Indonesian"},
        {"ukr", "Ukrainian"}, {"uk", "Ukrainian"},
        {"hrv", "Croatian"}, {"hr", "Croatian"},
        {"srp", "Serbian"}, {"sr", "Serbian"},
        {"bul", "Bulgarian"}, {"bg", "Bulgarian"},
        {"slk", "Slovak"}, {"slo", "Slovak"}, {"sk", "Slovak"},
        {"slv", "Slovenian"}, {"sl", "Slovenian"},
        {"isl", "Icelandic"}, {"ice", "Icelandic"}, {"is", "Icelandic"},
        {"cat", "Catalan"}, {"ca", "Catalan"},
        {"est", "Estonian"}, {"et", "Estonian"},
        {"lav", "Latvian"}, {"lv", "Latvian"},
        {"lit", "Lithuanian"}, {"lt", "Lithuanian"},
        {"msa", "Malay"}, {"may", "Malay"}, {"ms", "Malay"},
        {"und", "Unknown"},
    };

    for (size_t i = 0; i < sizeof(langs) / sizeof(langs[0]); i++) {
        if (strcmp(code, langs[i].code) == 0)
            return langs[i].name;
    }

    return NULL;
}

static const char *subtitle_format_name(SubtitleTrack *track)
{
    switch (track->codec_id) {
    case AV_CODEC_ID_ASS:
    case AV_CODEC_ID_SSA: return "ASS";
    case AV_CODEC_ID_SUBRIP:
    case AV_CODEC_ID_SRT: return "SRT";
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE: return "PGS";
    case AV_CODEC_ID_DVD_SUBTITLE: return "VOB";
    case AV_CODEC_ID_WEBVTT: return "VTT";
    case AV_CODEC_ID_MOV_TEXT: return "TX3G";
    default: return track->is_text_based ? "Text" : "Bitmap";
    }
}

void videoplayer_render_control_bar(VideoPlayer *vp)
{
    VideoPlayerControlBar *bar;
    struct wlr_scene_node *node, *tmp;
    int y_center;
    float progress;
    char time_str[64];

    if (!vp)
        return;

    bar = &vp->control_bar;

    if (!bar->tree || !bar->bg)
        return;

    /* Clear existing content */
    wl_list_for_each_safe(node, tmp, &bar->bg->children, link) {
        wlr_scene_node_destroy(node);
    }

    y_center = bar->height / 2;

    /* Background */
    draw_rounded_rect(bar->bg, 0, 0, bar->width, bar->height, color_bar_bg, 8);

    /* Play/Pause button */
    {
        int btn_y = y_center - CONTROL_BAR_BUTTON_SIZE / 2;
        const float *btn_color = bar->hovered ? color_button_hover : color_button;

        if (vp->state == VP_STATE_PLAYING) {
            /* Pause icon - two vertical bars */
            int bar_w = 8;
            int bar_h = 24;
            int gap = 6;
            int total_w = bar_w * 2 + gap;
            int icon_x = bar->play_btn_x + (bar->play_btn_w - total_w) / 2;
            int icon_y = btn_y + (CONTROL_BAR_BUTTON_SIZE - bar_h) / 2;

            draw_rect(bar->bg, icon_x, icon_y, bar_w, bar_h, btn_color);
            draw_rect(bar->bg, icon_x + bar_w + gap, icon_y, bar_w, bar_h, btn_color);
        } else {
            /* Play icon - triangle (approximated with rects) */
            int tri_w = 20;
            int tri_h = 24;
            int icon_x = bar->play_btn_x + (bar->play_btn_w - tri_w) / 2;
            int icon_y = btn_y + (CONTROL_BAR_BUTTON_SIZE - tri_h) / 2;

            /* Simple triangle approximation */
            for (int i = 0; i < tri_h; i++) {
                int w = (i < tri_h / 2) ? (i * tri_w / (tri_h / 2)) : ((tri_h - 1 - i) * tri_w / (tri_h / 2));
                if (w > 0) {
                    draw_rect(bar->bg, icon_x, icon_y + i, w, 1, btn_color);
                }
            }
        }
    }

    /* Progress bar */
    {
        int prog_y = y_center - 4;
        int prog_h = 8;

        /* Background */
        draw_rounded_rect(bar->bg, bar->progress_x, prog_y,
                          bar->progress_w, prog_h, color_progress_bg, 4);

        /* Progress fill */
        if (vp->duration_us > 0) {
            int64_t display_pos = (vp->seek_requested || vp->control_bar.seek_hold_active)
                ? vp->seek_target_us : vp->position_us;
            progress = (float)display_pos / vp->duration_us;
            if (progress < 0.0f) progress = 0.0f;
            if (progress > 1.0f) progress = 1.0f;

            int fill_w = (int)(bar->progress_w * progress);
            if (fill_w > 0) {
                draw_rounded_rect(bar->bg, bar->progress_x, prog_y,
                                  fill_w, prog_h, color_progress_fg, 4);
            }
        }
    }

    /* Time display: position / duration  -remaining */
    {
        char pos_str[16], dur_str[16], rem_str[16];
        int64_t pos = (vp->seek_requested || vp->control_bar.seek_hold_active)
            ? vp->seek_target_us : vp->position_us;
        int64_t remaining = vp->duration_us - pos;
        if (remaining < 0) remaining = 0;

        videoplayer_format_time(pos_str, sizeof(pos_str), pos);
        videoplayer_format_time(dur_str, sizeof(dur_str), vp->duration_us);
        videoplayer_format_time(rem_str, sizeof(rem_str), remaining);
        snprintf(time_str, sizeof(time_str), "%s / %s  -%s", pos_str, dur_str, rem_str);

        /* Render time text using Cairo */
        struct wlr_buffer *text_buf = create_text_buffer(time_str,
                                                          bar->time_w, 24,
                                                          color_text, "monospace", 13.0);
        if (text_buf) {
            struct wlr_scene_buffer *text_node = wlr_scene_buffer_create(bar->bg, text_buf);
            if (text_node) {
                int text_y = y_center - 12;
                wlr_scene_node_set_position(&text_node->node, bar->time_x, text_y);
            }
            wlr_buffer_drop(text_buf);
        }
    }

    /* Volume button */
    {
        int btn_y = y_center - CONTROL_BAR_BUTTON_SIZE / 2;
        int icon_x = bar->volume_x + 8;
        int icon_y = btn_y + 8;
        int icon_h = 24;

        if (vp->muted) {
            /* Muted - X over speaker */
            draw_rect(bar->bg, icon_x, icon_y + 8, 12, 8, color_button);
            /* X marks */
            draw_rect(bar->bg, icon_x + 16, icon_y + 4, 2, 16, color_button);
            draw_rect(bar->bg, icon_x + 20, icon_y + 4, 2, 16, color_button);
        } else {
            /* Speaker icon */
            draw_rect(bar->bg, icon_x, icon_y + 8, 8, 8, color_button);
            draw_rect(bar->bg, icon_x + 8, icon_y + 4, 4, 16, color_button);

            /* Volume waves based on level */
            if (vp->volume > 0.3f) {
                draw_rect(bar->bg, icon_x + 16, icon_y + 6, 2, 12, color_button);
            }
            if (vp->volume > 0.6f) {
                draw_rect(bar->bg, icon_x + 20, icon_y + 4, 2, 16, color_button);
            }
        }
        (void)icon_h;
    }

    /* Audio track button - show quality and channel layout */
    {
        char audio_label[64];

        if (vp->audio_track_count > 0) {
            AudioTrack *track = &vp->audio_tracks[vp->current_audio_track];
            const char *ch_str;

            switch (track->channels) {
            case 8:  ch_str = "7.1"; break;
            case 6:  ch_str = "5.1"; break;
            case 2:  ch_str = "Stereo"; break;
            case 1:  ch_str = "Mono"; break;
            default: ch_str = ""; break;
            }

            if (track->is_atmos) {
                if (vp->audio.passthrough_mode)
                    snprintf(audio_label, sizeof(audio_label), "Atmos Passthrough");
                else
                    snprintf(audio_label, sizeof(audio_label), "Atmos %s", ch_str);
            } else if (track->is_lossless) {
                snprintf(audio_label, sizeof(audio_label), "Lossless %s", ch_str);
            } else {
                const char *codec_display = audio_codec_display_name(track);
                snprintf(audio_label, sizeof(audio_label), "%s %s", codec_display, ch_str);
            }
        } else {
            snprintf(audio_label, sizeof(audio_label), "No Audio");
        }

        struct wlr_buffer *audio_buf = create_text_buffer(audio_label,
                                                           bar->audio_w, 24,
                                                           color_text, "sans-serif", 12.0);
        if (audio_buf) {
            struct wlr_scene_buffer *audio_node = wlr_scene_buffer_create(bar->bg, audio_buf);
            if (audio_node) {
                wlr_scene_node_set_position(&audio_node->node, bar->audio_x, y_center - 12);
            }
            wlr_buffer_drop(audio_buf);
        }
    }

    /* Subtitle button - list available subtitle languages */
    {
        char sub_label[128];

        if (vp->subtitle_track_count > 0) {
            /* Collect unique language names */
            const char *seen[VP_MAX_SUBTITLE_TRACKS];
            int seen_count = 0;
            int pos = 0;

            for (int i = 0; i < vp->subtitle_track_count; i++) {
                SubtitleTrack *track = &vp->subtitle_tracks[i];
                const char *name = language_full_name(track->language);
                if (!name)
                    name = track->language[0] ? track->language : "Unknown";

                /* Deduplicate */
                int dup = 0;
                for (int j = 0; j < seen_count; j++) {
                    if (strcmp(seen[j], name) == 0) { dup = 1; break; }
                }
                if (dup) continue;
                seen[seen_count++] = name;

                /* Append to label with comma separator */
                if (pos > 0) {
                    int w = snprintf(sub_label + pos, sizeof(sub_label) - pos, ", ");
                    if (w > 0) pos += w;
                }
                int w = snprintf(sub_label + pos, sizeof(sub_label) - pos, "%s", name);
                if (w > 0) pos += w;
                if (pos >= (int)sizeof(sub_label) - 4) break;
            }
        } else {
            snprintf(sub_label, sizeof(sub_label), "No Subs");
        }

        struct wlr_buffer *sub_buf = create_text_buffer(sub_label,
                                                         bar->subtitle_w, 24,
                                                         color_text, "sans-serif", 12.0);
        if (sub_buf) {
            struct wlr_scene_buffer *sub_node = wlr_scene_buffer_create(bar->bg, sub_buf);
            if (sub_node) {
                wlr_scene_node_set_position(&sub_node->node, bar->subtitle_x, y_center - 12);
            }
            wlr_buffer_drop(sub_buf);
        }
    }
}

/* ================================================================
 *  Hold-to-Seek
 * ================================================================ */

static int64_t seek_hold_increment(int tick_count)
{
    /* Acceleration: the longer you hold, the faster it seeks.
     * At 100ms ticks: 0-5 ticks = 20x, 5-20 = 60x, 20-50 = 120x, 50+ = 240x */
    if (tick_count < 5)
        return 2000000LL;   /* 2s per tick = 20x speed */
    else if (tick_count < 20)
        return 6000000LL;   /* 6s per tick = 60x speed */
    else if (tick_count < 50)
        return 12000000LL;  /* 12s per tick = 120x speed */
    else
        return 24000000LL;  /* 24s per tick = 240x speed */
}

static int seek_hold_timer_cb(void *data)
{
    VideoPlayer *vp = data;
    VideoPlayerControlBar *bar;

    if (!vp)
        return 0;

    bar = &vp->control_bar;
    if (!bar->seek_hold_active)
        return 0;

    int64_t increment = seek_hold_increment(bar->seek_hold_count);
    int64_t new_pos = vp->seek_target_us + bar->seek_hold_direction * increment;

    /* Clamp */
    if (new_pos < 0)
        new_pos = 0;
    if (vp->duration_us > 0 && new_pos > vp->duration_us)
        new_pos = vp->duration_us;

    vp->seek_target_us = new_pos;
    bar->seek_hold_count++;

    /* Re-render OSD for real-time update */
    render_playback_osd();
    videoplayer_render_control_bar(vp);

    /* Schedule next tick */
    if (bar->seek_hold_timer)
        wl_event_source_timer_update(bar->seek_hold_timer, SEEK_HOLD_TICK_MS);

    return 0;
}

static void reset_hide_timer_ms(VideoPlayer *vp, int ms)
{
    VideoPlayerControlBar *bar = &vp->control_bar;

    bar->last_activity_ms = get_time_ms();

    if (!bar->hide_timer && event_loop) {
        bar->hide_timer = wl_event_loop_add_timer(event_loop,
                                                   control_bar_hide_timer_cb, vp);
    }

    if (bar->hide_timer) {
        wl_event_source_timer_update(bar->hide_timer, ms);
    }
}

void videoplayer_seek_hold_start(VideoPlayer *vp, int direction)
{
    VideoPlayerControlBar *bar;

    if (!vp)
        return;

    bar = &vp->control_bar;

    /* If already holding in the same direction, ignore (key repeat) */
    if (bar->seek_hold_active && bar->seek_hold_direction == direction)
        return;

    /* If holding in opposite direction, stop first */
    if (bar->seek_hold_active)
        videoplayer_seek_hold_stop(vp);

    bar->seek_hold_active = 1;
    bar->seek_hold_direction = direction;
    bar->seek_hold_start_ms = get_time_ms();
    bar->seek_hold_count = 0;
    bar->seek_hold_base_us = vp->seek_requested ? vp->seek_target_us : vp->position_us;

    /* Initial seek - immediate first step (10 seconds) */
    int64_t initial_seek = 10 * 1000000LL;
    vp->seek_target_us = bar->seek_hold_base_us + direction * initial_seek;

    /* Clamp */
    if (vp->seek_target_us < 0)
        vp->seek_target_us = 0;
    if (vp->duration_us > 0 && vp->seek_target_us > vp->duration_us)
        vp->seek_target_us = vp->duration_us;

    /* Show control bar and OSD */
    videoplayer_show_control_bar(vp);
    render_playback_osd();

    /* Start hold timer */
    if (!bar->seek_hold_timer && event_loop) {
        bar->seek_hold_timer = wl_event_loop_add_timer(event_loop,
                                                        seek_hold_timer_cb, vp);
    }
    if (bar->seek_hold_timer)
        wl_event_source_timer_update(bar->seek_hold_timer, SEEK_HOLD_TICK_MS);
}

void videoplayer_seek_hold_stop(VideoPlayer *vp)
{
    VideoPlayerControlBar *bar;

    if (!vp)
        return;

    bar = &vp->control_bar;

    if (!bar->seek_hold_active)
        return;

    bar->seek_hold_active = 0;

    /* Stop timer */
    if (bar->seek_hold_timer)
        wl_event_source_timer_update(bar->seek_hold_timer, 0);

    /* Commit the seek to the actual position */
    videoplayer_seek(vp, vp->seek_target_us);

    /* Update OSD */
    render_playback_osd();

    /* Schedule auto-hide after 1.5 seconds */
    reset_hide_timer_ms(vp, SEEK_HOLD_HIDE_MS);
}

/* ================================================================
 *  Real-time UI Update Timer
 * ================================================================ */

static int ui_update_timer_cb(void *data)
{
    VideoPlayer *vp = data;

    if (!vp || !vp->control_bar.visible)
        return 0;

    /* Re-render to update time display */
    videoplayer_render_control_bar(vp);
    render_playback_osd();

    /* Continue if still visible and playing (or hold-seeking) */
    if (vp->control_bar.visible &&
        (vp->state == VP_STATE_PLAYING || vp->control_bar.seek_hold_active)) {
        if (vp->control_bar.ui_update_timer)
            wl_event_source_timer_update(vp->control_bar.ui_update_timer, UI_UPDATE_TICK_MS);
    }

    return 0;
}

/* ================================================================
 *  Seek OSD
 * ================================================================ */

#define SEEK_OSD_HIDE_MS  1500
#define SEEK_OSD_WIDTH    360
#define SEEK_OSD_HEIGHT   44

static int seek_osd_hide_timer_cb(void *data)
{
    VideoPlayer *vp = data;

    if (vp)
        videoplayer_hide_seek_osd(vp);

    return 0;
}

void videoplayer_show_seek_osd(VideoPlayer *vp)
{
    if (!vp)
        return;

    /* Show seek info directly in the control bar instead of a separate OSD */
    videoplayer_show_control_bar(vp);
}

void videoplayer_hide_seek_osd(VideoPlayer *vp)
{
    if (!vp || !vp->seek_osd_tree)
        return;

    wlr_scene_node_set_enabled(&vp->seek_osd_tree->node, false);
}

/* ================================================================
 *  Subtitle Rendering
 * ================================================================ */

static struct wlr_buffer *create_subtitle_buffer(ASS_Image *images, int width, int height)
{
    struct CairoBuffer *buf;
    ASS_Image *img;

    if (!images || width <= 0 || height <= 0)
        return NULL;

    buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    buf->stride = width * 4;
    buf->data = calloc(height, buf->stride);
    if (!buf->data) {
        free(buf);
        return NULL;
    }

    buf->surface = cairo_image_surface_create_for_data(buf->data,
                                                        CAIRO_FORMAT_ARGB32,
                                                        width, height, buf->stride);
    if (cairo_surface_status(buf->surface) != CAIRO_STATUS_SUCCESS) {
        free(buf->data);
        free(buf);
        return NULL;
    }

    cairo_t *cr = cairo_create(buf->surface);

    /* Clear background (transparent) */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Composite all ASS_Image bitmaps */
    for (img = images; img != NULL; img = img->next) {
        if (img->w == 0 || img->h == 0)
            continue;

        /* ASS_Image color format: 0xRRGGBBAA
         * Override colors at render time to ensure clean appearance
         * regardless of embedded ASS styles (green backgrounds etc.) */
        uint32_t color = img->color;
        double r, g, b, a;

        switch (img->type) {
        case IMAGE_TYPE_CHARACTER:
            /* White text, fully opaque */
            r = 1.0; g = 1.0; b = 1.0;
            a = 1.0 - ((color & 0xFF) / 255.0);
            break;
        case IMAGE_TYPE_OUTLINE:
            /* Black outline, fully opaque */
            r = 0.0; g = 0.0; b = 0.0;
            a = 1.0 - ((color & 0xFF) / 255.0);
            break;
        case IMAGE_TYPE_SHADOW:
            /* Black shadow, semi-transparent */
            r = 0.0; g = 0.0; b = 0.0;
            a = (1.0 - ((color & 0xFF) / 255.0)) * 0.5;
            break;
        default:
            /* Unknown image type — skip to prevent colored backgrounds */
            continue;
        }

        /* Create Cairo surface from ASS bitmap (8-bit alpha) */
        cairo_surface_t *ass_surface = cairo_image_surface_create(
            CAIRO_FORMAT_A8, img->w, img->h);

        if (cairo_surface_status(ass_surface) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(ass_surface);
            continue;
        }

        /* Copy ASS bitmap data */
        unsigned char *ass_data = cairo_image_surface_get_data(ass_surface);
        int ass_stride = cairo_image_surface_get_stride(ass_surface);

        for (int y = 0; y < img->h; y++) {
            memcpy(ass_data + y * ass_stride,
                   img->bitmap + y * img->stride,
                   img->w);
        }
        cairo_surface_mark_dirty(ass_surface);

        /* Draw the subtitle glyph */
        cairo_save(cr);
        cairo_translate(cr, img->dst_x, img->dst_y);
        cairo_set_source_rgba(cr, r, g, b, a);
        cairo_mask_surface(cr, ass_surface, 0, 0);
        cairo_restore(cr);

        cairo_surface_destroy(ass_surface);
    }

    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);

    wlr_buffer_init(&buf->base, &cairo_buffer_impl, width, height);

    return &buf->base;
}

static int sub_render_log_count = 0;
static int sub_render_shown_count = 0;

/* Create a wlr_buffer from bitmap subtitle BGRA data, scaled to display size */
static struct wlr_buffer *create_bitmap_subtitle_buffer(VideoPlayer *vp)
{
    struct CairoBuffer *buf;

    if (!vp->subtitle.bitmap_data || !vp->subtitle.bitmap_valid)
        return NULL;

    int fw = vp->fullscreen_width;
    int fh = vp->fullscreen_height;
    if (fw <= 0 || fh <= 0)
        return NULL;

    buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;

    buf->stride = fw * 4;
    buf->data = calloc(fh, buf->stride);
    if (!buf->data) {
        free(buf);
        return NULL;
    }

    buf->surface = cairo_image_surface_create_for_data(buf->data,
                                                        CAIRO_FORMAT_ARGB32,
                                                        fw, fh, buf->stride);
    if (cairo_surface_status(buf->surface) != CAIRO_STATUS_SUCCESS) {
        free(buf->data);
        free(buf);
        return NULL;
    }

    cairo_t *cr = cairo_create(buf->surface);

    /* Clear background (transparent) */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Create a cairo surface from the bitmap BGRA data */
    int bw = vp->subtitle.bitmap_w;
    int bh = vp->subtitle.bitmap_h;
    int bstride = vp->subtitle.bitmap_stride;

    cairo_surface_t *bmp_surface = cairo_image_surface_create_for_data(
        vp->subtitle.bitmap_data, CAIRO_FORMAT_ARGB32, bw, bh, bstride);

    if (cairo_surface_status(bmp_surface) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(buf->surface);
        free(buf->data);
        free(buf);
        return NULL;
    }

    /* Scale bitmap from video coordinates to fullscreen display.
     * PGS subtitle positions are relative to the video frame dimensions.
     * Apply 0.75 downscale since PGS bitmaps are typically designed for
     * cinema-scale viewing and appear oversized on desktop displays.
     * Anchor the bottom-center of the subtitle to its mapped screen position
     * so dialogue subs stay at the bottom while becoming smaller. */
    int vid_w = vp->subtitle.bitmap_video_w;
    int vid_h = vp->subtitle.bitmap_video_h;
    if (vid_w <= 0) vid_w = bw;
    if (vid_h <= 0) vid_h = bh;

    double pgs_scale = 0.75;
    double base_scale_x = (double)fw / vid_w;
    double base_scale_y = (double)fh / vid_h;
    double scale_x = base_scale_x * pgs_scale;
    double scale_y = base_scale_y * pgs_scale;

    double bottom_center_x = (vp->subtitle.bitmap_x + bw / 2.0) * base_scale_x;
    double bottom_y = (vp->subtitle.bitmap_y + bh) * base_scale_y;
    double dst_x = bottom_center_x - (bw * scale_x) / 2.0;
    double dst_y = bottom_y - (bh * scale_y);

    cairo_save(cr);
    cairo_translate(cr, dst_x, dst_y);
    cairo_scale(cr, scale_x, scale_y);
    cairo_set_source_surface(cr, bmp_surface, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_surface_destroy(bmp_surface);
    cairo_destroy(cr);
    cairo_surface_flush(buf->surface);

    wlr_buffer_init(&buf->base, &cairo_buffer_impl, fw, fh);

    return &buf->base;
}

void videoplayer_render_subtitles(VideoPlayer *vp, int64_t pts_us)
{
    struct wlr_buffer *sub_buffer;

    if (!vp)
        return;

    if (vp->current_subtitle_track < 0)
        return;

    if (vp->fullscreen_width <= 0 || vp->fullscreen_height <= 0) {
        if (sub_render_log_count++ < 5)
            fprintf(stderr, "[subtitle] render: fullscreen_size=%dx%d — skipping\n",
                    vp->fullscreen_width, vp->fullscreen_height);
        return;
    }

    long long render_ms = pts_us / 1000;
    int is_bitmap_track = (vp->current_subtitle_track >= 0 &&
                           !vp->subtitle_tracks[vp->current_subtitle_track].is_text_based);

    if (is_bitmap_track) {
        /* ── Bitmap subtitle path (PGS/VOBSUB) ── */
        pthread_mutex_lock(&vp->subtitle_mutex);
        int valid = vp->subtitle.bitmap_valid;
        int64_t bstart = vp->subtitle.bitmap_start_ms;
        int64_t bend = vp->subtitle.bitmap_end_ms;
        pthread_mutex_unlock(&vp->subtitle_mutex);

        /* Check if bitmap is active at current PTS */
        int show = valid && render_ms >= bstart && render_ms < bend;

        /* Log periodically */
        if (sub_render_log_count < 20 || sub_render_log_count % 500 == 0) {
            fprintf(stderr, "[subtitle] render #%d: BITMAP pts=%lldms valid=%d "
                    "range=[%ld,%ld] show=%d shown=%d\n",
                    sub_render_log_count, render_ms, valid,
                    (long)bstart, (long)bend, show, sub_render_shown_count);
        }
        sub_render_log_count++;

        /* Check if we can reuse existing display */
        if (show && vp->subtitle.current_buffer &&
            pts_us >= vp->subtitle.current_pts_us &&
            pts_us < vp->subtitle.current_end_us) {
            return;
        }

        /* Clear old subtitle */
        if (vp->subtitle.current_buffer) {
            wlr_buffer_drop(vp->subtitle.current_buffer);
            vp->subtitle.current_buffer = NULL;
        }

        /* Clear subtitle tree */
        if (vp->subtitle_tree) {
            struct wlr_scene_node *node, *tmp;
            wl_list_for_each_safe(node, tmp, &vp->subtitle_tree->children, link) {
                wlr_scene_node_destroy(node);
            }
        }

        if (!show)
            return;

        /* Create bitmap subtitle buffer (scaled to display) */
        pthread_mutex_lock(&vp->subtitle_mutex);
        sub_buffer = create_bitmap_subtitle_buffer(vp);
        pthread_mutex_unlock(&vp->subtitle_mutex);

        if (!sub_buffer) {
            fprintf(stderr, "[subtitle] render: create_bitmap_subtitle_buffer FAILED\n");
            return;
        }

        /* Display */
        if (vp->subtitle_tree) {
            struct wlr_scene_buffer *sub_node = wlr_scene_buffer_create(
                vp->subtitle_tree, sub_buffer);
            if (sub_node) {
                wlr_scene_node_set_position(&sub_node->node, 0, 0);
                sub_render_shown_count++;
                fprintf(stderr, "[subtitle] render: DISPLAYED bitmap subtitle "
                        "at pts=%lldms (%dx%d)\n",
                        render_ms, vp->fullscreen_width, vp->fullscreen_height);
            }
        }

        vp->subtitle.current_buffer = sub_buffer;
        vp->subtitle.current_pts_us = pts_us;
        vp->subtitle.current_end_us = (int64_t)bend * 1000;
    } else {
        /* ── Text subtitle path (ASS/SRT via libass) ── */
        ASS_Image *images;
        int changed;

        if (!vp->subtitle.renderer || !vp->subtitle.track) {
            if (sub_render_log_count++ < 5)
                fprintf(stderr, "[subtitle] render: renderer=%p track=%p — skipping\n",
                        (void *)vp->subtitle.renderer, (void *)vp->subtitle.track);
            return;
        }

        /* Update frame size if needed */
        ass_set_frame_size(vp->subtitle.renderer,
                           vp->fullscreen_width, vp->fullscreen_height);

        /* Render subtitles for current PTS */
        pthread_mutex_lock(&vp->subtitle_mutex);
        images = ass_render_frame(vp->subtitle.renderer, vp->subtitle.track,
                                   render_ms, &changed);
        pthread_mutex_unlock(&vp->subtitle_mutex);

        if (changed || (sub_render_log_count < 20) ||
            (sub_render_log_count % 500 == 0)) {
            fprintf(stderr, "[subtitle] render #%d: pts=%lldms changed=%d images=%p "
                    "size=%dx%d shown=%d\n",
                    sub_render_log_count, render_ms, changed, (void *)images,
                    vp->fullscreen_width, vp->fullscreen_height,
                    sub_render_shown_count);
        }
        sub_render_log_count++;

        /* Always clear and re-render subtitles.  The libass `changed`
         * flag is unreliable for detecting subtitle expiry — some versions
         * return changed=0 when transitioning from visible→empty, causing
         * expired text to persist on screen.  Re-rendering every frame is
         * cheap (just cairo compositing a few glyphs at video framerate). */

        /* Clear old subtitle display */
        if (vp->subtitle.current_buffer) {
            wlr_buffer_drop(vp->subtitle.current_buffer);
            vp->subtitle.current_buffer = NULL;
        }

        if (vp->subtitle_tree) {
            struct wlr_scene_node *node, *tmp;
            wl_list_for_each_safe(node, tmp, &vp->subtitle_tree->children, link) {
                wlr_scene_node_destroy(node);
            }
        }

        /* No active subtitle at this time — screen is cleared */
        if (!images)
            return;

        sub_buffer = create_subtitle_buffer(images,
                                             vp->fullscreen_width,
                                             vp->fullscreen_height);
        if (!sub_buffer) {
            fprintf(stderr, "[subtitle] render: create_subtitle_buffer FAILED (%dx%d)\n",
                    vp->fullscreen_width, vp->fullscreen_height);
            return;
        }

        if (vp->subtitle_tree) {
            struct wlr_scene_buffer *sub_node = wlr_scene_buffer_create(
                vp->subtitle_tree, sub_buffer);
            if (sub_node) {
                wlr_scene_node_set_position(&sub_node->node, 0, 0);
                sub_render_shown_count++;
                fprintf(stderr, "[subtitle] render: DISPLAYED subtitle at pts=%lldms (%dx%d)\n",
                        render_ms, vp->fullscreen_width, vp->fullscreen_height);
            }
        }

        vp->subtitle.current_buffer = sub_buffer;
    }
}

int videoplayer_subtitle_init(VideoPlayer *vp)
{
    if (!vp)
        return -1;

    fprintf(stderr, "[subtitle] init: starting libass initialization\n");

    /* Initialize libass */
    vp->subtitle.library = ass_library_init();
    if (!vp->subtitle.library) {
        fprintf(stderr, "[subtitle] init: FAILED ass_library_init()\n");
        return -1;
    }
    fprintf(stderr, "[subtitle] init: library=%p\n", (void *)vp->subtitle.library);

    vp->subtitle.renderer = ass_renderer_init(vp->subtitle.library);
    if (!vp->subtitle.renderer) {
        fprintf(stderr, "[subtitle] init: FAILED ass_renderer_init()\n");
        ass_library_done(vp->subtitle.library);
        vp->subtitle.library = NULL;
        return -1;
    }
    fprintf(stderr, "[subtitle] init: renderer=%p\n", (void *)vp->subtitle.renderer);

    /* Configure renderer with initial video dimensions */
    fprintf(stderr, "[subtitle] init: frame_size=%dx%d (video), fullscreen=%dx%d\n",
            vp->video.width, vp->video.height,
            vp->fullscreen_width, vp->fullscreen_height);

    int fw = vp->fullscreen_width > 0 ? vp->fullscreen_width : vp->video.width;
    int fh = vp->fullscreen_height > 0 ? vp->fullscreen_height : vp->video.height;
    ass_set_frame_size(vp->subtitle.renderer, fw, fh);

    /* Use DejaVu Sans as default font family — always available on NixOS.
     * fontconfig autodetect resolves the actual .ttf path.
     * We pass NULL for font path since nix store paths are content-addressed
     * and can change between builds. */
    ass_set_fonts(vp->subtitle.renderer, NULL, "DejaVu Sans",
                  ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    fprintf(stderr, "[subtitle] init: fonts configured (DejaVu Sans + fontconfig)\n");

    /* Scale subtitle fonts down to a reasonable size.
     * File-embedded ASS styles often have oversized fonts (60-80 at PlayRes 1080)
     * which are too large for desktop/TV viewing. */
    ass_set_font_scale(vp->subtitle.renderer, 0.65);

    /* Force style overrides on all subtitle tracks:
     * - Transparent background (no colored boxes — some ASS files use green/etc.)
     * - Outline border for readability on any background
     * - Bottom-center alignment */
    char *style_overrides[] = {
        "BackColour=&HFF000000",     /* Fully transparent (ASS alpha: FF=transparent) */
        "BorderStyle=1",              /* Outline + shadow (not opaque box) */
        "Outline=2",
        "Shadow=1",
        "Alignment=2",               /* Bottom center */
        "MarginV=50",
        NULL
    };
    ass_set_style_overrides(vp->subtitle.library, style_overrides);

    /* Selective style override for dialogue events.
     * This overrides even per-event inline tags (\3c, \4c, \bord, \an, etc.)
     * on events that libass heuristically identifies as normal dialogue,
     * while preserving sign/typesetting positioning. */
    ASS_Style override_style;
    char override_font[] = "DejaVu Sans";
    memset(&override_style, 0, sizeof(override_style));
    override_style.Name = override_font;
    override_style.FontName = override_font;
    override_style.FontSize = 48;
    override_style.PrimaryColour = 0x00FFFFFF;   /* White, fully opaque */
    override_style.SecondaryColour = 0x000000FF;
    override_style.OutlineColour = 0x00000000;   /* Black, fully opaque */
    override_style.BackColour = 0xFF000000;      /* Fully transparent */
    override_style.ScaleX = 1.0;
    override_style.ScaleY = 1.0;
    override_style.BorderStyle = 1;              /* Outline + shadow */
    override_style.Outline = 2.0;
    override_style.Shadow = 1.0;
    override_style.Alignment = 2;                /* Bottom center */
    override_style.MarginV = 50;

    ass_set_selective_style_override_enabled(vp->subtitle.renderer,
        ASS_OVERRIDE_BIT_COLORS | ASS_OVERRIDE_BIT_BORDER | ASS_OVERRIDE_BIT_ALIGNMENT);
    ass_set_selective_style_override(vp->subtitle.renderer, &override_style);

    fprintf(stderr, "[subtitle] init: style overrides set (font_scale=0.65, bottom-center, "
            "no bg, selective override for colors/border/alignment)\n");

    /* Reset render counters */
    sub_render_log_count = 0;
    sub_render_shown_count = 0;

    fprintf(stderr, "[subtitle] init: DONE — library=%p renderer=%p\n",
            (void *)vp->subtitle.library, (void *)vp->subtitle.renderer);

    return 0;
}

void videoplayer_subtitle_cleanup(VideoPlayer *vp)
{
    if (!vp)
        return;

    if (vp->subtitle.track) {
        ass_free_track(vp->subtitle.track);
        vp->subtitle.track = NULL;
    }

    if (vp->subtitle.renderer) {
        ass_renderer_done(vp->subtitle.renderer);
        vp->subtitle.renderer = NULL;
    }

    if (vp->subtitle.library) {
        ass_library_done(vp->subtitle.library);
        vp->subtitle.library = NULL;
    }

    if (vp->subtitle.current_buffer) {
        wlr_buffer_drop(vp->subtitle.current_buffer);
        vp->subtitle.current_buffer = NULL;
    }

    if (vp->subtitle.sws_ctx) {
        sws_freeContext(vp->subtitle.sws_ctx);
        vp->subtitle.sws_ctx = NULL;
    }

    free(vp->subtitle.bitmap_data);
    vp->subtitle.bitmap_data = NULL;
    vp->subtitle.bitmap_valid = 0;

    fprintf(stderr, "[subtitle] Cleanup complete\n");
}
