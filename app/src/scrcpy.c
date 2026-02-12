#include "scrcpy.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#ifdef _WIN32
// not needed here, but winsock2.h must never be included AFTER windows.h
# include <winsock2.h>
# include <windows.h>
#endif

#include "adb/adb.h"
#include "audio_player.h"
#include "controller.h"
#include "decoder.h"
#include "delay_buffer.h"
#include "demuxer.h"
#include "events.h"
#include "file_pusher.h"
#include "keyboard_sdk.h"
#include "mouse_sdk.h"
#include "recorder.h"
#include "screen.h"
#include "server.h"
#include "uhid/gamepad_uhid.h"
#include "uhid/keyboard_uhid.h"
#include "uhid/mouse_uhid.h"
#ifdef HAVE_USB
# include "usb/aoa_hid.h"
# include "usb/gamepad_aoa.h"
# include "usb/keyboard_aoa.h"
# include "usb/mouse_aoa.h"
# include "usb/usb.h"
#endif
#include "util/acksync.h"
#include "util/log.h"
#include "util/rand.h"
#include "util/timeout.h"
#include "util/tick.h"
#ifdef HAVE_V4L2
# include "v4l2_sink.h"
#endif

struct scrcpy {
    struct sc_server server;
    struct sc_screen screen;
    struct sc_audio_player audio_player;
    struct sc_demuxer video_demuxer;
    struct sc_demuxer audio_demuxer;
    struct sc_decoder video_decoder;
    struct sc_decoder audio_decoder;
    struct sc_recorder recorder;
    struct sc_delay_buffer video_buffer;
#ifdef HAVE_V4L2
    struct sc_v4l2_sink v4l2_sink;
    struct sc_delay_buffer v4l2_buffer;
#endif
    struct sc_controller controller;
    struct sc_file_pusher file_pusher;
#ifdef HAVE_USB
    struct sc_usb usb;
    struct sc_aoa aoa;
    // sequence/ack helper to synchronize clipboard and Ctrl+v via HID
    struct sc_acksync acksync;
#endif
    struct sc_uhid_devices uhid_devices;
    union {
        struct sc_keyboard_sdk keyboard_sdk;
        struct sc_keyboard_uhid keyboard_uhid;
#ifdef HAVE_USB
        struct sc_keyboard_aoa keyboard_aoa;
#endif
    };
    union {
        struct sc_mouse_sdk mouse_sdk;
        struct sc_mouse_uhid mouse_uhid;
#ifdef HAVE_USB
        struct sc_mouse_aoa mouse_aoa;
#endif
    };
    union {
        struct sc_gamepad_uhid gamepad_uhid;
#ifdef HAVE_USB
        struct sc_gamepad_aoa gamepad_aoa;
#endif
    };
    struct sc_timeout timeout;
};

struct sc_secure_content_monitor {
    sc_thread thread;
    struct sc_intr intr;
    const char *serial;
    atomic_bool stopped;
    bool reported;
    bool last_bouncer_showing;
};

#define SC_SECURE_CONTENT_POLL_INTERVAL_MS 1200

static bool
sc_push_secure_content_event(bool detected) {
    SDL_Event event = {
        .user = {
            .type = SC_EVENT_SCREEN_SECURE_CONTENT,
            .code = detected ? 1 : 0,
        },
    };

    int ret = SDL_PushEvent(&event);
    if (ret != 1) {
        if (ret < 0) {
            LOGW("Could not post secure-content event: %s", SDL_GetError());
        }
        return false;
    }

    return true;
}

static int
run_secure_content_monitor(void *data) {
    struct sc_secure_content_monitor *monitor = data;

    while (!atomic_load_explicit(&monitor->stopped, memory_order_relaxed)) {
        bool bouncer_showing;
        bool ok = sc_adb_is_keyguard_bouncer_showing(&monitor->intr,
                                                     monitor->serial,
                                                     &bouncer_showing);
        if (ok) {
            if (!monitor->reported
                    || bouncer_showing != monitor->last_bouncer_showing) {
                monitor->reported = true;
                monitor->last_bouncer_showing = bouncer_showing;
                sc_push_secure_content_event(bouncer_showing);
            }
        }

        uint32_t slept_ms = 0;
        while (slept_ms < SC_SECURE_CONTENT_POLL_INTERVAL_MS
                && !atomic_load_explicit(&monitor->stopped,
                                         memory_order_relaxed)) {
            SDL_Delay(100);
            slept_ms += 100;
        }
    }

    return 0;
}

static bool
sc_secure_content_monitor_init(struct sc_secure_content_monitor *monitor,
                               const char *serial) {
    assert(serial);

    bool ok = sc_intr_init(&monitor->intr);
    if (!ok) {
        return false;
    }

    monitor->serial = serial;
    monitor->reported = false;
    monitor->last_bouncer_showing = false;
    atomic_store_explicit(&monitor->stopped, false, memory_order_relaxed);
    return true;
}

static bool
sc_secure_content_monitor_start(struct sc_secure_content_monitor *monitor) {
    bool ok = sc_thread_create(&monitor->thread, run_secure_content_monitor,
                               "scrcpy-secure", monitor);
    if (!ok) {
        LOGW("Could not start secure-content monitor thread");
        return false;
    }
    return true;
}

static void
sc_secure_content_monitor_stop(struct sc_secure_content_monitor *monitor) {
    atomic_store_explicit(&monitor->stopped, true, memory_order_relaxed);
    sc_intr_interrupt(&monitor->intr);
}

static void
sc_secure_content_monitor_join(struct sc_secure_content_monitor *monitor) {
    sc_thread_join(&monitor->thread, NULL);
}

static void
sc_secure_content_monitor_destroy(struct sc_secure_content_monitor *monitor) {
    sc_intr_destroy(&monitor->intr);
}

#ifdef _WIN32
static BOOL WINAPI windows_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        sc_push_event(SDL_QUIT);
        return TRUE;
    }
    return FALSE;
}
#endif // _WIN32

static void
sdl_set_hints(const char *render_driver) {
    if (render_driver && !SDL_SetHint(SDL_HINT_RENDER_DRIVER, render_driver)) {
        LOGW("Could not set render driver");
    }

    // App name used in various contexts (such as PulseAudio)
#if defined(SCRCPY_SDL_HAS_HINT_APP_NAME)
    if (!SDL_SetHint(SDL_HINT_APP_NAME, "scrcpy")) {
        LOGW("Could not set app name");
    }
#elif defined(SCRCPY_SDL_HAS_HINT_AUDIO_DEVICE_APP_NAME)
    if (!SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_NAME, "scrcpy")) {
        LOGW("Could not set audio device app name");
    }
#endif

    // Linear filtering
    if (!SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1")) {
        LOGW("Could not enable linear filtering");
    }

    // Handle a click to gain focus as any other click
    if (!SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1")) {
        LOGW("Could not enable mouse focus clickthrough");
    }

#ifdef SCRCPY_SDL_HAS_HINT_TOUCH_MOUSE_EVENTS
    // Disable synthetic mouse events from touch events
    // Touch events with id SDL_TOUCH_MOUSEID are ignored anyway, but it is
    // better not to generate them in the first place.
    if (!SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0")) {
        LOGW("Could not disable synthetic mouse events");
    }
#endif

#ifdef SCRCPY_SDL_HAS_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
    // Disable compositor bypassing on X11
    if (!SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0")) {
        LOGW("Could not disable X11 compositor bypass");
    }
#endif

    // Do not minimize on focus loss
    if (!SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0")) {
        LOGW("Could not disable minimize on focus loss");
    }

    if (!SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1")) {
        LOGW("Could not allow joystick background events");
    }
}

static void
sdl_configure(bool video_playback, bool disable_screensaver) {
#ifdef _WIN32
    // Clean up properly on Ctrl+C on Windows
    bool ok = SetConsoleCtrlHandler(windows_ctrl_handler, TRUE);
    if (!ok) {
        LOGW("Could not set Ctrl+C handler");
    }
#endif // _WIN32

    if (!video_playback) {
        return;
    }

    if (disable_screensaver) {
        SDL_DisableScreenSaver();
    } else {
        SDL_EnableScreenSaver();
    }
}

static enum scrcpy_exit_code
event_loop(struct scrcpy *s, bool has_screen) {
    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        switch (event.type) {
            case SC_EVENT_DEVICE_DISCONNECTED:
                LOGW("Device disconnected");
                if (has_screen) {
                    sc_screen_set_connection_state(&s->screen,
                        SC_SCREEN_CONNECTION_CONNECTING);
                }
                return SCRCPY_EXIT_DISCONNECTED;
            case SC_EVENT_SERVER_CONNECTION_FAILED:
                if (has_screen) {
                    sc_screen_set_connection_state(&s->screen,
                        SC_SCREEN_CONNECTION_CONNECTING);
                }
                return SCRCPY_EXIT_DISCONNECTED;
            case SC_EVENT_SERVER_CONNECTED:
                if (has_screen) {
                    sc_screen_set_connection_state(&s->screen,
                        SC_SCREEN_CONNECTION_RUNNING);
                }
                break;
            case SC_EVENT_DEMUXER_ERROR:
                LOGE("Demuxer error");
                return SCRCPY_EXIT_FAILURE;
            case SC_EVENT_CONTROLLER_ERROR:
                LOGE("Controller error");
                return SCRCPY_EXIT_FAILURE;
            case SC_EVENT_RECORDER_ERROR:
                LOGE("Recorder error");
                return SCRCPY_EXIT_FAILURE;
            case SC_EVENT_AOA_OPEN_ERROR:
                LOGE("AOA open error");
                return SCRCPY_EXIT_FAILURE;
            case SC_EVENT_TIME_LIMIT_REACHED:
                LOGI("Time limit reached");
                return SCRCPY_EXIT_SUCCESS;
            case SDL_QUIT:
                LOGD("User requested to quit");
                return SCRCPY_EXIT_SUCCESS;
            case SC_EVENT_RUN_ON_MAIN_THREAD: {
                sc_runnable_fn run = event.user.data1;
                void *userdata = event.user.data2;
                run(userdata);
                break;
            }
            default:
                if (has_screen && !sc_screen_handle_event(&s->screen, &event)) {
                    return SCRCPY_EXIT_FAILURE;
                }
                break;
        }
    }
    return SCRCPY_EXIT_FAILURE;
}

static void
terminate_event_loop(void) {
    sc_reject_new_runnables();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SC_EVENT_RUN_ON_MAIN_THREAD) {
            // Make sure all posted runnables are run, to avoid memory leaks
            sc_runnable_fn run = event.user.data1;
            void *userdata = event.user.data2;
            run(userdata);
        }
    }
}

static bool
wait_retry_delay(struct scrcpy *s, bool has_screen, uint32_t delay_ms) {
    uint64_t deadline = SDL_GetTicks64() + delay_ms;

    for (;;) {
        uint64_t now = SDL_GetTicks64();
        if (now >= deadline) {
            return true;
        }

        SDL_Event event;
        uint32_t timeout = (uint32_t) (deadline - now);
        if (!SDL_WaitEventTimeout(&event, timeout)) {
            // timeout
            return true;
        }

        switch (event.type) {
            case SDL_QUIT:
                return false;
            case SC_EVENT_RUN_ON_MAIN_THREAD: {
                sc_runnable_fn run = event.user.data1;
                void *userdata = event.user.data2;
                run(userdata);
                break;
            }
            default:
                if (has_screen && !sc_screen_handle_event(&s->screen, &event)) {
                    return false;
                }
                break;
        }
    }
}

enum sc_await_server_result {
    SC_AWAIT_SERVER_RESULT_CONNECTED,
    SC_AWAIT_SERVER_RESULT_USER_QUIT,
    SC_AWAIT_SERVER_RESULT_CONNECTION_FAILED,
    SC_AWAIT_SERVER_RESULT_ERROR,
};

static enum sc_await_server_result
await_for_server(struct sc_screen *screen) {
    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return SC_AWAIT_SERVER_RESULT_USER_QUIT;
            case SC_EVENT_SERVER_CONNECTION_FAILED:
                return SC_AWAIT_SERVER_RESULT_CONNECTION_FAILED;
            case SC_EVENT_SERVER_CONNECTED:
                return SC_AWAIT_SERVER_RESULT_CONNECTED;
            default:
                if (screen && !sc_screen_handle_event(screen, &event)) {
                    return SC_AWAIT_SERVER_RESULT_ERROR;
                }
                break;
        }
    }

    LOGE("SDL_WaitEvent() error: %s", SDL_GetError());
    return SC_AWAIT_SERVER_RESULT_ERROR;
}

static void
sc_recorder_on_ended(struct sc_recorder *recorder, bool success,
                     void *userdata) {
    (void) recorder;
    (void) userdata;

    if (!success) {
        sc_push_event(SC_EVENT_RECORDER_ERROR);
    }
}

static void
sc_video_demuxer_on_ended(struct sc_demuxer *demuxer,
                          enum sc_demuxer_status status, void *userdata) {
    (void) demuxer;
    (void) userdata;

    // The device may not decide to disable the video
    assert(status != SC_DEMUXER_STATUS_DISABLED);

    if (status == SC_DEMUXER_STATUS_EOS) {
        sc_push_event(SC_EVENT_DEVICE_DISCONNECTED);
    } else {
        sc_push_event(SC_EVENT_DEMUXER_ERROR);
    }
}

static void
sc_audio_demuxer_on_ended(struct sc_demuxer *demuxer,
                          enum sc_demuxer_status status, void *userdata) {
    (void) demuxer;

    const struct scrcpy_options *options = userdata;

    // Contrary to the video demuxer, keep mirroring if only the audio fails
    // (unless --require-audio is set).
    if (status == SC_DEMUXER_STATUS_EOS) {
        sc_push_event(SC_EVENT_DEVICE_DISCONNECTED);
    } else if (status == SC_DEMUXER_STATUS_ERROR
            || (status == SC_DEMUXER_STATUS_DISABLED
                && options->require_audio)) {
        sc_push_event(SC_EVENT_DEMUXER_ERROR);
    }
}

static void
sc_controller_on_ended(struct sc_controller *controller, bool error,
                       void *userdata) {
    // Note: this function may be called twice, once from the controller thread
    // and once from the receiver thread
    (void) controller;
    (void) userdata;

    if (error) {
        sc_push_event(SC_EVENT_CONTROLLER_ERROR);
    } else {
        sc_push_event(SC_EVENT_DEVICE_DISCONNECTED);
    }
}

static void
sc_server_on_connection_failed(struct sc_server *server, void *userdata) {
    (void) server;
    (void) userdata;

    sc_push_event(SC_EVENT_SERVER_CONNECTION_FAILED);
}

static void
sc_server_on_connected(struct sc_server *server, void *userdata) {
    (void) server;
    (void) userdata;

    sc_push_event(SC_EVENT_SERVER_CONNECTED);
}

static void
sc_server_on_disconnected(struct sc_server *server, void *userdata) {
    (void) server;
    (void) userdata;

    LOGD("Server disconnected");
    // Do nothing, the disconnection will be handled by the "stream stopped"
    // event
}

static void
sc_timeout_on_timeout(struct sc_timeout *timeout, void *userdata) {
    (void) timeout;
    (void) userdata;

    sc_push_event(SC_EVENT_TIME_LIMIT_REACHED);
}

// Generate a scrcpy id to differentiate multiple running scrcpy instances
static uint32_t
scrcpy_generate_scid(void) {
    struct sc_rand rand;
    sc_rand_init(&rand);
    // Only use 31 bits to avoid issues with signed values on the Java-side
    return sc_rand_u32(&rand) & 0x7FFFFFFF;
}

static void
init_sdl_gamepads(void) {
    // Trigger a SDL_CONTROLLERDEVICEADDED event for all gamepads already
    // connected
    int num_joysticks = SDL_NumJoysticks();
    for (int i = 0; i < num_joysticks; ++i) {
        if (SDL_IsGameController(i)) {
            SDL_Event event;
            event.cdevice.type = SDL_CONTROLLERDEVICEADDED;
            event.cdevice.which = i;
            SDL_PushEvent(&event);
        }
    }
}

static void
set_waiting_window_title(struct scrcpy *s, const struct scrcpy_options *options) {
    if (!options->window) {
        return;
    }

    const char *base_title = options->window_title ? options->window_title
                                                    : "scrcpy";
    char title[256];
    int written = snprintf(title, sizeof(title), "%s - waiting for connection",
                           base_title);
    if (written <= 0 || written >= (int) sizeof(title)) {
        sc_screen_set_window_title(&s->screen, "scrcpy - waiting for connection");
    } else {
        sc_screen_set_window_title(&s->screen, title);
    }
}

enum scrcpy_exit_code
scrcpy(struct scrcpy_options *options) {
    static struct scrcpy scrcpy;
#ifndef NDEBUG
    // Detect missing initializations
    memset(&scrcpy, 42, sizeof(scrcpy));
#endif
    struct scrcpy *s = &scrcpy;

    // Minimal SDL initialization
    if (SDL_Init(SDL_INIT_EVENTS)) {
        LOGE("Could not initialize SDL: %s", SDL_GetError());
        return SCRCPY_EXIT_FAILURE;
    }

    atexit(SDL_Quit);

    enum scrcpy_exit_code ret = SCRCPY_EXIT_FAILURE;

    // playback implies capture
    assert(!options->video_playback || options->video);
    assert(!options->audio_playback || options->audio);

    if (options->window) {
        // Set hints before creating the screen to avoid race conditions in SDL.
        sdl_set_hints(options->render_driver);
    }

    if (options->window ||
            (options->control && options->clipboard_autosync)) {
        // Initialize the video subsystem even if --no-video or
        // --no-video-playback is passed so that clipboard synchronization still
        // works.
        // <https://github.com/Genymobile/scrcpy/issues/4418>
        if (SDL_Init(SDL_INIT_VIDEO)) {
            if (options->video_playback) {
                LOGE("Could not initialize SDL video: %s", SDL_GetError());
                return ret;
            }
            LOGW("Could not initialize SDL video: %s", SDL_GetError());
        }
    }

    if (options->audio_playback && SDL_Init(SDL_INIT_AUDIO)) {
        LOGE("Could not initialize SDL audio: %s", SDL_GetError());
        return ret;
    }

    if (options->gamepad_input_mode != SC_GAMEPAD_INPUT_MODE_DISABLED
            && SDL_Init(SDL_INIT_GAMECONTROLLER)) {
        LOGE("Could not initialize SDL gamepad: %s", SDL_GetError());
        return ret;
    }

    sdl_configure(options->video_playback, options->disable_screensaver);

    bool screen_initialized = false;
    if (options->window) {
        struct sc_screen_params screen_params = {
            .video = options->video_playback,
            .controller = NULL,
            .fp = NULL,
            .kp = NULL,
            .mp = NULL,
            .gp = NULL,
            .mouse_bindings = options->mouse_bindings,
            .legacy_paste = options->legacy_paste,
            .clipboard_autosync = options->clipboard_autosync,
            .shortcut_mods = options->shortcut_mods,
            .window_title = options->window_title ? options->window_title
                                                  : "scrcpy",
            .always_on_top = options->always_on_top,
            .window_x = options->window_x,
            .window_y = options->window_y,
            .window_width = options->window_width,
            .window_height = options->window_height,
            .window_borderless = options->window_borderless,
            .orientation = options->display_orientation,
            .mipmaps = options->mipmaps,
            .fullscreen = options->fullscreen,
            .start_fps_counter = options->start_fps_counter,
        };

        if (!sc_screen_init(&s->screen, &screen_params)) {
            return ret;
        }
        screen_initialized = true;
        set_waiting_window_title(s, options);
    }

    static const struct sc_server_callbacks cbs = {
        .on_connection_failed = sc_server_on_connection_failed,
        .on_connected = sc_server_on_connected,
        .on_disconnected = sc_server_on_disconnected,
    };

    for (;;) {
        bool server_started = false;
        bool file_pusher_initialized = false;
        bool recorder_initialized = false;
        bool recorder_started = false;
#ifdef HAVE_V4L2
        bool v4l2_sink_initialized = false;
#endif
        bool video_demuxer_started = false;
        bool audio_demuxer_started = false;
#ifdef HAVE_USB
        bool aoa_hid_initialized = false;
        bool keyboard_aoa_initialized = false;
        bool mouse_aoa_initialized = false;
        bool gamepad_aoa_initialized = false;
#endif
        bool controller_initialized = false;
        bool controller_started = false;
        bool timeout_initialized = false;
        bool timeout_started = false;
        bool secure_monitor_initialized = false;
        bool secure_monitor_started = false;
        bool retry = false;
        bool stop = false;

        struct sc_secure_content_monitor secure_monitor;

        struct sc_acksync *acksync = NULL;
        struct sc_controller *controller = NULL;
        struct sc_key_processor *kp = NULL;
        struct sc_mouse_processor *mp = NULL;
        struct sc_gamepad_processor *gp = NULL;
        struct sc_file_pusher *fp = NULL;

        uint32_t scid = scrcpy_generate_scid();

        struct sc_server_params params = {
            .scid = scid,
            .req_serial = options->serial,
            .select_usb = options->select_usb,
            .select_tcpip = options->select_tcpip,
            .log_level = options->log_level,
            .video_codec = options->video_codec,
            .audio_codec = options->audio_codec,
            .video_source = options->video_source,
            .audio_source = options->audio_source,
            .camera_facing = options->camera_facing,
            .crop = options->crop,
            .port_range = options->port_range,
            .tunnel_host = options->tunnel_host,
            .tunnel_port = options->tunnel_port,
            .max_size = options->max_size,
            .video_bit_rate = options->video_bit_rate,
            .audio_bit_rate = options->audio_bit_rate,
            .max_fps = options->max_fps,
            .angle = options->angle,
            .screen_off_timeout = options->screen_off_timeout,
            .capture_orientation = options->capture_orientation,
            .capture_orientation_lock = options->capture_orientation_lock,
            .control = options->control,
            .display_id = options->display_id,
            .new_display = options->new_display,
            .display_ime_policy = options->display_ime_policy,
            .video = options->video,
            .audio = options->audio,
            .audio_dup = options->audio_dup,
            .show_touches = options->show_touches,
            .stay_awake = options->stay_awake,
            .video_codec_options = options->video_codec_options,
            .audio_codec_options = options->audio_codec_options,
            .video_encoder = options->video_encoder,
            .audio_encoder = options->audio_encoder,
            .camera_id = options->camera_id,
            .camera_size = options->camera_size,
            .camera_ar = options->camera_ar,
            .camera_fps = options->camera_fps,
            .force_adb_forward = options->force_adb_forward,
            .power_off_on_close = options->power_off_on_close,
            .clipboard_autosync = options->clipboard_autosync,
            .downsize_on_error = options->downsize_on_error,
            .tcpip = options->tcpip,
            .tcpip_dst = options->tcpip_dst,
            .cleanup = options->cleanup,
            .power_on = options->power_on,
            .kill_adb_on_close = options->kill_adb_on_close,
            .camera_high_speed = options->camera_high_speed,
            .vd_destroy_content = options->vd_destroy_content,
            .vd_system_decorations = options->vd_system_decorations,
            .list = options->list,
        };

        if (!sc_server_init(&s->server, &params, &cbs, NULL)) {
            ret = SCRCPY_EXIT_FAILURE;
            break;
        }

        if (!sc_server_start(&s->server)) {
            goto session_end;
        }
        server_started = true;

        if (options->list) {
            enum sc_await_server_result res = await_for_server(NULL);
            ret = (res == SC_AWAIT_SERVER_RESULT_CONNECTED
                || res == SC_AWAIT_SERVER_RESULT_USER_QUIT)
                ? SCRCPY_EXIT_SUCCESS
                : SCRCPY_EXIT_FAILURE;
            stop = true;
            goto session_end;
        }

        enum sc_await_server_result await_res =
            await_for_server(screen_initialized ? &s->screen : NULL);
        if (await_res == SC_AWAIT_SERVER_RESULT_CONNECTION_FAILED) {
            if (screen_initialized) {
                set_waiting_window_title(s, options);
                sc_screen_set_connection_state(&s->screen,
                                               SC_SCREEN_CONNECTION_CONNECTING);
                retry = true;
                ret = SCRCPY_EXIT_DISCONNECTED;
            } else {
                ret = SCRCPY_EXIT_FAILURE;
                stop = true;
            }
            goto session_end;
        }

        if (await_res == SC_AWAIT_SERVER_RESULT_USER_QUIT) {
            LOGD("User requested to quit");
            ret = SCRCPY_EXIT_SUCCESS;
            stop = true;
            goto session_end;
        }

        if (await_res != SC_AWAIT_SERVER_RESULT_CONNECTED) {
            ret = SCRCPY_EXIT_FAILURE;
            stop = true;
            goto session_end;
        }

        LOGD("Server connected");

        const char *serial = s->server.serial;
        assert(serial);

        if (screen_initialized && options->video_playback) {
            bool ok = sc_secure_content_monitor_init(&secure_monitor, serial);
            if (ok) {
                secure_monitor_initialized = true;
                ok = sc_secure_content_monitor_start(&secure_monitor);
                if (ok) {
                    secure_monitor_started = true;
                } else {
                    sc_secure_content_monitor_destroy(&secure_monitor);
                    secure_monitor_initialized = false;
                }
            } else {
                LOGW("Could not initialize secure-content monitor");
            }
        }

        if (options->video_playback && options->control) {
            if (!sc_file_pusher_init(&s->file_pusher, serial,
                                     options->push_target)) {
                goto session_end;
            }
            fp = &s->file_pusher;
            file_pusher_initialized = true;
        }

        if (options->video) {
            static const struct sc_demuxer_callbacks video_demuxer_cbs = {
                .on_ended = sc_video_demuxer_on_ended,
            };
            sc_demuxer_init(&s->video_demuxer, "video", s->server.video_socket,
                            &video_demuxer_cbs, NULL);
        }

        if (options->audio) {
            static const struct sc_demuxer_callbacks audio_demuxer_cbs = {
                .on_ended = sc_audio_demuxer_on_ended,
            };
            sc_demuxer_init(&s->audio_demuxer, "audio", s->server.audio_socket,
                            &audio_demuxer_cbs, options);
        }

        bool needs_video_decoder = options->video_playback;
        bool needs_audio_decoder = options->audio_playback;
#ifdef HAVE_V4L2
        needs_video_decoder |= !!options->v4l2_device;
#endif
        if (needs_video_decoder) {
            sc_decoder_init(&s->video_decoder, "video");
            sc_packet_source_add_sink(&s->video_demuxer.packet_source,
                                      &s->video_decoder.packet_sink);
        }
        if (needs_audio_decoder) {
            sc_decoder_init(&s->audio_decoder, "audio");
            sc_packet_source_add_sink(&s->audio_demuxer.packet_source,
                                      &s->audio_decoder.packet_sink);
        }

        if (options->record_filename) {
            static const struct sc_recorder_callbacks recorder_cbs = {
                .on_ended = sc_recorder_on_ended,
            };
            if (!sc_recorder_init(&s->recorder, options->record_filename,
                                  options->record_format, options->video,
                                  options->audio, options->record_orientation,
                                  &recorder_cbs, NULL)) {
                goto session_end;
            }
            recorder_initialized = true;

            if (!sc_recorder_start(&s->recorder)) {
                goto session_end;
            }
            recorder_started = true;

            if (options->video) {
                sc_packet_source_add_sink(&s->video_demuxer.packet_source,
                                          &s->recorder.video_packet_sink);
            }
            if (options->audio) {
                sc_packet_source_add_sink(&s->audio_demuxer.packet_source,
                                          &s->recorder.audio_packet_sink);
            }
        }

        if (options->control) {
            static const struct sc_controller_callbacks controller_cbs = {
                .on_ended = sc_controller_on_ended,
            };

            if (!sc_controller_init(&s->controller, s->server.control_socket,
                &controller_cbs, NULL)) {
                goto session_end;
            }
            controller_initialized = true;

            controller = &s->controller;

#ifdef HAVE_USB
            bool use_keyboard_aoa =
                options->keyboard_input_mode == SC_KEYBOARD_INPUT_MODE_AOA;
            bool use_mouse_aoa =
                options->mouse_input_mode == SC_MOUSE_INPUT_MODE_AOA;
            bool use_gamepad_aoa =
                options->gamepad_input_mode == SC_GAMEPAD_INPUT_MODE_AOA;
            if (use_keyboard_aoa || use_mouse_aoa || use_gamepad_aoa) {
                bool ok = sc_acksync_init(&s->acksync);
                if (!ok) {
                    goto session_end;
                }

                ok = sc_usb_init(&s->usb);
                if (!ok) {
                    LOGE("Failed to initialize USB");
                    sc_acksync_destroy(&s->acksync);
                    goto session_end;
                }

                struct sc_usb_device usb_device;
                ok = sc_usb_select_device(&s->usb, serial, &usb_device);
                if (!ok) {
                    sc_usb_destroy(&s->usb);
                    goto session_end;
                }

                LOGI("USB device: %s (%04" PRIx16 ":%04" PRIx16 ") %s %s",
                     usb_device.serial, usb_device.vid, usb_device.pid,
                     usb_device.manufacturer, usb_device.product);

                ok = sc_usb_connect(&s->usb, usb_device.device, NULL, NULL);
                sc_usb_device_destroy(&usb_device);
                if (!ok) {
                    LOGE("Failed to connect to USB device %s", serial);
                    sc_usb_destroy(&s->usb);
                    sc_acksync_destroy(&s->acksync);
                    goto session_end;
                }

                ok = sc_aoa_init(&s->aoa, &s->usb, &s->acksync);
                if (!ok) {
                    LOGE("Failed to enable HID over AOA");
                    sc_usb_disconnect(&s->usb);
                    sc_usb_destroy(&s->usb);
                    sc_acksync_destroy(&s->acksync);
                    goto session_end;
                }

                bool aoa_fail = false;
                if (use_keyboard_aoa) {
                    if (sc_keyboard_aoa_init(&s->keyboard_aoa, &s->aoa)) {
                        keyboard_aoa_initialized = true;
                        kp = &s->keyboard_aoa.key_processor;
                    } else {
                        LOGE("Could not initialize HID keyboard");
                        aoa_fail = true;
                        goto aoa_complete;
                    }
                }

                if (use_mouse_aoa) {
                    if (sc_mouse_aoa_init(&s->mouse_aoa, &s->aoa)) {
                        mouse_aoa_initialized = true;
                        mp = &s->mouse_aoa.mouse_processor;
                    } else {
                        LOGE("Could not initialized HID mouse");
                        aoa_fail = true;
                        goto aoa_complete;
                    }
                }

                if (use_gamepad_aoa) {
                    sc_gamepad_aoa_init(&s->gamepad_aoa, &s->aoa);
                    gp = &s->gamepad_aoa.gamepad_processor;
                    gamepad_aoa_initialized = true;
                }

aoa_complete:
                if (aoa_fail || !sc_aoa_start(&s->aoa)) {
                    sc_acksync_destroy(&s->acksync);
                    sc_usb_disconnect(&s->usb);
                    sc_usb_destroy(&s->usb);
                    sc_aoa_destroy(&s->aoa);
                    goto session_end;
                }

                acksync = &s->acksync;
                aoa_hid_initialized = true;
            }
#else
            assert(options->keyboard_input_mode != SC_KEYBOARD_INPUT_MODE_AOA);
            assert(options->mouse_input_mode != SC_MOUSE_INPUT_MODE_AOA);
#endif

            struct sc_keyboard_uhid *uhid_keyboard = NULL;

            if (options->keyboard_input_mode == SC_KEYBOARD_INPUT_MODE_SDK) {
                sc_keyboard_sdk_init(&s->keyboard_sdk, &s->controller,
                                     options->key_inject_mode,
                                     options->forward_key_repeat);
                kp = &s->keyboard_sdk.key_processor;
            } else if (options->keyboard_input_mode
                    == SC_KEYBOARD_INPUT_MODE_UHID) {
                bool ok = sc_keyboard_uhid_init(&s->keyboard_uhid,
                                                &s->controller);
                if (!ok) {
                    goto session_end;
                }
                kp = &s->keyboard_uhid.key_processor;
                uhid_keyboard = &s->keyboard_uhid;
            }

            if (options->mouse_input_mode == SC_MOUSE_INPUT_MODE_SDK) {
                sc_mouse_sdk_init(&s->mouse_sdk, &s->controller,
                                  options->mouse_hover);
                mp = &s->mouse_sdk.mouse_processor;
            } else if (options->mouse_input_mode == SC_MOUSE_INPUT_MODE_UHID) {
                bool ok = sc_mouse_uhid_init(&s->mouse_uhid, &s->controller);
                if (!ok) {
                    goto session_end;
                }
                mp = &s->mouse_uhid.mouse_processor;
            }

            if (options->gamepad_input_mode == SC_GAMEPAD_INPUT_MODE_UHID) {
                sc_gamepad_uhid_init(&s->gamepad_uhid, &s->controller);
                gp = &s->gamepad_uhid.gamepad_processor;
            }

            struct sc_uhid_devices *uhid_devices = NULL;
            if (uhid_keyboard) {
                sc_uhid_devices_init(&s->uhid_devices, uhid_keyboard);
                uhid_devices = &s->uhid_devices;
            }

            sc_controller_configure(&s->controller, acksync, uhid_devices);

            if (!sc_controller_start(&s->controller)) {
                goto session_end;
            }
            controller_started = true;
        }

        assert(options->control == !!controller);

        if (screen_initialized) {
            const char *window_title =
                options->window_title ? options->window_title
                                      : s->server.info.device_name;
            sc_screen_set_window_title(&s->screen, window_title);
            sc_screen_set_input_processors(&s->screen, controller, fp, kp, mp,
                                           gp);
            sc_screen_set_connection_state(&s->screen,
                                           SC_SCREEN_CONNECTION_RUNNING);

            if (options->video_playback) {
                struct sc_frame_source *src = &s->video_decoder.frame_source;
                if (options->video_buffer) {
                    sc_delay_buffer_init(&s->video_buffer,
                                         options->video_buffer, true);
                    sc_frame_source_add_sink(src, &s->video_buffer.frame_sink);
                    src = &s->video_buffer.frame_source;
                }

                sc_frame_source_add_sink(src, &s->screen.frame_sink);
            }
        }

        if (options->audio_playback) {
            sc_audio_player_init(&s->audio_player, options->audio_buffer,
                                 options->audio_output_buffer);
            sc_frame_source_add_sink(&s->audio_decoder.frame_source,
                                     &s->audio_player.frame_sink);
        }

#ifdef HAVE_V4L2
        if (options->v4l2_device) {
            if (!sc_v4l2_sink_init(&s->v4l2_sink, options->v4l2_device)) {
                goto session_end;
            }

            struct sc_frame_source *src = &s->video_decoder.frame_source;
            if (options->v4l2_buffer) {
                sc_delay_buffer_init(&s->v4l2_buffer, options->v4l2_buffer,
                                     true);
                sc_frame_source_add_sink(src, &s->v4l2_buffer.frame_sink);
                src = &s->v4l2_buffer.frame_source;
            }

            sc_frame_source_add_sink(src, &s->v4l2_sink.frame_sink);

            v4l2_sink_initialized = true;
        }
#endif

        if (options->video) {
            if (!sc_demuxer_start(&s->video_demuxer)) {
                goto session_end;
            }
            video_demuxer_started = true;
        }

        if (options->audio) {
            if (!sc_demuxer_start(&s->audio_demuxer)) {
                goto session_end;
            }
            audio_demuxer_started = true;
        }

        if (options->control && options->turn_screen_off) {
            struct sc_control_msg msg;
            msg.type = SC_CONTROL_MSG_TYPE_SET_DISPLAY_POWER;
            msg.set_display_power.on = false;

            if (!sc_controller_push_msg(&s->controller, &msg)) {
                LOGW("Could not request 'set display power'");
            }
        }

        if (options->time_limit) {
            bool ok = sc_timeout_init(&s->timeout);
            if (!ok) {
                goto session_end;
            }

            timeout_initialized = true;

            sc_tick deadline = sc_tick_now() + options->time_limit;
            static const struct sc_timeout_callbacks timeout_cbs = {
                .on_timeout = sc_timeout_on_timeout,
            };

            ok = sc_timeout_start(&s->timeout, deadline, &timeout_cbs, NULL);
            if (!ok) {
                goto session_end;
            }

            timeout_started = true;
        }

        if (options->control
                && options->gamepad_input_mode
                        != SC_GAMEPAD_INPUT_MODE_DISABLED) {
            init_sdl_gamepads();
        }

        if (options->control && options->start_app) {
            assert(controller);

            char *name = strdup(options->start_app);
            if (!name) {
                LOG_OOM();
                goto session_end;
            }

            struct sc_control_msg msg;
            msg.type = SC_CONTROL_MSG_TYPE_START_APP;
            msg.start_app.name = name;

            if (!sc_controller_push_msg(controller, &msg)) {
                LOGW("Could not request start app '%s'", name);
                free(name);
            }
        }

        ret = event_loop(s, screen_initialized);
        terminate_event_loop();

        if (ret == SCRCPY_EXIT_DISCONNECTED && screen_initialized) {
            retry = true;
        } else {
            stop = true;
            if (screen_initialized && options->video_playback) {
                sc_screen_hide_window(&s->screen);
            }
        }

session_end:
        if (secure_monitor_started) {
            sc_secure_content_monitor_stop(&secure_monitor);
        }

        if (timeout_started) {
            sc_timeout_stop(&s->timeout);
        }

#ifdef HAVE_USB
        if (aoa_hid_initialized) {
            if (keyboard_aoa_initialized) {
                sc_keyboard_aoa_destroy(&s->keyboard_aoa);
            }
            if (mouse_aoa_initialized) {
                sc_mouse_aoa_destroy(&s->mouse_aoa);
            }
            if (gamepad_aoa_initialized) {
                sc_gamepad_aoa_destroy(&s->gamepad_aoa);
            }
            sc_aoa_stop(&s->aoa);
            sc_usb_stop(&s->usb);
        }
        if (acksync) {
            sc_acksync_destroy(acksync);
        }
#endif
        if (controller_started) {
            sc_controller_stop(&s->controller);
        }
        if (file_pusher_initialized) {
            sc_file_pusher_stop(&s->file_pusher);
        }
        if (recorder_initialized) {
            sc_recorder_stop(&s->recorder);
        }

        if (screen_initialized) {
            sc_screen_set_input_processors(&s->screen, NULL, NULL, NULL, NULL,
                                           NULL);
            if (!retry) {
                sc_screen_interrupt(&s->screen);
            } else {
                set_waiting_window_title(s, options);
                sc_screen_set_connection_state(&s->screen,
                                               SC_SCREEN_CONNECTION_CONNECTING);
            }
        }

        if (server_started) {
            sc_server_stop(&s->server);
        }

        if (timeout_started) {
            sc_timeout_join(&s->timeout);
        }
        if (timeout_initialized) {
            sc_timeout_destroy(&s->timeout);
        }

        if (secure_monitor_started) {
            sc_secure_content_monitor_join(&secure_monitor);
        }
        if (secure_monitor_initialized) {
            sc_secure_content_monitor_destroy(&secure_monitor);
        }

        if (video_demuxer_started) {
            sc_demuxer_join(&s->video_demuxer);
        }

        if (audio_demuxer_started) {
            sc_demuxer_join(&s->audio_demuxer);
        }

#ifdef HAVE_V4L2
        if (v4l2_sink_initialized) {
            sc_v4l2_sink_destroy(&s->v4l2_sink);
        }
#endif

#ifdef HAVE_USB
        if (aoa_hid_initialized) {
            sc_aoa_join(&s->aoa);
            sc_aoa_destroy(&s->aoa);
            sc_usb_join(&s->usb);
            sc_usb_disconnect(&s->usb);
            sc_usb_destroy(&s->usb);
        }
#endif

        if (controller_started) {
            sc_controller_join(&s->controller);
        }
        if (controller_initialized) {
            sc_controller_destroy(&s->controller);
        }

        if (recorder_started) {
            sc_recorder_join(&s->recorder);
        }
        if (recorder_initialized) {
            sc_recorder_destroy(&s->recorder);
        }

        if (file_pusher_initialized) {
            sc_file_pusher_join(&s->file_pusher);
            sc_file_pusher_destroy(&s->file_pusher);
        }

        if (server_started) {
            sc_server_join(&s->server);
        }
        sc_server_destroy(&s->server);

        if (stop || !retry) {
            break;
        }

        if (!wait_retry_delay(s, screen_initialized, 1000)) {
            ret = SCRCPY_EXIT_SUCCESS;
            break;
        }
    }

    if (screen_initialized) {
        sc_screen_join(&s->screen);
        sc_screen_destroy(&s->screen);
    }

    return ret;
}
