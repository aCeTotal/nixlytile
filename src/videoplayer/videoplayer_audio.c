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

#include "videoplayer.h"

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
    size_t avail;
    pthread_mutex_lock(&ring->lock);
    avail = ring->available;
    pthread_mutex_unlock(&ring->lock);
    return avail;
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
    }

    /* Apply volume and mute */
    if (vp->audio.muted || vp->muted) {
        memset(dst, 0, bytes_needed);
    } else {
        float vol = vp->audio.volume * vp->volume;
        if (vol < 0.999f) {
            float *samples = dst;
            uint32_t total_samples = n_frames * vp->audio.channels;
            for (uint32_t i = 0; i < total_samples; i++) {
                samples[i] *= vol;
            }
        }
    }

    /* Update audio clock based on samples actually played */
    if (bytes_read > 0) {
        uint32_t frames_played = bytes_read / stride;

        /* Get buffer level BEFORE taking audio.lock to avoid deadlock */
        size_t buffer_bytes = ring_buffer_available(&vp->audio.ring);
        size_t buffer_samples = buffer_bytes / stride;

        pthread_mutex_lock(&vp->audio.lock);
        vp->audio.audio_played_samples += frames_played;

        /* Calculate audio PTS based on difference between written and remaining in buffer */
        /* This gives us the actual playback position */
        uint64_t played_from_written = 0;
        if (vp->audio.audio_written_samples > buffer_samples) {
            played_from_written = vp->audio.audio_written_samples - buffer_samples;
        }
        int64_t samples_to_us = (played_from_written * 1000000LL) / vp->audio.sample_rate;
        vp->audio.audio_pts_us = vp->audio.base_pts_us + samples_to_us;
        pthread_mutex_unlock(&vp->audio.lock);
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

    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "[audio] Stream error: %s\n", error ? error : "unknown");
        /* Mark as interrupted so video continues with timing-based playback */
        vp->audio.stream_interrupted = 1;
    }

    /* When stream is interrupted (e.g., Bluetooth/controller reconnection),
     * flush the ring buffer and reset sync state.
     * IMPORTANT: Set interrupted flag BEFORE taking lock to prevent render thread
     * from using stale A/V sync during the transition */
    if (old == PW_STREAM_STATE_STREAMING &&
        (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_UNCONNECTED ||
         state == PW_STREAM_STATE_ERROR)) {
        fprintf(stderr, "[audio] Stream interrupted, flushing buffer - video will continue with timing-based playback\n");

        /* Signal render thread to use timing-based playback immediately */
        vp->audio.stream_interrupted = 1;

        ring_buffer_clear(&vp->audio.ring);

        /* Reset sync tracking */
        pthread_mutex_lock(&vp->audio.lock);
        vp->audio.audio_pts_us = 0;
        vp->audio.base_pts_us = 0;
        vp->audio.audio_written_samples = 0;
        vp->audio.audio_played_samples = 0;
        vp->audio.last_clock = 0;
        vp->audio.last_audio_pts = 0;
        vp->audio.stall_count = 0;
        /* Set recovery frames - wait for audio to stabilize before using A/V sync again */
        vp->audio.recovery_frames = 60;  /* ~1 second at 60Hz for better stability */
        pthread_mutex_unlock(&vp->audio.lock);
    }

    /* NOTE: We intentionally do NOT call pw_stream_set_active(true) when entering
     * PAUSED state. During Bluetooth device reconnection, PipeWire may rapidly
     * cycle between STREAMING and PAUSED as it reconfigures audio routing.
     * Immediately reactivating causes STREAMING<->PAUSED oscillation that
     * destabilizes PipeWire. Instead, the recovery mechanism in
     * videoplayer_audio_check_recovery() handles reconnection after PipeWire
     * has fully stabilized. */

    /* Handle UNCONNECTED state - stream was disconnected, need to try reconnecting */
    if (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_CONNECTING) {
        fprintf(stderr, "[audio] Stream unconnected - will attempt reconnection on next audio frame\n");
        /* Keep stream_interrupted = 1 - video continues independently */
    }

    /* When stream resumes streaming, reset video timing for clean sync */
    if (state == PW_STREAM_STATE_STREAMING &&
        (old == PW_STREAM_STATE_PAUSED || old == PW_STREAM_STATE_CONNECTING)) {
        fprintf(stderr, "[audio] Stream resumed streaming, video will resync gradually\n");
        /* Clear interrupted flag - audio is streaming again */
        vp->audio.stream_interrupted = 0;

        /* Reset sync state for clean restart */
        pthread_mutex_lock(&vp->audio.lock);
        vp->audio.recovery_frames = 60;  /* Allow gradual resync */
        pthread_mutex_unlock(&vp->audio.lock);
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
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
            if (vp->audio.stream && vp->audio.loop && !vp->audio.stream_interrupted) {
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
            audio_cmd_apply_volume(vp, cmd.volume);
            break;

        case AUDIO_CMD_MUTE:
            audio_cmd_apply_mute(vp, cmd.mute);
            break;

        case AUDIO_CMD_RECREATE:
            fprintf(stderr, "[audio-cmd] Recreating audio stream\n");
            videoplayer_audio_cleanup(vp);
            if (videoplayer_audio_init(vp) == 0) {
                fprintf(stderr, "[audio-cmd] Stream recreated successfully\n");
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

    fprintf(stderr, "[audio] Command pipe and thread initialized\n");
    return 0;
}

void videoplayer_audio_cmd_shutdown(VideoPlayer *vp)
{
    if (!vp)
        return;

    if (vp->audio.cmd_thread_running) {
        AudioCmd cmd = { .type = AUDIO_CMD_QUIT };
        audio_cmd_send(vp, &cmd);
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
    vp->audio.last_clock = 0;
    vp->audio.last_audio_pts = 0;
    vp->audio.stall_count = 0;
    vp->audio.recovery_frames = 0;
    vp->audio.stream_interrupted = 0;
    vp->audio.audio_recreating = 0;

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
        return -1;
    }

    pw_thread_loop_lock(vp->audio.loop);

    /* Connect to PipeWire server */
    vp->audio.core = pw_context_connect(vp->audio.context, NULL, 0);
    if (!vp->audio.core) {
        fprintf(stderr, "[audio] Failed to connect to PipeWire\n");
        pw_thread_loop_unlock(vp->audio.loop);
        pw_thread_loop_stop(vp->audio.loop);
        pw_context_destroy(vp->audio.context);
        pw_thread_loop_destroy(vp->audio.loop);
        vp->audio.context = NULL;
        vp->audio.loop = NULL;
        ring_buffer_free(&vp->audio.ring);
        return -1;
    }

    /* Create stream properties
     * Allow PipeWire to move our stream when devices change (e.g., Bluetooth connect).
     * The on_stream_state_changed callback handles interruptions gracefully. */
    props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Movie",
        PW_KEY_APP_NAME, "nixlytile",
        PW_KEY_NODE_NAME, "nixlytile-player",
        PW_KEY_NODE_DESCRIPTION, "Nixlytile Video Player",
        NULL);

    /* Create stream */
    vp->audio.stream = pw_stream_new(vp->audio.core, "nixlytile-audio-stream", props);
    if (!vp->audio.stream) {
        fprintf(stderr, "[audio] Failed to create stream\n");
        pw_core_disconnect(vp->audio.core);
        pw_thread_loop_unlock(vp->audio.loop);
        pw_thread_loop_stop(vp->audio.loop);
        pw_context_destroy(vp->audio.context);
        pw_thread_loop_destroy(vp->audio.loop);
        vp->audio.core = NULL;
        vp->audio.context = NULL;
        vp->audio.loop = NULL;
        ring_buffer_free(&vp->audio.ring);
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
        return -1;
    }

    pw_thread_loop_unlock(vp->audio.loop);

    fprintf(stderr, "[audio] Initialized: %d channels @ %d Hz\n", channels, sample_rate);

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

    if (vp->audio.loop) {
        pw_thread_loop_lock(vp->audio.loop);

        if (vp->audio.stream) {
            spa_hook_remove(&vp->audio.stream_listener);
            pw_stream_destroy(vp->audio.stream);
            vp->audio.stream = NULL;
        }

        if (vp->audio.core) {
            pw_core_disconnect(vp->audio.core);
            vp->audio.core = NULL;
        }

        pw_thread_loop_unlock(vp->audio.loop);
        pw_thread_loop_stop(vp->audio.loop);

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

    fprintf(stderr, "[audio] Cleanup complete\n");
}

/* ================================================================
 *  Stream Recovery (for Bluetooth/device reconnection)
 * ================================================================ */

/*
 * Check if audio stream needs recovery and attempt to restore it.
 * This should be called periodically (e.g., from present_frame ~every second).
 * Returns: 0 = no action needed, 1 = recovery in progress, -1 = recovery failed
 *
 * IMPORTANT: This function is non-blocking — it sends a command via pipe
 * to the audio command thread which handles the actual PipeWire recreation.
 */
int videoplayer_audio_check_recovery(VideoPlayer *vp)
{
    /* Track how long we've been interrupted (called ~every second from render thread) */
    static int interrupted_ticks = 0;

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
        interrupted_ticks++;

        /* After ~5 seconds of continuous interruption, send RECREATE command */
        if (interrupted_ticks >= 5) {
            fprintf(stderr, "[audio] Stream interrupted for %d seconds, sending RECREATE command\n",
                    interrupted_ticks);
            interrupted_ticks = 0;

            if (!vp->audio.audio_recreating) {
                vp->audio.audio_recreating = 1;
                AudioCmd cmd = { .type = AUDIO_CMD_RECREATE };
                audio_cmd_send(vp, &cmd);
            }
        }

        return 1;  /* Recovery in progress */
    }

    /* Stream is healthy, reset the counter */
    interrupted_ticks = 0;
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
            return -1;
        }

        ret = swr_init(vp->audio.swr_ctx);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[audio] Failed to init resampler: %s\n", errbuf);
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
        /* Buffer full - this is normal, caller should retry later */
        return 1;
    }

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
    vp->audio.last_clock = 0;
    vp->audio.last_audio_pts = 0;
    vp->audio.stall_count = 0;
    vp->audio.recovery_frames = 0;
    vp->audio.stream_interrupted = 0;
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

    /* Use trylock to avoid blocking render thread if PipeWire thread
     * holds the mutex (e.g., during Bluetooth controller reconnection) */
    if (pthread_mutex_trylock(&vp->audio.lock) == 0) {
        vp->audio.last_clock = vp->audio.audio_pts_us;
        pthread_mutex_unlock(&vp->audio.lock);
    }
    /* If we couldn't get the lock, return last known value (per-instance) */

    return vp->audio.last_clock;
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

    /* Store volume immediately for software mixing (on_process uses this) */
    vp->audio.volume = volume;

    if (!vp->audio.stream || !vp->audio.loop)
        return;

    /* Send to cmd thread for PipeWire param update */
    AudioCmd cmd = { .type = AUDIO_CMD_VOLUME, .volume = volume };
    audio_cmd_send(vp, &cmd);

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

    /* Store mute state immediately for software mixing (on_process uses this) */
    vp->audio.muted = mute;

    if (!vp->audio.stream || !vp->audio.loop)
        return;

    /* Send to cmd thread for PipeWire param update */
    AudioCmd cmd = { .type = AUDIO_CMD_MUTE, .mute = mute };
    audio_cmd_send(vp, &cmd);

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
     * Allow PipeWire to move stream on device changes (e.g., Bluetooth). */
    props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Movie",
        PW_KEY_APP_NAME, "nixlytile",
        PW_KEY_NODE_NAME, "nixlytile-player-passthrough",
        PW_KEY_NODE_DESCRIPTION, "Nixlytile Video Player (Passthrough)",
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
