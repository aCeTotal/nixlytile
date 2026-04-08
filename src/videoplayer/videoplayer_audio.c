/*
 * Nixlytile Video Player - Audio Module
 * PipeWire audio output with 7.1 surround and Atmos passthrough
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include <time.h>
#include <stdarg.h>
#include <pthread.h>

#include "videoplayer.h"

/* Diagnostics log fds (defined in globals.c) */
extern int audio_log_fd;
extern int error_log_fd;

static void audio_diag(const char *fmt, ...)
{
    if (audio_log_fd < 0) return;
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03ld] ",
        tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec/1000000);
    va_list ap; va_start(ap, fmt);
    off += vsnprintf(buf+off, sizeof(buf)-off, fmt, ap);
    va_end(ap);
    if (off < (int)sizeof(buf)-1) buf[off++] = '\n';
    (void)!write(audio_log_fd, buf, off);
}

static void audio_diag_error(const char *fmt, ...)
{
    if (error_log_fd < 0) return;
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    char buf[1024];
    int off = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03ld] [AUDIO] ",
        tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec/1000000);
    va_list ap; va_start(ap, fmt);
    off += vsnprintf(buf+off, sizeof(buf)-off, fmt, ap);
    va_end(ap);
    if (off < (int)sizeof(buf)-1) buf[off++] = '\n';
    (void)!write(error_log_fd, buf, off);
}

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Audio buffer settings */
#define AUDIO_BUFFER_FRAMES 2048
#define AUDIO_LATENCY_MS 50

/* IEC61937 constants for TrueHD/Atmos passthrough */
#define IEC61937_HEADER_SIZE 8
#define IEC61937_TRUEHD_BURST_SIZE 61440  /* MAT frame size */

/* ================================================================
 *  Audio Command Pipe (non-blocking from compositor thread)
 * ================================================================ */

typedef enum {
    AUDIO_CMD_PLAY = 1,
    AUDIO_CMD_PAUSE,
    AUDIO_CMD_VOLUME,
    AUDIO_CMD_MUTE,
    AUDIO_CMD_RECREATE,
    AUDIO_CMD_QUIT
} AudioCmdType;

typedef struct {
    uint32_t type;              /* AudioCmdType */
    union {
        float volume;           /* for AUDIO_CMD_VOLUME */
        int32_t mute;           /* for AUDIO_CMD_MUTE */
    };
} AudioCmd;  /* 8 bytes — atomic write for pipe ≤ PIPE_BUF */

/* Forward declarations for cmd thread */
static void audio_cmd_send(VideoPlayer *vp, const AudioCmd *cmd);
static void *audio_cmd_thread_func(void *arg);
static void audio_cmd_apply_volume(VideoPlayer *vp, float volume);
static void audio_cmd_apply_mute(VideoPlayer *vp, int mute);

/* Forward declarations for recovery (used by cmd thread) */
void videoplayer_audio_play(VideoPlayer *vp);
int videoplayer_audio_init(VideoPlayer *vp);
void videoplayer_audio_cleanup(VideoPlayer *vp);

/* ================================================================
 *  Ring Buffer Implementation
 * ================================================================ */

static int ring_buffer_init(AudioRingBuffer *ring, size_t size)
{
    ring->data = calloc(1, size);
    if (!ring->data)
        return -1;

    ring->size = size;
    ring->read_pos = 0;
    ring->write_pos = 0;
    ring->available = 0;
    pthread_mutex_init(&ring->lock, NULL);

    return 0;
}

static void ring_buffer_free(AudioRingBuffer *ring)
{
    if (ring->data) {
        free(ring->data);
        ring->data = NULL;
    }
    pthread_mutex_destroy(&ring->lock);
}

static void ring_buffer_clear(AudioRingBuffer *ring)
{
    pthread_mutex_lock(&ring->lock);
    ring->read_pos = 0;
    ring->write_pos = 0;
    ring->available = 0;
    pthread_mutex_unlock(&ring->lock);
}

static size_t ring_buffer_write(AudioRingBuffer *ring, const uint8_t *data, size_t len)
{
    size_t written = 0;
    size_t space;
    size_t chunk;

    pthread_mutex_lock(&ring->lock);

    space = ring->size - ring->available;
    if (len > space)
        len = space;

    while (written < len) {
        chunk = ring->size - ring->write_pos;
        if (chunk > len - written)
            chunk = len - written;

        memcpy(ring->data + ring->write_pos, data + written, chunk);
        ring->write_pos = (ring->write_pos + chunk) % ring->size;
        written += chunk;
    }

    ring->available += written;
    pthread_mutex_unlock(&ring->lock);

    return written;
}

static size_t ring_buffer_read(AudioRingBuffer *ring, uint8_t *data, size_t len)
{
    size_t read_bytes = 0;
    size_t chunk;

    pthread_mutex_lock(&ring->lock);

    if (len > ring->available)
        len = ring->available;

    while (read_bytes < len) {
        chunk = ring->size - ring->read_pos;
        if (chunk > len - read_bytes)
            chunk = len - read_bytes;

        memcpy(data + read_bytes, ring->data + ring->read_pos, chunk);
        ring->read_pos = (ring->read_pos + chunk) % ring->size;
        read_bytes += chunk;
    }

    ring->available -= read_bytes;
    pthread_mutex_unlock(&ring->lock);

    return read_bytes;
}

static size_t ring_buffer_available(AudioRingBuffer *ring)
{
    return __atomic_load_n(&ring->available, __ATOMIC_ACQUIRE);
}

static size_t ring_buffer_available_space(AudioRingBuffer *ring)
{
    return ring->size - __atomic_load_n(&ring->available, __ATOMIC_ACQUIRE);
}

/* ================================================================
 *  PipeWire Stream Callbacks
 * ================================================================ */

static void on_process(void *userdata)
{
    VideoPlayer *vp = userdata;
    struct pw_buffer *b;
    struct spa_buffer *buf;
    float *dst;
    uint32_t n_frames;
    uint32_t stride;
    size_t bytes_needed;
    size_t bytes_read;

    if (!vp || !vp->audio.stream)
        return;

    b = pw_stream_dequeue_buffer(vp->audio.stream);
    if (!b) {
        return;
    }

    buf = b->buffer;
    dst = buf->datas[0].data;
    if (!dst) {
        pw_stream_queue_buffer(vp->audio.stream, b);
        return;
    }

    stride = sizeof(float) * vp->audio.channels;
    n_frames = buf->datas[0].maxsize / stride;

    if (b->requested)
        n_frames = SPA_MIN(n_frames, b->requested);

    bytes_needed = n_frames * stride;

    /* Read audio data from ring buffer */
    bytes_read = ring_buffer_read(&vp->audio.ring, (uint8_t *)dst, bytes_needed);

    /* Fill remaining with silence if not enough data */
    if (bytes_read < bytes_needed) {
        memset((uint8_t *)dst + bytes_read, 0, bytes_needed - bytes_read);
        vp->debug_audio_underruns++;
        audio_diag("underrun: got %zu/%zu bytes", bytes_read, bytes_needed);
    }

    /* Apply volume and mute.
     * Volume is applied ONLY here in software — PipeWire channelVolumes are
     * NOT used (removed to prevent double application that caused crackling
     * through HDMI and extreme over-attenuation at non-unity volumes). */
    if (vp->audio.muted || vp->muted) {
        memset(dst, 0, bytes_needed);
    } else {
        float vol = vp->volume;
        if (vol < 0.999f) {
            float *samples = dst;
            uint32_t total_samples = n_frames * vp->audio.channels;
            for (uint32_t i = 0; i < total_samples; i++) {
                samples[i] *= vol;
            }
        }
    }

    /* Update audio clock based on samples actually played.
     * Use audio_played_samples directly — it's incremented here under
     * audio.lock, so it's race-free. The previous approach (written -
     * ring_buffer_available) had a TOCTOU race: the decode thread could
     * increment audio_written_samples between the ring_buffer_available
     * read and the audio.lock acquisition, inflating the clock and
     * causing periodic A/V sync jumps.
     *
     * IMPORTANT: Minimize lock hold time in this RT callback — long
     * critical sections cause priority inversion with the decode thread,
     * leading to xruns → audible crackling on HDMI.  Pre-compute
     * everything outside the lock, then write under lock briefly. */
    if (bytes_read > 0) {
        uint32_t frames_played = bytes_read / stride;
        uint64_t now_ns = get_time_ns();

        /* Capture PipeWire output pipeline delay OUTSIDE the lock.
         * pw_stream_get_time is lock-free in PipeWire's RT context. */
        int64_t delay_us = 0;
        struct pw_time pwt;
        if (pw_stream_get_time(vp->audio.stream, &pwt) == 0 &&
            pwt.rate.denom > 0) {
            /* Pipeline delay (graph latency to DAC) */
            delay_us += (int64_t)pwt.delay * 1000000LL *
                        pwt.rate.num / pwt.rate.denom;
            /* Queued data in PipeWire buffers (bytes → frames → us) */
            uint32_t frame_size = sizeof(float) * vp->audio.channels;
            if (frame_size > 0 && vp->audio.sample_rate > 0) {
                delay_us += ((int64_t)pwt.queued / frame_size) *
                            1000000LL / vp->audio.sample_rate;
                delay_us += (int64_t)pwt.buffered * 1000000LL /
                            vp->audio.sample_rate;
            }
        }

        /* Brief lock: only counter increment + store precomputed values */
        pthread_mutex_lock(&vp->audio.lock);
        vp->audio.audio_played_samples += frames_played;

        int64_t samples_to_us = ((int64_t)vp->audio.audio_played_samples * 1000000LL) / vp->audio.sample_rate;
        vp->audio.audio_pts_us = vp->audio.base_pts_us + samples_to_us;

        vp->audio.clock_snapshot_us = vp->audio.audio_pts_us;
        vp->audio.clock_update_ns = now_ns;
        vp->audio.output_delay_us = delay_us;
        pthread_mutex_unlock(&vp->audio.lock);
    }

    /* Signal synchronized A/V start: PipeWire is actually consuming audio.
     * The render thread waits for this before presenting the first video frame,
     * ensuring audio hardware is outputting samples when video begins. */
    if (bytes_read > 0 && !vp->audio.audio_preroll_done) {
        vp->audio.audio_preroll_done = 1;
        audio_diag("preroll complete");
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = stride;
    buf->datas[0].chunk->size = bytes_needed;

    pw_stream_queue_buffer(vp->audio.stream, b);
}

static void on_stream_state_changed(void *userdata, enum pw_stream_state old,
                                     enum pw_stream_state state, const char *error)
{
    VideoPlayer *vp = userdata;

    fprintf(stderr, "[audio] Stream state: %s -> %s",
            pw_stream_state_as_string(old),
            pw_stream_state_as_string(state));
    if (error)
        fprintf(stderr, " (error: %s)", error);
    fprintf(stderr, "\n");

    audio_diag("state: %s -> %s%s%s",
        pw_stream_state_as_string(old),
        pw_stream_state_as_string(state),
        error ? " (error: " : "",
        error ? error : "");

    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "[audio] Stream error: %s\n", error ? error : "unknown");
        audio_diag_error("Stream error: %s", error ? error : "unknown");
        /* Mark as interrupted so video continues with timing-based playback */
        vp->audio.stream_interrupted = 1;
    }

    /* When stream is interrupted (e.g., Bluetooth/controller reconnection),
     * flush the ring buffer and reset sync state.
     * IMPORTANT: Set interrupted flag BEFORE taking lock to prevent render thread
     * from using stale A/V sync during the transition.
     *
     * EXCEPTION: User-initiated pause (pw_stream_set_active(false)) also causes
     * STREAMING→PAUSED but should NOT be treated as an interruption — the ring
     * buffer and sync state must be preserved for seamless resume. */
    if (old == PW_STREAM_STATE_STREAMING &&
        (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_UNCONNECTED ||
         state == PW_STREAM_STATE_ERROR)) {

        if (state == PW_STREAM_STATE_PAUSED && vp->audio.user_paused) {
            fprintf(stderr, "[audio] User-initiated pause, keeping audio state\n");
        } else {
            fprintf(stderr, "[audio] Stream interrupted, flushing buffer - video will continue with timing-based playback\n");
            audio_diag("interrupted, flushing ring buffer");
            audio_diag_error("Stream interrupted: %s",
                state == PW_STREAM_STATE_ERROR ? "error" :
                state == PW_STREAM_STATE_UNCONNECTED ? "disconnected" : "device change");

            /* Signal render thread to use timing-based playback immediately */
            vp->audio.stream_interrupted = 1;
            /* Tell render thread to reset frame timing so video never stalls */
            vp->audio.needs_timing_reset = 1;
            /* Reset reactivation counter for staged recovery */
            vp->audio.reactivate_attempts = 0;

            ring_buffer_clear(&vp->audio.ring);

            /* Reset sync tracking */
            pthread_mutex_lock(&vp->audio.lock);
            vp->audio.audio_pts_us = 0;
            vp->audio.base_pts_us = 0;
            vp->audio.audio_written_samples = 0;
            vp->audio.audio_played_samples = 0;
            vp->audio.clock_update_ns = 0;
            vp->audio.clock_snapshot_us = 0;
            vp->audio.output_delay_us = 0;
            vp->audio.last_returned_clock_us = 0;
            vp->audio.last_audio_pts = 0;
            vp->audio.stall_count = 0;
            vp->audio.recovery_frames = 15;
            pthread_mutex_unlock(&vp->audio.lock);
        }
    }

    /* NOTE: We intentionally do NOT call pw_stream_set_active(true) when entering
     * PAUSED state. During Bluetooth device reconnection, PipeWire may rapidly
     * cycle between STREAMING and PAUSED as it reconfigures audio routing.
     * Immediately reactivating causes STREAMING<->PAUSED oscillation that
     * destabilizes PipeWire. Instead, the recovery mechanism in
     * videoplayer_audio_check_recovery() handles reconnection after PipeWire
     * has fully stabilized (with staged reactivation → recreate). */

    /* Handle UNCONNECTED state - stream was disconnected, need to try reconnecting */
    if (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_CONNECTING) {
        fprintf(stderr, "[audio] Stream unconnected - will attempt reconnection on next audio frame\n");
        /* Keep stream_interrupted = 1 - video continues independently */
    }

    /* When stream resumes streaming after interruption, reset for clean sync.
     * For user-initiated resume (after user_paused), ring buffer and sync
     * state are still valid — just resume playback seamlessly. */
    if (state == PW_STREAM_STATE_STREAMING &&
        (old == PW_STREAM_STATE_PAUSED || old == PW_STREAM_STATE_CONNECTING)) {

        vp->audio.write_stall_count = 0;
        vp->audio.reactivate_attempts = 0;

        if (vp->audio.stream_interrupted) {
            /* Recovery from device interruption - full reset needed */
            fprintf(stderr, "[audio] Stream resumed after interruption, video will resync gradually\n");
            audio_diag("state: resumed after interruption, resyncing");
            vp->audio.stream_interrupted = 0;
            vp->audio.needs_timing_reset = 1;

            ring_buffer_clear(&vp->audio.ring);

            pthread_mutex_lock(&vp->audio.lock);
            vp->audio.audio_pts_us = 0;
            vp->audio.base_pts_us = 0;
            vp->audio.audio_written_samples = 0;
            vp->audio.audio_played_samples = 0;
            vp->audio.clock_update_ns = 0;
            vp->audio.clock_snapshot_us = 0;
            vp->audio.output_delay_us = 0;
            vp->audio.last_returned_clock_us = 0;
            vp->audio.recovery_frames = 15;
            pthread_mutex_unlock(&vp->audio.lock);
        } else {
            /* User-initiated resume - audio state is intact, just continue */
            fprintf(stderr, "[audio] Stream resumed (user unpause)\n");
        }
    }
}

static void on_param_changed(void *userdata, uint32_t id, const struct spa_pod *param)
{
    VideoPlayer *vp = userdata;

    if (!vp || !param)
        return;

    if (id == SPA_PARAM_Format) {
        /* Format renegotiation. During initial setup this is normal.
         * During active playback, this indicates a device change
         * (e.g., Bluetooth connection causing PipeWire to move the stream). */
        if (vp->state == VP_STATE_PLAYING && !vp->audio.user_paused &&
            !vp->audio.stream_interrupted) {
            fprintf(stderr, "[audio] Format change during playback - device change detected\n");
            audio_diag("format renegotiation during playback (device change)");
            vp->audio.stream_interrupted = 1;
            vp->audio.needs_timing_reset = 1;
        }
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

/* ================================================================
 *  Audio Command Pipe Implementation
 * ================================================================ */

static void audio_cmd_send(VideoPlayer *vp, const AudioCmd *cmd)
{
    if (vp->audio.cmd_pipe[1] < 0)
        return;

    /* write() of ≤ PIPE_BUF bytes is atomic and non-blocking (pipe is O_NONBLOCK) */
    ssize_t n = write(vp->audio.cmd_pipe[1], cmd, sizeof(*cmd));
    if (n < 0 && errno != EAGAIN)
        fprintf(stderr, "[audio] cmd pipe write error: %s\n", strerror(errno));
}

static void *audio_cmd_thread_func(void *arg)
{
    VideoPlayer *vp = arg;
    AudioCmd cmd;
    ssize_t n;

    fprintf(stderr, "[audio-cmd] Command thread started\n");

    while (1) {
        /* Blocking read on pipe — wakes when compositor sends a command */
        n = read(vp->audio.cmd_pipe[0], &cmd, sizeof(cmd));
        if (n != sizeof(cmd)) {
            if (n == 0 || (n < 0 && errno != EINTR)) {
                fprintf(stderr, "[audio-cmd] Pipe closed or error, exiting\n");
                break;
            }
            continue;  /* EINTR — retry */
        }

        switch (cmd.type) {
        case AUDIO_CMD_PLAY:
            if (vp->audio.stream && vp->audio.loop) {
                pw_thread_loop_lock(vp->audio.loop);
                pw_stream_set_active(vp->audio.stream, true);
                pw_thread_loop_unlock(vp->audio.loop);
            }
            break;

        case AUDIO_CMD_PAUSE:
            if (vp->audio.stream && vp->audio.loop && !vp->audio.stream_interrupted) {
                pw_thread_loop_lock(vp->audio.loop);
                pw_stream_set_active(vp->audio.stream, false);
                pw_thread_loop_unlock(vp->audio.loop);
            }
            break;

        case AUDIO_CMD_VOLUME:
            /* Volume is now applied exclusively in software mixing (on_process).
             * PipeWire channelVolumes are no longer used — prevents double
             * application that caused HDMI crackling. */
            break;

        case AUDIO_CMD_MUTE:
            /* Mute is now applied exclusively in software mixing (on_process). */
            break;

        case AUDIO_CMD_RECREATE:
            fprintf(stderr, "[audio-cmd] Recreating audio stream\n");
            audio_diag("recreating audio stream");
            videoplayer_audio_cleanup(vp);
            if (videoplayer_audio_init(vp) == 0) {
                fprintf(stderr, "[audio-cmd] Stream recreated successfully\n");
                audio_diag("stream recreated successfully");
                if (vp->state == VP_STATE_PLAYING) {
                    /* Directly activate — we're already on the cmd thread */
                    if (vp->audio.stream && vp->audio.loop) {
                        pw_thread_loop_lock(vp->audio.loop);
                        pw_stream_set_active(vp->audio.stream, true);
                        pw_thread_loop_unlock(vp->audio.loop);
                    }
                }
            } else {
                fprintf(stderr, "[audio-cmd] Stream recreation failed\n");
                audio_diag_error("Stream recreation failed");
            }
            vp->audio.audio_recreating = 0;
            break;

        case AUDIO_CMD_QUIT:
            fprintf(stderr, "[audio-cmd] Command thread exiting\n");
            goto done;

        default:
            fprintf(stderr, "[audio-cmd] Unknown command %u\n", cmd.type);
            break;
        }
    }

done:
    vp->audio.cmd_thread_running = 0;
    return NULL;
}

static int audio_cmd_pipe_init(VideoPlayer *vp)
{
    /* Guard: don't re-create during RECREATE (cmd thread is still running) */
    if (vp->audio.cmd_thread_running)
        return 0;

    vp->audio.cmd_pipe[0] = -1;
    vp->audio.cmd_pipe[1] = -1;

    if (pipe(vp->audio.cmd_pipe) < 0) {
        fprintf(stderr, "[audio] Failed to create cmd pipe: %s\n", strerror(errno));
        return -1;
    }

    /* Write end non-blocking (compositor thread must never block) */
    int flags = fcntl(vp->audio.cmd_pipe[1], F_GETFL);
    fcntl(vp->audio.cmd_pipe[1], F_SETFL, flags | O_NONBLOCK);
    /* Read end stays blocking (cmd thread blocks on it) */

    vp->audio.cmd_thread_running = 1;
    if (pthread_create(&vp->audio.cmd_thread, NULL, audio_cmd_thread_func, vp) != 0) {
        fprintf(stderr, "[audio] Failed to create cmd thread: %s\n", strerror(errno));
        close(vp->audio.cmd_pipe[0]);
        close(vp->audio.cmd_pipe[1]);
        vp->audio.cmd_pipe[0] = -1;
        vp->audio.cmd_pipe[1] = -1;
        vp->audio.cmd_thread_running = 0;
        return -1;
    }
    pthread_setname_np(vp->audio.cmd_thread, "nl-audio-cmd");

    fprintf(stderr, "[audio] Command pipe and thread initialized\n");
    return 0;
}

void videoplayer_audio_cmd_shutdown(VideoPlayer *vp)
{
    if (!vp)
        return;

    if (vp->audio.cmd_thread_running) {
        /* Send QUIT command.  If the pipe write fails (e.g., full pipe),
         * closing the write end below will cause the read() in the cmd
         * thread to return 0/error, breaking it out of its blocking wait. */
        AudioCmd cmd = { .type = AUDIO_CMD_QUIT };
        audio_cmd_send(vp, &cmd);

        /* Close write end BEFORE join — if the QUIT command didn't
         * make it through (pipe full, race), this ensures the cmd
         * thread's blocking read() returns with error/EOF. */
        if (vp->audio.cmd_pipe[1] >= 0) {
            close(vp->audio.cmd_pipe[1]);
            vp->audio.cmd_pipe[1] = -1;
        }

        pthread_join(vp->audio.cmd_thread, NULL);
        vp->audio.cmd_thread_running = 0;
    }

    if (vp->audio.cmd_pipe[0] >= 0) {
        close(vp->audio.cmd_pipe[0]);
        vp->audio.cmd_pipe[0] = -1;
    }
    if (vp->audio.cmd_pipe[1] >= 0) {
        close(vp->audio.cmd_pipe[1]);
        vp->audio.cmd_pipe[1] = -1;
    }
}

/* ================================================================
 *  Audio Initialization
 * ================================================================ */

int videoplayer_audio_init(VideoPlayer *vp)
{
    struct pw_properties *props;
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    AudioTrack *track;
    int channels;
    int sample_rate;

    if (!vp)
        return -1;

    if (vp->audio_track_count == 0) {
        fprintf(stderr, "[audio] No audio tracks available\n");
        return -1;
    }

    track = &vp->audio_tracks[vp->current_audio_track];
    channels = track->channels;
    sample_rate = track->sample_rate;

    /* Clamp channels to 8 (7.1) */
    if (channels > 8)
        channels = 8;
    if (channels < 1)
        channels = 2;

    vp->audio.channels = channels;
    vp->audio.sample_rate = sample_rate;
    vp->audio.volume = 1.0f;
    vp->audio.muted = 0;
    vp->audio.base_pts_us = 0;
    vp->audio.audio_played_samples = 0;
    vp->audio.clock_update_ns = 0;
    vp->audio.clock_snapshot_us = 0;
    vp->audio.output_delay_us = 0;
    vp->audio.last_returned_clock_us = 0;
    vp->audio.last_audio_pts = 0;
    vp->audio.stall_count = 0;
    vp->audio.recovery_frames = 0;
    vp->audio.stream_interrupted = 0;
    vp->audio.audio_recreating = 0;
    vp->audio.needs_timing_reset = 0;
    vp->audio.user_paused = 0;
    vp->audio.needs_device_pause = 0;
    vp->audio.write_stall_count = 0;
    vp->audio.reactivate_attempts = 0;
    vp->audio.audio_preroll_done = 0;
    vp->audio.audio_prestart_sent = 0;
    vp->audio.audio_prestart_ns = 0;

    /* Initialize ring buffer */
    if (ring_buffer_init(&vp->audio.ring, AUDIO_RING_BUFFER_SIZE) < 0) {
        fprintf(stderr, "[audio] Failed to allocate ring buffer\n");
        return -1;
    }

    /* Initialize PipeWire */
    pw_init(NULL, NULL);

    /* Create thread loop */
    vp->audio.loop = pw_thread_loop_new("nixlytile-audio", NULL);
    if (!vp->audio.loop) {
        fprintf(stderr, "[audio] Failed to create thread loop\n");
        ring_buffer_free(&vp->audio.ring);
        pw_deinit();
        return -1;
    }

    /* Create context */
    vp->audio.context = pw_context_new(pw_thread_loop_get_loop(vp->audio.loop),
                                        NULL, 0);
    if (!vp->audio.context) {
        fprintf(stderr, "[audio] Failed to create context\n");
        pw_thread_loop_destroy(vp->audio.loop);
        vp->audio.loop = NULL;
        ring_buffer_free(&vp->audio.ring);
        pw_deinit();
        return -1;
    }

    /* Start thread loop */
    if (pw_thread_loop_start(vp->audio.loop) < 0) {
        fprintf(stderr, "[audio] Failed to start thread loop\n");
        pw_context_destroy(vp->audio.context);
        pw_thread_loop_destroy(vp->audio.loop);
        vp->audio.context = NULL;
        vp->audio.loop = NULL;
        ring_buffer_free(&vp->audio.ring);
        pw_deinit();
        return -1;
    }

    pw_thread_loop_lock(vp->audio.loop);

    /* Connect to PipeWire server */
    vp->audio.core = pw_context_connect(vp->audio.context, NULL, 0);
    if (!vp->audio.core) {
        fprintf(stderr, "[audio] Failed to connect to PipeWire\n");
        audio_diag_error("Failed to connect to PipeWire");
        pw_thread_loop_unlock(vp->audio.loop);
        pw_thread_loop_stop(vp->audio.loop);
        pw_context_destroy(vp->audio.context);
        pw_thread_loop_destroy(vp->audio.loop);
        vp->audio.context = NULL;
        vp->audio.loop = NULL;
        ring_buffer_free(&vp->audio.ring);
        pw_deinit();
        return -1;
    }

    /* Create stream properties
     * Allow PipeWire to move our stream when devices change (e.g., Bluetooth connect).
     * The on_stream_state_changed callback handles interruptions gracefully.
     *
     * NODE_LATENCY: Request a comfortable buffer size for HDMI outputs.
     * HDMI audio typically requires larger buffers than analog — without this
     * hint PipeWire may use a tiny quantum (256-512 frames) that the HDMI
     * controller can't service in time, causing xruns → audible crackling.
     * 2048/48000 ≈ 42.7ms — low enough for acceptable lip-sync, high enough
     * to prevent HDMI xruns.  PipeWire may negotiate a different size, this
     * is just a preference hint. */
    props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Movie",
        PW_KEY_APP_NAME, "nixlytile",
        PW_KEY_NODE_NAME, "nixlytile-player",
        PW_KEY_NODE_DESCRIPTION, "Nixlytile Video Player",
        PW_KEY_NODE_LATENCY, "2048/48000",
        NULL);

    /* Create stream */
    vp->audio.stream = pw_stream_new(vp->audio.core, "nixlytile-audio-stream", props);
    if (!vp->audio.stream) {
        fprintf(stderr, "[audio] Failed to create stream\n");
        audio_diag_error("Failed to create PipeWire stream");
        pw_core_disconnect(vp->audio.core);
        pw_thread_loop_unlock(vp->audio.loop);
        pw_thread_loop_stop(vp->audio.loop);
        pw_context_destroy(vp->audio.context);
        pw_thread_loop_destroy(vp->audio.loop);
        vp->audio.core = NULL;
        vp->audio.context = NULL;
        vp->audio.loop = NULL;
        ring_buffer_free(&vp->audio.ring);
        pw_deinit();
        return -1;
    }

    pw_stream_add_listener(vp->audio.stream, &vp->audio.stream_listener,
                           &stream_events, vp);

    /* Build audio format parameters */
    struct spa_audio_info_raw audio_info = {
        .format = SPA_AUDIO_FORMAT_F32,
        .rate = sample_rate,
        .channels = channels,
    };

    /* Set channel positions for surround sound */
    switch (channels) {
    case 8:  /* 7.1 */
        audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
        audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;
        audio_info.position[2] = SPA_AUDIO_CHANNEL_FC;
        audio_info.position[3] = SPA_AUDIO_CHANNEL_LFE;
        audio_info.position[4] = SPA_AUDIO_CHANNEL_RL;
        audio_info.position[5] = SPA_AUDIO_CHANNEL_RR;
        audio_info.position[6] = SPA_AUDIO_CHANNEL_SL;
        audio_info.position[7] = SPA_AUDIO_CHANNEL_SR;
        break;

    case 6:  /* 5.1 */
        audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
        audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;
        audio_info.position[2] = SPA_AUDIO_CHANNEL_FC;
        audio_info.position[3] = SPA_AUDIO_CHANNEL_LFE;
        audio_info.position[4] = SPA_AUDIO_CHANNEL_RL;
        audio_info.position[5] = SPA_AUDIO_CHANNEL_RR;
        break;

    case 2:  /* Stereo */
    default:
        audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
        audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;
        break;
    }

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info);

    /* Connect stream */
    if (pw_stream_connect(vp->audio.stream,
                          PW_DIRECTION_OUTPUT,
                          PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS,
                          params, 1) < 0) {
        fprintf(stderr, "[audio] Failed to connect stream\n");
        audio_diag_error("Failed to connect PipeWire stream");
        pw_stream_destroy(vp->audio.stream);
        pw_core_disconnect(vp->audio.core);
        pw_thread_loop_unlock(vp->audio.loop);
        pw_thread_loop_stop(vp->audio.loop);
        pw_context_destroy(vp->audio.context);
        pw_thread_loop_destroy(vp->audio.loop);
        vp->audio.stream = NULL;
        vp->audio.core = NULL;
        vp->audio.context = NULL;
        vp->audio.loop = NULL;
        ring_buffer_free(&vp->audio.ring);
        pw_deinit();
        return -1;
    }

    /* Start stream inactive — audio must not play until videoplayer_play()
     * sends AUDIO_CMD_PLAY.  Without this, PipeWire starts consuming the
     * ring buffer immediately while video is still in PAUSED state, causing
     * the audio clock to run ahead of video and creating A/V desync. */
    pw_stream_set_active(vp->audio.stream, false);

    pw_thread_loop_unlock(vp->audio.loop);

    fprintf(stderr, "[audio] Initialized: %d channels @ %d Hz\n", channels, sample_rate);
    audio_diag("init: %d ch @ %d Hz (ring=%zu)", channels, sample_rate, vp->audio.ring.size);

    /* Start command pipe + thread (skips if already running during RECREATE) */
    if (audio_cmd_pipe_init(vp) < 0) {
        fprintf(stderr, "[audio] Warning: cmd pipe init failed, falling back to direct calls\n");
    }

    return 0;
}

void videoplayer_audio_cleanup(VideoPlayer *vp)
{
    if (!vp)
        return;

    audio_diag("cleanup");

    int had_pipewire = (vp->audio.loop != NULL);

    if (vp->audio.loop) {
        /* CRITICAL: Stop the PipeWire thread loop FIRST.  This guarantees
         * that on_process() and all other PipeWire callbacks have finished
         * and will never fire again.  The previous order (lock → destroy →
         * unlock → stop) had a race: on_process could fire between unlock
         * and stop, accessing the already-destroyed stream → crash.
         *
         * pw_thread_loop_stop() blocks until the PipeWire thread exits,
         * so after this returns, no callbacks can be running. */
        pw_thread_loop_stop(vp->audio.loop);

        /* Now safe to destroy — no callbacks can fire */
        if (vp->audio.stream) {
            spa_hook_remove(&vp->audio.stream_listener);
            pw_stream_destroy(vp->audio.stream);
            vp->audio.stream = NULL;
        }

        if (vp->audio.core) {
            pw_core_disconnect(vp->audio.core);
            vp->audio.core = NULL;
        }

        if (vp->audio.context) {
            pw_context_destroy(vp->audio.context);
            vp->audio.context = NULL;
        }

        pw_thread_loop_destroy(vp->audio.loop);
        vp->audio.loop = NULL;
    }

    /* Free ring buffer */
    ring_buffer_free(&vp->audio.ring);

    /* Free resampler */
    if (vp->audio.swr_ctx) {
        swr_free(&vp->audio.swr_ctx);
    }

    if (vp->audio.resample_buffer) {
        av_freep(&vp->audio.resample_buffer);
        vp->audio.resample_buffer_size = 0;
    }

    /* Free IEC61937 buffer */
    if (vp->audio.iec_buffer) {
        free(vp->audio.iec_buffer);
        vp->audio.iec_buffer = NULL;
        vp->audio.iec_buffer_size = 0;
    }

    /* Balance pw_init() from videoplayer_audio_init().
     * Only call pw_deinit() if PipeWire was actually initialized —
     * calling pw_deinit() without a matching pw_init() corrupts the
     * internal refcount and can prevent future PipeWire connections. */
    if (had_pipewire)
        pw_deinit();

    fprintf(stderr, "[audio] Cleanup complete\n");
}

/* ================================================================
 *  Stream Recovery (for Bluetooth/device reconnection)
 * ================================================================ */

/*
 * Check if audio stream needs recovery and attempt to restore it.
 * Called periodically from the render thread (~every 0.5s).
 * Returns: 0 = no action needed, 1 = recovery in progress, -1 = recovery failed
 *
 * Staged recovery:
 *   1s: Try pw_stream_set_active(true) — handles simple PipeWire pause/resume
 *   2s: Full RECREATE — handles stream destruction / device change
 *
 * IMPORTANT: This function is non-blocking — it sends a command via pipe
 * to the audio command thread which handles the actual PipeWire recreation.
 */
int videoplayer_audio_check_recovery(VideoPlayer *vp)
{
    if (!vp)
        return 0;

    /* If recreation is already in progress, just wait */
    if (vp->audio.audio_recreating)
        return 1;

    /* If no stream exists at all, nothing to recover */
    if (!vp->audio.stream || !vp->audio.loop)
        return 0;

    /* Just check the volatile flag - no locking required. */
    if (vp->audio.stream_interrupted) {
        vp->audio_interrupted_ticks++;

        /* Stage 1: After ~1 second, try reactivating the existing stream.
         * This handles the common case where PipeWire paused the stream
         * during BT device routing and just needs it set active again. */
        if (vp->audio_interrupted_ticks == 2 && vp->audio.reactivate_attempts < 2) {
            fprintf(stderr, "[audio] Trying stream reactivation (attempt %d)\n",
                    vp->audio.reactivate_attempts + 1);
            audio_diag("recovery: reactivation attempt %d", vp->audio.reactivate_attempts + 1);
            vp->audio.reactivate_attempts++;

            AudioCmd cmd = { .type = AUDIO_CMD_PLAY };
            audio_cmd_send(vp, &cmd);
        }

        /* Stage 2: After ~2 seconds, do a full RECREATE.
         * The stream is probably dead — destroy and rebuild. */
        if (vp->audio_interrupted_ticks >= 4) {
            fprintf(stderr, "[audio] Stream interrupted for %d ticks, sending RECREATE command\n",
                    vp->audio_interrupted_ticks);
            audio_diag("recovery: RECREATE after %d ticks", vp->audio_interrupted_ticks);
            vp->audio_interrupted_ticks = 0;

            if (!vp->audio.audio_recreating) {
                vp->audio.audio_recreating = 1;
                AudioCmd cmd = { .type = AUDIO_CMD_RECREATE };
                audio_cmd_send(vp, &cmd);
            }
        }

        return 1;  /* Recovery in progress */
    }

    /* Stream is healthy, reset the counter */
    vp->audio_interrupted_ticks = 0;
    return 0;
}

/* ================================================================
 *  Playback Control
 * ================================================================ */

void videoplayer_audio_play(VideoPlayer *vp)
{
    if (!vp || !vp->audio.stream)
        return;

    if (vp->audio.stream_interrupted) {
        fprintf(stderr, "[audio] Play skipped - stream interrupted\n");
        return;
    }

    /* Clear user_paused before resuming so on_stream_state_changed
     * knows this is a normal resume from user pause */
    vp->audio.user_paused = 0;

    AudioCmd cmd = { .type = AUDIO_CMD_PLAY };
    audio_cmd_send(vp, &cmd);

    fprintf(stderr, "[audio] Play (queued)\n");
}

void videoplayer_audio_pause(VideoPlayer *vp)
{
    if (!vp || !vp->audio.stream)
        return;

    if (vp->audio.stream_interrupted) {
        fprintf(stderr, "[audio] Pause skipped - stream interrupted\n");
        return;
    }

    /* Mark as user-initiated pause so on_stream_state_changed doesn't
     * treat STREAMING→PAUSED as a device interruption */
    vp->audio.user_paused = 1;

    AudioCmd cmd = { .type = AUDIO_CMD_PAUSE };
    audio_cmd_send(vp, &cmd);

    fprintf(stderr, "[audio] Pause (queued)\n");
}

/* ================================================================
 *  Audio Frame Queue
 * ================================================================ */

int videoplayer_audio_queue_frame(VideoPlayer *vp, AVFrame *frame)
{
    int ret;
    int out_samples;
    int out_size;
    int dst_nb_samples;
    uint8_t *out_buffer;

    if (!vp || !frame)
        return -1;

    /* If audio stream is not available or interrupted, silently drop audio frames.
     * Video playback continues independently with timing-based presentation. */
    if (!vp->audio.stream)
        return 0;  /* Return success to avoid blocking decode thread */

    /* If stream is interrupted (e.g., Bluetooth controller reconnect), drop audio
     * rather than queuing to a stale buffer. Video continues smoothly. */
    if (vp->audio.stream_interrupted)
        return 0;

    /* After seeking, discard audio frames from well before the seek target.
     * av_seek_frame(BACKWARD) positions the file to the cluster containing
     * the video keyframe, but that cluster may also contain audio packets
     * from seconds earlier.  Without this filter, those early audio packets
     * fill the ring buffer with content that doesn't match the visible video,
     * causing audible A/V desync (user hears wrong scene's audio).
     * Allow 500ms before target so audio roughly matches the keyframe. */
    if (vp->seek_target_us > 0 && !vp->audio_seek_trim_done &&
        frame->pts != AV_NOPTS_VALUE) {
        AVRational tb = vp->fmt_ctx->streams[
            vp->audio_tracks[vp->current_audio_track].stream_index]->time_base;
        int64_t pts = av_rescale_q(frame->pts, tb, AV_TIME_BASE_Q);
        if (pts < vp->seek_target_us - 500000) {
            return 0;  /* Silently discard pre-keyframe audio */
        }
    }

    /* During user-initiated pause, PipeWire isn't consuming the ring buffer,
     * so writes would fill it up and trigger the stall detector.  Drop audio.
     * But during initial buffering (open → play transition, user_paused=0),
     * we MUST keep audio in the ring buffer so it's ready when play() starts.
     * Without this, the first ~667ms of audio is lost, creating massive A/V
     * desync that causes aggressive frame-skipping on playback start. */
    if (vp->state == VP_STATE_PAUSED && vp->audio.user_paused)
        return 0;

    /* Initialize or update resampler if needed */
    if (!vp->audio.swr_ctx) {
        AVChannelLayout out_layout;

        /* Set output channel layout based on channel count */
        switch (vp->audio.channels) {
        case 8:
            av_channel_layout_copy(&out_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1);
            break;
        case 6:
            av_channel_layout_copy(&out_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1);
            break;
        case 1:
            av_channel_layout_copy(&out_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_MONO);
            break;
        default:
            av_channel_layout_copy(&out_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
            break;
        }

        ret = swr_alloc_set_opts2(&vp->audio.swr_ctx,
                                   &out_layout,              /* out_ch_layout */
                                   AV_SAMPLE_FMT_FLT,        /* out_sample_fmt (float for PipeWire) */
                                   vp->audio.sample_rate,    /* out_sample_rate */
                                   &frame->ch_layout,        /* in_ch_layout */
                                   frame->format,            /* in_sample_fmt */
                                   frame->sample_rate,       /* in_sample_rate */
                                   0, NULL);

        av_channel_layout_uninit(&out_layout);

        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[audio] Failed to allocate resampler: %s\n", errbuf);
            audio_diag_error("Failed to allocate resampler: %s", errbuf);
            return -1;
        }

        ret = swr_init(vp->audio.swr_ctx);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[audio] Failed to init resampler: %s\n", errbuf);
            audio_diag_error("Failed to init resampler: %s", errbuf);
            swr_free(&vp->audio.swr_ctx);
            return -1;
        }

        fprintf(stderr, "[audio] Resampler initialized: %d ch @ %d Hz -> %d ch @ %d Hz\n",
                frame->ch_layout.nb_channels, frame->sample_rate,
                vp->audio.channels, vp->audio.sample_rate);
    }

    /* Calculate output sample count */
    dst_nb_samples = av_rescale_rnd(swr_get_delay(vp->audio.swr_ctx, frame->sample_rate) +
                                     frame->nb_samples,
                                     vp->audio.sample_rate, frame->sample_rate,
                                     AV_ROUND_UP);

    /* Allocate/resize output buffer if needed */
    out_size = dst_nb_samples * vp->audio.channels * sizeof(float);
    if (out_size > vp->audio.resample_buffer_size) {
        av_freep(&vp->audio.resample_buffer);
        vp->audio.resample_buffer = av_malloc(out_size);
        if (!vp->audio.resample_buffer) {
            vp->audio.resample_buffer_size = 0;
            return -1;
        }
        vp->audio.resample_buffer_size = out_size;
    }

    out_buffer = vp->audio.resample_buffer;

    /* Check ring buffer space BEFORE resampling to prevent double-conversion.
     * If the ring buffer can't hold the output, return 1 immediately WITHOUT
     * calling swr_convert.  This prevents:
     * - Double-processing through the resampler on retry (corrupts delay buffer)
     * - Partial writes that lose resampled data
     * - Over-counting of audio_written_samples → audio clock runs fast
     * The caller retries with the same AVFrame, which is safe because
     * swr_convert hasn't consumed it yet. */
    {
        size_t estimated_size = (size_t)dst_nb_samples * vp->audio.channels * sizeof(float);
        size_t ring_space = ring_buffer_available_space(&vp->audio.ring);
        if (ring_space < estimated_size) {
            if (vp->state != VP_STATE_BUFFERING) {
                vp->audio.write_stall_count++;
                if (vp->audio.write_stall_count >= 50) {
                    fprintf(stderr, "[audio] Ring buffer stall detected (%d consecutive full writes)"
                            " - triggering early interruption for smooth video\n",
                            vp->audio.write_stall_count);
                    audio_diag("ring buffer stall: %d consecutive full writes", vp->audio.write_stall_count);
                    audio_diag_error("Ring buffer stall (%d writes), triggering interruption",
                            vp->audio.write_stall_count);
                    vp->audio.stream_interrupted = 1;
                    vp->audio.write_stall_count = 0;
                    return 0;  /* Drop frame - video continues smoothly */
                }
            }
            return 1;  /* Ring buffer full, retry later without resampling */
        }
    }

    /* Resample */
    out_samples = swr_convert(vp->audio.swr_ctx,
                               &out_buffer, dst_nb_samples,
                               (const uint8_t **)frame->extended_data, frame->nb_samples);

    if (out_samples < 0) {
        char errbuf[256];
        av_strerror(out_samples, errbuf, sizeof(errbuf));
        fprintf(stderr, "[audio] Resampling failed: %s\n", errbuf);
        return -1;
    }

    if (out_samples == 0)
        return 0;

    /* Calculate byte size of output */
    out_size = out_samples * vp->audio.channels * sizeof(float);

    /* Write to ring buffer FIRST, then count only what was actually written.
     * This prevents audio_written_samples from inflating on partial writes,
     * which would make the audio clock run ahead and trigger false frame skips. */
    size_t written = ring_buffer_write(&vp->audio.ring, out_buffer, out_size);

    if (written > 0) {
        uint32_t stride = vp->audio.channels * sizeof(float);
        uint32_t samples_written = written / stride;

        pthread_mutex_lock(&vp->audio.lock);
        if (vp->audio.audio_written_samples == 0 && frame->pts != AV_NOPTS_VALUE) {
            /* This is the first frame, set base PTS */
            AVRational tb = vp->fmt_ctx->streams[vp->audio_tracks[vp->current_audio_track].stream_index]->time_base;
            vp->audio.base_pts_us = av_rescale_q(frame->pts, tb, AV_TIME_BASE_Q);
        }
        vp->audio.audio_written_samples += samples_written;
        pthread_mutex_unlock(&vp->audio.lock);
    }

    if (written < (size_t)out_size) {
        /* Partial write after swr_convert — rare since we check space before
         * resampling, but possible if PipeWire consumption raced with our
         * space check.  The data was already resampled so we accept the
         * partial write (counted above) rather than retrying swr_convert.
         * The lost tail samples (~a few ms) are inaudible. */
        audio_diag("partial write after resample: %zu/%d bytes", written, out_size);
    }

    /* Successful write - reset stall counter */
    vp->audio.write_stall_count = 0;
    return 0;
}

/* ================================================================
 *  Buffer Control
 * ================================================================ */

void videoplayer_audio_flush(VideoPlayer *vp)
{
    if (!vp)
        return;

    /* Clear ring buffer */
    ring_buffer_clear(&vp->audio.ring);

    /* Reset audio clock and sync tracking */
    pthread_mutex_lock(&vp->audio.lock);
    vp->audio.audio_pts_us = 0;
    vp->audio.base_pts_us = 0;
    vp->audio.audio_written_samples = 0;
    vp->audio.audio_played_samples = 0;
    vp->audio.clock_update_ns = 0;
    vp->audio.clock_snapshot_us = 0;
    vp->audio.output_delay_us = 0;
    vp->audio.last_returned_clock_us = 0;
    vp->audio.last_audio_pts = 0;
    vp->audio.stall_count = 0;
    vp->audio.recovery_frames = 0;
    vp->audio.stream_interrupted = 0;
    vp->audio.needs_device_pause = 0;
    vp->audio.needs_timing_reset = 0;
    vp->audio.write_stall_count = 0;
    vp->audio.reactivate_attempts = 0;
    vp->audio.audio_preroll_done = 0;
    vp->audio.audio_prestart_sent = 0;
    vp->audio.audio_prestart_ns = 0;
    pthread_mutex_unlock(&vp->audio.lock);

    /* Reset resampler if present */
    if (vp->audio.swr_ctx) {
        swr_close(vp->audio.swr_ctx);
        swr_init(vp->audio.swr_ctx);
    }
}

/* ================================================================
 *  A/V Synchronization
 * ================================================================ */

int64_t videoplayer_audio_get_clock(VideoPlayer *vp)
{
    if (!vp)
        return 0;

    /* Read the last on_process snapshot under lock, then interpolate
     * using wallclock elapsed time.  This gives a smooth, continuous
     * audio clock instead of one that jumps in discrete PipeWire
     * quantum steps (~21ms), eliminating the beat-frequency hickup. */
    pthread_mutex_lock(&vp->audio.lock);
    int64_t  base_us = vp->audio.clock_snapshot_us;
    uint64_t base_ns = vp->audio.clock_update_ns;
    int64_t  delay_us = vp->audio.output_delay_us;
    pthread_mutex_unlock(&vp->audio.lock);

    if (base_us <= 0 || base_ns == 0)
        return 0;

    int64_t result;
    uint64_t now_ns = get_time_ns();
    if (now_ns > base_ns) {
        int64_t elapsed_us = (int64_t)(now_ns - base_ns) / 1000;
        /* Cap interpolation to one PipeWire period (~50ms) to avoid
         * runaway extrapolation if on_process stops calling */
        if (elapsed_us > 50000)
            elapsed_us = 50000;
        result = base_us + elapsed_us;
    } else {
        result = base_us;
    }

    /* Subtract PipeWire output pipeline delay so the clock reflects
     * what is audible NOW, not what was just consumed from our buffer.
     * This keeps video, subtitles, and heard audio in sync. */
    if (delay_us > 0)
        result -= delay_us;

    if (result <= 0)
        return 0;

    /* Monotonic clamp: prevent backward clock jumps.
     *
     * When PipeWire's on_process is delayed (scheduling jitter, power
     * management, graph reconfiguration), the wallclock interpolation
     * above extrapolates the clock forward.  When on_process finally
     * fires, the new snapshot reflects fewer samples than the
     * extrapolation predicted, causing a backward jump of ~20ms.
     *
     * This backward jump spikes the A/V diff past the 20ms hold
     * threshold, creating a visible stutter.  Worse, each gap causes
     * PipeWire to output silence for the missed quantum, so the audio
     * content falls behind — and these offsets accumulate over time,
     * producing growing A/V desync.
     *
     * The clamp freezes the clock at its peak until the real value
     * catches up (typically within one quantum, ~21ms).  This smooths
     * the transition and prevents the diff spike. */
    if (result > vp->audio.last_returned_clock_us)
        vp->audio.last_returned_clock_us = result;
    else
        result = vp->audio.last_returned_clock_us;

    return result;
}

void videoplayer_audio_set_clock(VideoPlayer *vp, int64_t pts_us)
{
    if (!vp)
        return;

    pthread_mutex_lock(&vp->audio.lock);
    vp->audio.audio_pts_us = pts_us;
    pthread_mutex_unlock(&vp->audio.lock);
}

/* ================================================================
 *  Volume Control
 * ================================================================ */

void videoplayer_audio_set_volume(VideoPlayer *vp, float volume)
{
    if (!vp)
        return;

    /* Clamp volume */
    if (volume < 0.0f)
        volume = 0.0f;
    if (volume > 1.0f)
        volume = 1.0f;

    /* Volume is applied exclusively in software mixing (on_process reads
     * vp->volume directly).  PipeWire channelVolumes are NOT used — setting
     * both caused double application: software * PipeWire, producing
     * extreme over-attenuation and crackling artifacts on HDMI outputs. */

    fprintf(stderr, "[audio] Volume set to %.0f%%\n", volume * 100);
}

/* Internal: apply volume to PipeWire stream (called from cmd thread) */
static void audio_cmd_apply_volume(VideoPlayer *vp, float volume)
{
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_pod_frame f;

    if (!vp->audio.stream || !vp->audio.loop)
        return;

    /* PipeWire uses cubic volume scale for perceptual linearity */
    float pw_volume = volume * volume * volume;

    float volumes[SPA_AUDIO_MAX_CHANNELS];
    for (int i = 0; i < vp->audio.channels; i++) {
        volumes[i] = pw_volume;
    }

    pw_thread_loop_lock(vp->audio.loop);

    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
    spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float,
                          vp->audio.channels, volumes);
    struct spa_pod *pod = spa_pod_builder_pop(&b, &f);

    pw_stream_set_param(vp->audio.stream, SPA_PARAM_Props, pod);

    pw_thread_loop_unlock(vp->audio.loop);
}

void videoplayer_audio_set_mute(VideoPlayer *vp, int mute)
{
    if (!vp)
        return;

    /* Mute is applied in software mixing (on_process checks vp->muted
     * and vp->audio.muted).  No PipeWire param needed — keeps it simple
     * and avoids the PipeWire mute param race with on_process. */
    vp->audio.muted = mute;

    fprintf(stderr, "[audio] Mute %s\n", mute ? "on" : "off");
}

/* Internal: apply mute to PipeWire stream (called from cmd thread) */
static void audio_cmd_apply_mute(VideoPlayer *vp, int mute)
{
    uint8_t buffer[256];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    if (!vp->audio.stream || !vp->audio.loop)
        return;

    pw_thread_loop_lock(vp->audio.loop);

    struct spa_pod *pod = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
        SPA_PROP_mute, SPA_POD_Bool(mute ? true : false));

    pw_stream_set_param(vp->audio.stream, SPA_PARAM_Props, pod);

    pw_thread_loop_unlock(vp->audio.loop);
}

/* ================================================================
 *  IEC61937 Framing for TrueHD/Atmos Passthrough
 * ================================================================ */

/*
 * IEC61937 burst preamble for TrueHD (MAT format)
 * Pa = 0xF872, Pb = 0x4E1F
 * Pc = data type (0x0016 for TrueHD)
 * Pd = length in bits
 */

#define IEC61937_PA 0xF872
#define IEC61937_PB 0x4E1F
#define IEC61937_TRUEHD_TYPE 0x0016

/* MAT frame for TrueHD is 61440 bytes (61424 payload + 16 header) */
#define MAT_FRAME_SIZE 61440
#define MAT_PKT_OFFSET 2560  /* Offset for major sync in MAT frame */

static int iec61937_create_truehd_burst(uint8_t *out, size_t out_size,
                                         const uint8_t *data, size_t data_len)
{
    uint16_t *out16 = (uint16_t *)out;
    size_t i;

    if (out_size < MAT_FRAME_SIZE)
        return -1;

    /* Clear buffer */
    memset(out, 0, MAT_FRAME_SIZE);

    /* IEC61937 preamble (first 8 bytes) */
    out16[0] = IEC61937_PA;
    out16[1] = IEC61937_PB;
    out16[2] = IEC61937_TRUEHD_TYPE;
    out16[3] = (uint16_t)(data_len * 8);  /* Length in bits */

    /* Copy TrueHD data after preamble */
    if (data_len > MAT_FRAME_SIZE - 8)
        data_len = MAT_FRAME_SIZE - 8;

    memcpy(out + 8, data, data_len);

    /* Byte-swap for S/PDIF (IEC60958) - little endian pairs */
    for (i = 0; i < MAT_FRAME_SIZE / 2; i++) {
        uint16_t val = out16[i];
        out16[i] = ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
    }

    return MAT_FRAME_SIZE;
}

/* ================================================================
 *  Passthrough Mode (TrueHD/Atmos)
 * ================================================================ */

static int init_passthrough_stream(VideoPlayer *vp)
{
    struct pw_properties *props;
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    /* Destroy existing stream if any */
    if (vp->audio.stream) {
        pw_thread_loop_lock(vp->audio.loop);
        spa_hook_remove(&vp->audio.stream_listener);
        pw_stream_destroy(vp->audio.stream);
        vp->audio.stream = NULL;
        pw_thread_loop_unlock(vp->audio.loop);
    }

    /* Allocate IEC61937 buffer */
    if (!vp->audio.iec_buffer) {
        vp->audio.iec_buffer = malloc(MAT_FRAME_SIZE);
        if (!vp->audio.iec_buffer)
            return -1;
        vp->audio.iec_buffer_size = MAT_FRAME_SIZE;
    }

    pw_thread_loop_lock(vp->audio.loop);

    /* Create stream properties for passthrough
     * Allow PipeWire to move stream on device changes (e.g., Bluetooth).
     * Latency hint prevents HDMI xruns (crackling). */
    props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Movie",
        PW_KEY_APP_NAME, "nixlytile",
        PW_KEY_NODE_NAME, "nixlytile-player-passthrough",
        PW_KEY_NODE_DESCRIPTION, "Nixlytile Video Player (Passthrough)",
        PW_KEY_NODE_LATENCY, "2048/192000",
        "audio.format", "S32LE",  /* IEC61937 uses S32LE stereo */
        NULL);

    vp->audio.stream = pw_stream_new(vp->audio.core, "nixlytile-passthrough", props);
    if (!vp->audio.stream) {
        pw_thread_loop_unlock(vp->audio.loop);
        return -1;
    }

    /* IEC61937 uses stereo S32LE at 192kHz for TrueHD */
    struct spa_audio_info_raw audio_info = {
        .format = SPA_AUDIO_FORMAT_S32_LE,
        .rate = 192000,
        .channels = 2,
        .position[0] = SPA_AUDIO_CHANNEL_FL,
        .position[1] = SPA_AUDIO_CHANNEL_FR,
    };

    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info);

    pw_stream_add_listener(vp->audio.stream, &vp->audio.stream_listener,
                           &stream_events, vp);

    if (pw_stream_connect(vp->audio.stream,
                          PW_DIRECTION_OUTPUT,
                          PW_ID_ANY,
                          PW_STREAM_FLAG_AUTOCONNECT |
                          PW_STREAM_FLAG_MAP_BUFFERS |
                          PW_STREAM_FLAG_RT_PROCESS,
                          params, 1) < 0) {
        pw_stream_destroy(vp->audio.stream);
        vp->audio.stream = NULL;
        pw_thread_loop_unlock(vp->audio.loop);
        return -1;
    }

    pw_thread_loop_unlock(vp->audio.loop);

    vp->audio.channels = 2;
    vp->audio.sample_rate = 192000;

    fprintf(stderr, "[audio] Passthrough stream initialized (S32LE 192kHz stereo)\n");

    return 0;
}

int videoplayer_audio_enable_passthrough(VideoPlayer *vp)
{
    if (!vp)
        return -1;

    /* Check if current track supports passthrough */
    AudioTrack *track = &vp->audio_tracks[vp->current_audio_track];
    if (!track->is_atmos && track->codec_id != AV_CODEC_ID_TRUEHD) {
        fprintf(stderr, "[audio] Track does not support passthrough (codec: %s)\n",
                avcodec_get_name(track->codec_id));
        return -1;
    }

    /* Clear ring buffer */
    ring_buffer_clear(&vp->audio.ring);

    /* Initialize passthrough stream */
    if (init_passthrough_stream(vp) < 0) {
        fprintf(stderr, "[audio] Failed to initialize passthrough stream\n");
        return -1;
    }

    vp->audio.passthrough_mode = 1;
    fprintf(stderr, "[audio] Passthrough mode enabled for TrueHD/Atmos\n");

    return 0;
}

void videoplayer_audio_disable_passthrough(VideoPlayer *vp)
{
    if (!vp)
        return;

    if (!vp->audio.passthrough_mode)
        return;

    vp->audio.passthrough_mode = 0;

    /* Reinitialize normal audio stream */
    videoplayer_audio_cleanup(vp);
    videoplayer_audio_init(vp);

    fprintf(stderr, "[audio] Passthrough mode disabled\n");
}

/* Queue raw TrueHD packet for passthrough (bypasses decoder) */
int videoplayer_audio_queue_passthrough(VideoPlayer *vp, AVPacket *pkt)
{
    int burst_size;

    if (!vp || !pkt || !vp->audio.passthrough_mode)
        return -1;

    if (!vp->audio.iec_buffer)
        return -1;

    /* Create IEC61937 burst from TrueHD packet */
    burst_size = iec61937_create_truehd_burst(vp->audio.iec_buffer,
                                               vp->audio.iec_buffer_size,
                                               pkt->data, pkt->size);
    if (burst_size < 0)
        return -1;

    /* Write to ring buffer */
    size_t written = ring_buffer_write(&vp->audio.ring,
                                        vp->audio.iec_buffer, burst_size);
    if (written < (size_t)burst_size) {
        /* Buffer full */
        return 1;
    }

    return 0;
}
