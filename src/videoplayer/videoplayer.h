/*
 * Nixlytile Video Player
 * FFmpeg-based integrated video player with HDR, 7.1/Atmos, and silky smooth playback
 */

#ifndef VIDEOPLAYER_H
#define VIDEOPLAYER_H

#include <stdint.h>
#include <pthread.h>
#include <limits.h>

/* FFmpeg */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/mastering_display_metadata.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

/* PipeWire */
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

/* libass for subtitles */
#include <ass/ass.h>

/* xkbcommon for key handling */
#include <xkbcommon/xkbcommon.h>

/* Forward declarations */
struct wlr_scene_tree;
struct wlr_scene_buffer;
struct wlr_scene_rect;
struct wlr_buffer;
struct wl_event_source;
struct Monitor;

/* ================================================================
 *  Enums
 * ================================================================ */

typedef enum {
    VP_STATE_IDLE = 0,
    VP_STATE_LOADING,
    VP_STATE_PLAYING,
    VP_STATE_PAUSED,
    VP_STATE_SEEKING,
    VP_STATE_BUFFERING,
    VP_STATE_ERROR
} VideoPlayerState;

typedef enum {
    VP_HW_VAAPI = 0,
    VP_HW_NVDEC,
    VP_HW_NONE  /* Software fallback */
} VideoPlayerHWAccel;

/* ================================================================
 *  Track Structures
 * ================================================================ */

typedef struct VideoTrack {
    int stream_index;
    int width;
    int height;
    AVRational framerate;          /* e.g., {24000, 1001} for 23.976 */
    AVRational time_base;
    enum AVPixelFormat pix_fmt;
    enum AVPixelFormat sw_pix_fmt; /* Software format for HW decode */

    /* HDR metadata */
    int is_hdr;
    enum AVColorPrimaries color_primaries;
    enum AVColorTransferCharacteristic color_trc;
    enum AVColorSpace color_space;
    enum AVColorRange color_range;
    int bit_depth;

    /* Mastering display metadata */
    int has_mastering_display;
    AVMasteringDisplayMetadata mastering;

    /* Content light level */
    int has_content_light;
    AVContentLightMetadata content_light;
} VideoTrack;

typedef struct AudioTrack {
    int stream_index;
    int index;                     /* Track index for UI */
    char title[128];
    char language[8];
    int channels;
    int sample_rate;
    enum AVCodecID codec_id;
    int is_atmos;                  /* TrueHD with Atmos */
    int is_lossless;               /* FLAC, TrueHD, DTS-HD MA */
    int is_default;
} AudioTrack;

typedef struct SubtitleTrack {
    int stream_index;
    int index;                     /* Track index for UI */
    char title[128];
    char language[8];
    enum AVCodecID codec_id;
    int is_text_based;             /* SRT/ASS vs bitmap PGS/VOBSUB */
    int is_default;
    int is_forced;
} SubtitleTrack;

/* ================================================================
 *  Frame Queue
 * ================================================================ */

#define VP_FRAME_QUEUE_SIZE 16     /* Large buffer for smooth playback */

typedef struct VideoPlayerFrame {
    struct wlr_buffer *buffer;
    int64_t pts_us;                /* Presentation timestamp (microseconds) */
    int64_t duration_us;           /* Frame duration (microseconds) */
    int width;
    int height;
    int ready;                     /* 1 if frame is ready for display */
} VideoPlayerFrame;

/* ================================================================
 *  Control Bar
 * ================================================================ */

#define CONTROL_BAR_HEIGHT      60
#define CONTROL_BAR_MARGIN      20
#define CONTROL_BAR_BUTTON_SIZE 40
#define CONTROL_BAR_AUTO_HIDE_MS 3000

typedef struct VideoPlayerControlBar {
    struct wlr_scene_tree *tree;
    struct wlr_scene_tree *bg;
    int visible;
    int hovered;
    int x, y, width, height;

    /* Element positions (relative to bar) */
    int play_btn_x, play_btn_w;
    int progress_x, progress_w;
    int time_x, time_w;
    int volume_x, volume_w;
    int audio_x, audio_w;
    int subtitle_x, subtitle_w;

    /* Interaction state */
    int dragging_progress;
    float drag_position;           /* 0.0 - 1.0 */
    int volume_popup_visible;
    int audio_popup_visible;
    int subtitle_popup_visible;

    /* Animation */
    float hover_alpha;
    uint64_t last_activity_ms;
    struct wl_event_source *hide_timer;
} VideoPlayerControlBar;

/* ================================================================
 *  Audio Ring Buffer
 * ================================================================ */

#define AUDIO_RING_BUFFER_SIZE (48000 * 8 * sizeof(float) * 2)  /* ~1 second of 8ch 48kHz */

typedef struct AudioRingBuffer {
    uint8_t *data;
    size_t size;                   /* Total buffer size in bytes */
    size_t read_pos;               /* Read position */
    size_t write_pos;              /* Write position */
    size_t available;              /* Bytes available to read */
    pthread_mutex_t lock;
} AudioRingBuffer;

/* ================================================================
 *  Audio State
 * ================================================================ */

typedef struct VideoPlayerAudio {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook stream_listener;

    /* Audio ring buffer */
    AudioRingBuffer ring;

    /* Audio clock for A/V sync */
    int64_t audio_pts_us;          /* Current audio position */
    int64_t base_pts_us;           /* PTS of first sample in buffer */
    uint64_t audio_written_samples;
    uint64_t audio_played_samples; /* Samples actually played */
    int sample_rate;
    int channels;

    /* Sync tracking (per-instance, not static) */
    int64_t last_clock;            /* Last returned clock value (for trylock fallback) */
    int64_t last_audio_pts;        /* Last audio PTS for stall detection */
    int stall_count;               /* Frames since audio clock progressed */
    int recovery_frames;           /* Frames to wait after audio recovery before using A/V sync */
    volatile int stream_interrupted; /* Set when stream is interrupted (controller reconnect, etc.) */
    volatile int audio_recreating;   /* Set while background thread is recreating audio stream */
    volatile int needs_timing_reset; /* Set by audio thread to tell render thread to reset frame timing */
    int write_stall_count;           /* Consecutive buffer-full writes (stall detection for decode thread) */
    int reactivate_attempts;         /* Attempts to reactivate stream before RECREATE */

    /* Passthrough mode */
    int passthrough_mode;          /* IEC61937 for TrueHD/Atmos */
    uint8_t *iec_buffer;           /* IEC61937 framing buffer */
    size_t iec_buffer_size;

    /* Resampling */
    struct SwrContext *swr_ctx;
    uint8_t *resample_buffer;
    int resample_buffer_size;
    int resample_buffer_samples;

    /* Volume/Mute */
    float volume;
    int muted;

    /* Audio command pipe (non-blocking from compositor thread) */
    int cmd_pipe[2];                 /* [0]=read (cmd thread), [1]=write (compositor) */
    pthread_t cmd_thread;
    volatile int cmd_thread_running;

    pthread_mutex_t lock;
} VideoPlayerAudio;

/* ================================================================
 *  Subtitle State
 * ================================================================ */

typedef struct VideoPlayerSubtitle {
    ASS_Library *library;
    ASS_Renderer *renderer;
    ASS_Track *track;

    /* Current subtitle image */
    struct wlr_buffer *current_buffer;
    int64_t current_pts_us;
    int64_t current_end_us;

    /* For bitmap subtitles */
    struct SwsContext *sws_ctx;
} VideoPlayerSubtitle;

/* ================================================================
 *  Main Video Player Structure
 * ================================================================ */

#define VP_MAX_AUDIO_TRACKS    16
#define VP_MAX_SUBTITLE_TRACKS 16

typedef struct VideoPlayer {
    /* State */
    VideoPlayerState state;
    VideoPlayerHWAccel hw_accel;
    struct Monitor *mon;
    char error_msg[256];

    /* Scene integration */
    struct wlr_scene_tree *tree;           /* Main container (on LyrOverlay) */
    struct wlr_scene_rect *bg_rect;        /* Black background for letterboxing */
    struct wlr_scene_tree *video_tree;     /* Video frame layer */
    struct wlr_scene_tree *subtitle_tree;  /* Subtitle overlay */
    struct wlr_scene_buffer *frame_node;   /* Current video frame */
    VideoPlayerControlBar control_bar;
    int fullscreen_width;                  /* Current fullscreen size */
    int fullscreen_height;

    /* File info */
    char filepath[PATH_MAX];
    int64_t duration_us;
    int64_t position_us;

    /* Tracks */
    VideoTrack video;
    AudioTrack audio_tracks[VP_MAX_AUDIO_TRACKS];
    int audio_track_count;
    int current_audio_track;
    SubtitleTrack subtitle_tracks[VP_MAX_SUBTITLE_TRACKS];
    int subtitle_track_count;
    int current_subtitle_track;            /* -1 = off */

    /* FFmpeg demuxing */
    AVFormatContext *fmt_ctx;

    /* Video decoding */
    AVCodecContext *video_codec_ctx;
    AVBufferRef *hw_device_ctx;
    AVBufferRef *hw_frames_ctx;
    struct SwsContext *sws_ctx;
    AVFrame *decode_frame;
    AVFrame *sw_frame;                     /* For HW->SW transfer */

    /* Audio decoding */
    AVCodecContext *audio_codec_ctx;
    AVFrame *audio_frame;

    /* Subtitle decoding */
    AVCodecContext *subtitle_codec_ctx;
    VideoPlayerSubtitle subtitle;

    /* Frame queue (triple buffering) */
    VideoPlayerFrame frame_queue[VP_FRAME_QUEUE_SIZE];
    int frame_read_idx;
    int frame_write_idx;
    int frames_queued;
    pthread_mutex_t frame_mutex;
    pthread_cond_t frame_cond;

    /* Decode thread */
    pthread_t decode_thread;
    volatile int decode_running;
    volatile int seek_requested;
    int64_t seek_target_us;

    /* Audio */
    VideoPlayerAudio audio;

    /* Frame pacing */
    uint64_t last_frame_ns;
    uint64_t last_present_time_ns;         /* Wall-clock time of last actual frame presentation */
    uint64_t frame_interval_ns;
    uint64_t display_interval_ns;          /* Display refresh interval */
    int64_t av_sync_offset_us;
    int frame_repeat_mode;                 /* 0=VRR, 1=fixed, 2=3:2 pulldown */
    int frame_repeat_count;
    int current_repeat;
    float video_fps;
    float display_hz;                      /* Current display refresh rate */

    /* Volume */
    float volume;                          /* 0.0 - 1.0 */
    int muted;

    /* Playback speed */
    float speed;                           /* 1.0 = normal */
} VideoPlayer;

/* ================================================================
 *  Public API
 * ================================================================ */

/* Lifecycle */
VideoPlayer *videoplayer_create(struct Monitor *mon);
void videoplayer_destroy(VideoPlayer *vp);

/* File operations */
int videoplayer_open(VideoPlayer *vp, const char *filepath);
void videoplayer_close(VideoPlayer *vp);

/* Playback control */
void videoplayer_play(VideoPlayer *vp);
void videoplayer_pause(VideoPlayer *vp);
void videoplayer_toggle_pause(VideoPlayer *vp);
void videoplayer_stop(VideoPlayer *vp);
void videoplayer_seek(VideoPlayer *vp, int64_t position_us);
void videoplayer_seek_relative(VideoPlayer *vp, int64_t offset_us);

/* Track selection */
void videoplayer_set_audio_track(VideoPlayer *vp, int index);
void videoplayer_set_subtitle_track(VideoPlayer *vp, int index);  /* -1 = off */
void videoplayer_cycle_audio_track(VideoPlayer *vp);
void videoplayer_cycle_subtitle_track(VideoPlayer *vp);

/* Volume */
void videoplayer_set_volume(VideoPlayer *vp, float volume);
void videoplayer_adjust_volume(VideoPlayer *vp, float delta);
void videoplayer_toggle_mute(VideoPlayer *vp);

/* Frame presentation (called from compositor) */
void videoplayer_present_frame(VideoPlayer *vp, uint64_t now_ns);

/* Display mode configuration */
void videoplayer_setup_display_mode(VideoPlayer *vp, float display_hz, int vrr_capable);

/* Input handling */
int videoplayer_handle_key(VideoPlayer *vp, uint32_t mods, xkb_keysym_t sym);
int videoplayer_handle_motion(VideoPlayer *vp, int x, int y);
int videoplayer_handle_button(VideoPlayer *vp, int x, int y, uint32_t button, int pressed);

/* UI */
void videoplayer_show_control_bar(VideoPlayer *vp);
void videoplayer_hide_control_bar(VideoPlayer *vp);
void videoplayer_render_control_bar(VideoPlayer *vp);
void videoplayer_render_subtitles(VideoPlayer *vp, int64_t pts_us);

/* Utility */
const char *videoplayer_state_str(VideoPlayerState state);
void videoplayer_format_time(char *buf, size_t len, int64_t us);

/* Rendering (internal) */
struct wlr_buffer *videoplayer_create_frame_buffer(VideoPlayer *vp, AVFrame *frame);
void videoplayer_update_frame_buffer(VideoPlayer *vp, struct wlr_buffer *buffer);
int videoplayer_init_scene(VideoPlayer *vp, struct wlr_scene_tree *parent);
void videoplayer_set_position(VideoPlayer *vp, int x, int y);
void videoplayer_set_visible(VideoPlayer *vp, int visible);
void videoplayer_set_fullscreen_size(VideoPlayer *vp, int width, int height);

#endif /* VIDEOPLAYER_H */
