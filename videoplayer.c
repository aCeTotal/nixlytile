/*
 * Nixlytile Video Player - Main Module
 * Lifecycle, playback control, input handling
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_keyboard.h>

#include "videoplayer.h"

/* External functions from other modules */
extern void videoplayer_close(VideoPlayer *vp);
extern int videoplayer_open(VideoPlayer *vp, const char *filepath);
extern void videoplayer_seek(VideoPlayer *vp, int64_t position_us);
extern void videoplayer_seek_relative(VideoPlayer *vp, int64_t offset_us);
extern void videoplayer_set_audio_track(VideoPlayer *vp, int index);
extern void videoplayer_set_subtitle_track(VideoPlayer *vp, int index);
extern void videoplayer_cycle_audio_track(VideoPlayer *vp);
extern void videoplayer_cycle_subtitle_track(VideoPlayer *vp);
extern void videoplayer_cleanup_scene(VideoPlayer *vp);
extern void videoplayer_show_control_bar(VideoPlayer *vp);
extern void videoplayer_hide_control_bar(VideoPlayer *vp);

/* Forward declarations for audio */
extern int videoplayer_audio_init(VideoPlayer *vp);
extern void videoplayer_audio_cleanup(VideoPlayer *vp);
extern void videoplayer_audio_play(VideoPlayer *vp);
extern void videoplayer_audio_pause(VideoPlayer *vp);

/* Forward declarations for subtitles */
extern int videoplayer_subtitle_init(VideoPlayer *vp);
extern void videoplayer_subtitle_cleanup(VideoPlayer *vp);

/* Seek amounts in microseconds */
#define SEEK_SMALL  (10 * 1000000LL)   /* 10 seconds */
#define SEEK_LARGE  (30 * 1000000LL)   /* 30 seconds */
#define SEEK_HUGE   (60 * 1000000LL)   /* 60 seconds */

/* Volume adjustment */
#define VOLUME_STEP 0.05f

/* ================================================================
 *  Lifecycle
 * ================================================================ */

VideoPlayer *videoplayer_create(struct Monitor *mon)
{
    VideoPlayer *vp;

    vp = calloc(1, sizeof(*vp));
    if (!vp)
        return NULL;

    vp->mon = mon;
    vp->state = VP_STATE_IDLE;
    vp->volume = 1.0f;
    vp->muted = 0;
    vp->speed = 1.0f;
    vp->current_subtitle_track = -1;

    /* Initialize audio mutex */
    pthread_mutex_init(&vp->audio.lock, NULL);

    fprintf(stderr, "[videoplayer] Created video player instance\n");

    return vp;
}

void videoplayer_destroy(VideoPlayer *vp)
{
    if (!vp)
        return;

    /* Stop playback and close file */
    videoplayer_stop(vp);

    /* Cleanup audio */
    videoplayer_audio_cleanup(vp);

    /* Cleanup subtitles */
    videoplayer_subtitle_cleanup(vp);

    /* Cleanup scene */
    videoplayer_cleanup_scene(vp);

    /* Cleanup audio mutex */
    pthread_mutex_destroy(&vp->audio.lock);

    free(vp);

    fprintf(stderr, "[videoplayer] Destroyed video player instance\n");
}

/* ================================================================
 *  Playback Control
 * ================================================================ */

void videoplayer_play(VideoPlayer *vp)
{
    if (!vp)
        return;

    if (vp->state == VP_STATE_IDLE || vp->state == VP_STATE_ERROR)
        return;

    /* Wait for initial buffering (at least 2 frames) - shorter wait for faster start */
    int wait_count = 0;
    while (vp->frames_queued < 2 && wait_count < 50) {
        usleep(10000);  /* 10ms */
        wait_count++;
    }

    /* Reset frame timing so first frame presents immediately.
     * Without this, stale last_frame_ns from before pause causes
     * the cadence-locked timing to either skip ahead or wait. */
    vp->last_frame_ns = 0;
    vp->current_repeat = 0;

    vp->state = VP_STATE_PLAYING;

    /* Start audio playback */
    videoplayer_audio_play(vp);

    fprintf(stderr, "[videoplayer] Play (buffered %d frames)\n", vp->frames_queued);
}

void videoplayer_pause(VideoPlayer *vp)
{
    if (!vp)
        return;

    if (vp->state != VP_STATE_PLAYING)
        return;

    vp->state = VP_STATE_PAUSED;

    /* Pause audio */
    videoplayer_audio_pause(vp);

    /* Show control bar when paused */
    videoplayer_show_control_bar(vp);

    fprintf(stderr, "[videoplayer] Pause\n");
}

void videoplayer_toggle_pause(VideoPlayer *vp)
{
    if (!vp)
        return;

    if (vp->state == VP_STATE_PLAYING) {
        videoplayer_pause(vp);
    } else if (vp->state == VP_STATE_PAUSED) {
        videoplayer_play(vp);
    }
}

void videoplayer_stop(VideoPlayer *vp)
{
    if (!vp)
        return;

    if (vp->state == VP_STATE_IDLE)
        return;

    /* Close the file - this stops decode thread */
    videoplayer_close(vp);

    fprintf(stderr, "[videoplayer] Stop\n");
}

/* ================================================================
 *  Volume Control
 * ================================================================ */

/* Forward declaration */
extern void videoplayer_audio_set_volume(VideoPlayer *vp, float volume);
extern void videoplayer_audio_set_mute(VideoPlayer *vp, int mute);

void videoplayer_set_volume(VideoPlayer *vp, float volume)
{
    if (!vp)
        return;

    if (volume < 0.0f)
        volume = 0.0f;
    if (volume > 1.0f)
        volume = 1.0f;

    vp->volume = volume;

    /* Apply to PipeWire stream */
    videoplayer_audio_set_volume(vp, volume);
}

void videoplayer_adjust_volume(VideoPlayer *vp, float delta)
{
    if (!vp)
        return;

    float new_volume = vp->volume + delta;
    videoplayer_set_volume(vp, new_volume);
}

void videoplayer_toggle_mute(VideoPlayer *vp)
{
    if (!vp)
        return;

    vp->muted = !vp->muted;

    /* Apply to PipeWire stream */
    videoplayer_audio_set_mute(vp, vp->muted);
}

/* ================================================================
 *  Input Handling
 * ================================================================ */

int videoplayer_handle_key(VideoPlayer *vp, uint32_t mods, xkb_keysym_t sym)
{
    if (!vp)
        return 0;

    if (vp->state == VP_STATE_IDLE || vp->state == VP_STATE_ERROR)
        return 0;

    /* Show control bar on any key */
    videoplayer_show_control_bar(vp);

    switch (sym) {
    /* Play/Pause */
    case XKB_KEY_space:
    case XKB_KEY_k:
    case XKB_KEY_K:
        videoplayer_toggle_pause(vp);
        return 1;

    /* Seek backward */
    case XKB_KEY_Left:
        videoplayer_seek_relative(vp, -SEEK_SMALL);
        return 1;

    case XKB_KEY_j:
    case XKB_KEY_J:
        videoplayer_seek_relative(vp, -SEEK_LARGE);
        return 1;

    /* Seek forward */
    case XKB_KEY_Right:
        videoplayer_seek_relative(vp, SEEK_SMALL);
        return 1;

    case XKB_KEY_l:
    case XKB_KEY_L:
        videoplayer_seek_relative(vp, SEEK_LARGE);
        return 1;

    /* Volume */
    case XKB_KEY_Up:
        videoplayer_adjust_volume(vp, VOLUME_STEP);
        return 1;

    case XKB_KEY_Down:
        videoplayer_adjust_volume(vp, -VOLUME_STEP);
        return 1;

    case XKB_KEY_m:
    case XKB_KEY_M:
        videoplayer_toggle_mute(vp);
        return 1;

    /* Subtitle track */
    case XKB_KEY_c:
    case XKB_KEY_C:
        videoplayer_cycle_subtitle_track(vp);
        return 1;

    /* Audio track */
    case XKB_KEY_a:
    case XKB_KEY_A:
        if (!(mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO))) {
            videoplayer_cycle_audio_track(vp);
            return 1;
        }
        break;

    /* Stop/Exit */
    case XKB_KEY_Escape:
    case XKB_KEY_q:
    case XKB_KEY_Q:
        videoplayer_stop(vp);
        return 1;

    /* Jump to percentage (0-9) */
    case XKB_KEY_0:
    case XKB_KEY_KP_0:
        videoplayer_seek(vp, 0);
        return 1;

    case XKB_KEY_1:
    case XKB_KEY_KP_1:
        videoplayer_seek(vp, vp->duration_us / 10);
        return 1;

    case XKB_KEY_2:
    case XKB_KEY_KP_2:
        videoplayer_seek(vp, vp->duration_us * 2 / 10);
        return 1;

    case XKB_KEY_3:
    case XKB_KEY_KP_3:
        videoplayer_seek(vp, vp->duration_us * 3 / 10);
        return 1;

    case XKB_KEY_4:
    case XKB_KEY_KP_4:
        videoplayer_seek(vp, vp->duration_us * 4 / 10);
        return 1;

    case XKB_KEY_5:
    case XKB_KEY_KP_5:
        videoplayer_seek(vp, vp->duration_us * 5 / 10);
        return 1;

    case XKB_KEY_6:
    case XKB_KEY_KP_6:
        videoplayer_seek(vp, vp->duration_us * 6 / 10);
        return 1;

    case XKB_KEY_7:
    case XKB_KEY_KP_7:
        videoplayer_seek(vp, vp->duration_us * 7 / 10);
        return 1;

    case XKB_KEY_8:
    case XKB_KEY_KP_8:
        videoplayer_seek(vp, vp->duration_us * 8 / 10);
        return 1;

    case XKB_KEY_9:
    case XKB_KEY_KP_9:
        videoplayer_seek(vp, vp->duration_us * 9 / 10);
        return 1;

    /* Home/End */
    case XKB_KEY_Home:
        videoplayer_seek(vp, 0);
        return 1;

    case XKB_KEY_End:
        if (vp->duration_us > 0)
            videoplayer_seek(vp, vp->duration_us - 5000000);  /* 5s before end */
        return 1;

    /* Page Up/Down - large seek */
    case XKB_KEY_Page_Up:
        videoplayer_seek_relative(vp, SEEK_HUGE);
        return 1;

    case XKB_KEY_Page_Down:
        videoplayer_seek_relative(vp, -SEEK_HUGE);
        return 1;

    default:
        break;
    }

    return 0;
}

int videoplayer_handle_motion(VideoPlayer *vp, int x, int y)
{
    VideoPlayerControlBar *bar;

    if (!vp)
        return 0;

    if (vp->state == VP_STATE_IDLE || vp->state == VP_STATE_ERROR)
        return 0;

    /* Show control bar on mouse movement */
    videoplayer_show_control_bar(vp);

    /* Check if mouse is over control bar */
    bar = &vp->control_bar;
    if (bar->visible) {
        if (x >= bar->x && x < bar->x + bar->width &&
            y >= bar->y && y < bar->y + bar->height) {
            bar->hovered = 1;
            return 1;
        } else {
            bar->hovered = 0;
        }
    }

    return 0;
}

int videoplayer_handle_button(VideoPlayer *vp, int x, int y, uint32_t button, int pressed)
{
    VideoPlayerControlBar *bar;
    int rx, ry;

    if (!vp)
        return 0;

    if (vp->state == VP_STATE_IDLE || vp->state == VP_STATE_ERROR)
        return 0;

    /* Only handle press events */
    if (!pressed)
        return 0;

    bar = &vp->control_bar;

    /* Check if click is on control bar */
    if (!bar->visible)
        return 0;

    if (x < bar->x || x >= bar->x + bar->width ||
        y < bar->y || y >= bar->y + bar->height)
        return 0;

    /* Relative coordinates within bar */
    rx = x - bar->x;
    ry = y - bar->y;
    (void)ry;  /* Not used currently */

    /* Play/Pause button */
    if (rx >= bar->play_btn_x && rx < bar->play_btn_x + bar->play_btn_w) {
        videoplayer_toggle_pause(vp);
        return 1;
    }

    /* Progress bar */
    if (rx >= bar->progress_x && rx < bar->progress_x + bar->progress_w) {
        float pos = (float)(rx - bar->progress_x) / bar->progress_w;
        int64_t seek_pos = (int64_t)(pos * vp->duration_us);
        videoplayer_seek(vp, seek_pos);
        return 1;
    }

    /* Volume button */
    if (rx >= bar->volume_x && rx < bar->volume_x + bar->volume_w) {
        videoplayer_toggle_mute(vp);
        return 1;
    }

    /* Audio track button */
    if (rx >= bar->audio_x && rx < bar->audio_x + bar->audio_w) {
        videoplayer_cycle_audio_track(vp);
        return 1;
    }

    /* Subtitle button */
    if (rx >= bar->subtitle_x && rx < bar->subtitle_x + bar->subtitle_w) {
        videoplayer_cycle_subtitle_track(vp);
        return 1;
    }

    return 1;  /* Consumed click */
}

/* ================================================================
 *  Stub implementations for modules not yet loaded
 * ================================================================ */

/* These will be replaced by actual implementations */

__attribute__((weak))
int videoplayer_audio_init(VideoPlayer *vp)
{
    (void)vp;
    return 0;
}

__attribute__((weak))
void videoplayer_audio_cleanup(VideoPlayer *vp)
{
    (void)vp;
}

__attribute__((weak))
void videoplayer_audio_play(VideoPlayer *vp)
{
    (void)vp;
}

__attribute__((weak))
void videoplayer_audio_pause(VideoPlayer *vp)
{
    (void)vp;
}

__attribute__((weak))
int videoplayer_subtitle_init(VideoPlayer *vp)
{
    (void)vp;
    return 0;
}

__attribute__((weak))
void videoplayer_subtitle_cleanup(VideoPlayer *vp)
{
    (void)vp;
}

__attribute__((weak))
void videoplayer_show_control_bar(VideoPlayer *vp)
{
    (void)vp;
}

__attribute__((weak))
void videoplayer_hide_control_bar(VideoPlayer *vp)
{
    (void)vp;
}

__attribute__((weak))
void videoplayer_cleanup_scene(VideoPlayer *vp)
{
    (void)vp;
}
