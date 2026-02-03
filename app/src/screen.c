#include "screen.h"

#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "events.h"
#include "icon.h"
#include "options.h"
#include "util/log.h"
#ifdef __APPLE__
# include "sys/darwin/clipboard.h"
#endif

#define DISPLAY_MARGINS 96
#define UI_PANEL_WIDTH 220
#define UI_MARGIN 16
#define UI_BUTTON_HEIGHT 52
#define UI_PLACEHOLDER_THICKNESS 8
#define UI_SCREENSHOT_LABEL "COPY SCREENSHOT"

#define DOWNCAST(SINK) container_of(SINK, struct sc_screen, frame_sink)

static void
sc_screen_show_idle_window(struct sc_screen *screen);

static inline struct sc_size
get_oriented_size(struct sc_size size, enum sc_orientation orientation) {
    struct sc_size oriented_size;
    if (sc_orientation_is_swap(orientation)) {
        oriented_size.width = size.height;
        oriented_size.height = size.width;
    } else {
        oriented_size.width = size.width;
        oriented_size.height = size.height;
    }
    return oriented_size;
}

// get the window size in a struct sc_size
static struct sc_size
get_window_size(const struct sc_screen *screen) {
    int width;
    int height;
    SDL_GetWindowSize(screen->window, &width, &height);

    struct sc_size size;
    size.width = width;
    size.height = height;
    return size;
}

static struct sc_point
get_window_position(const struct sc_screen *screen) {
    int x;
    int y;
    SDL_GetWindowPosition(screen->window, &x, &y);

    struct sc_point point;
    point.x = x;
    point.y = y;
    return point;
}

static struct sc_size
get_drawable_size(const struct sc_screen *screen) {
    int dw;
    int dh;
    SDL_GL_GetDrawableSize(screen->window, &dw, &dh);

    struct sc_size size;
    size.width = dw;
    size.height = dh;
    return size;
}

static int
scale_window_to_drawable(const struct sc_screen *screen, int value,
                         bool x_axis) {
    int ww;
    int wh;
    SDL_GetWindowSize(screen->window, &ww, &wh);

    struct sc_size drawable_size = get_drawable_size(screen);
    int window_size = x_axis ? ww : wh;
    int drawable_axis = x_axis ? drawable_size.width : drawable_size.height;
    if (window_size <= 0) {
        return value;
    }

    return (int64_t) value * drawable_axis / window_size;
}

static inline bool
point_in_rect(int32_t x, int32_t y, const SDL_Rect *rect) {
    return x >= rect->x && x < rect->x + rect->w
        && y >= rect->y && y < rect->y + rect->h;
}

// set the window size to be applied when fullscreen is disabled
static void
set_window_size(struct sc_screen *screen, struct sc_size new_size) {
    assert(!screen->fullscreen);
    assert(!screen->maximized);
    assert(!screen->minimized);
    SDL_SetWindowSize(screen->window, new_size.width, new_size.height);
}

// get the preferred display bounds (i.e. the screen bounds with some margins)
static bool
get_preferred_display_bounds(struct sc_size *bounds) {
    SDL_Rect rect;
    if (SDL_GetDisplayUsableBounds(0, &rect)) {
        LOGW("Could not get display usable bounds: %s", SDL_GetError());
        return false;
    }

    bounds->width = MAX(0, rect.w - DISPLAY_MARGINS);
    bounds->height = MAX(0, rect.h - DISPLAY_MARGINS);
    return true;
}

static bool
is_optimal_size(struct sc_size current_size, struct sc_size content_size) {
    // The size is optimal if we can recompute one dimension of the current
    // size from the other
    return current_size.height == current_size.width * content_size.height
                                                     / content_size.width
        || current_size.width == current_size.height * content_size.width
                                                     / content_size.height;
}

// return the optimal size of the window, with the following constraints:
//  - it attempts to keep at least one dimension of the current_size (i.e. it
//    crops the black borders)
//  - it keeps the aspect ratio
//  - it scales down to make it fit in the display_size
static struct sc_size
get_optimal_size(struct sc_size current_size, struct sc_size content_size,
                 bool within_display_bounds) {
    if (content_size.width == 0 || content_size.height == 0) {
        // avoid division by 0
        return current_size;
    }

    struct sc_size window_size;

    struct sc_size display_size;
    if (!within_display_bounds ||
            !get_preferred_display_bounds(&display_size)) {
        // do not constraint the size
        window_size = current_size;
    } else {
        window_size.width = MIN(current_size.width, display_size.width);
        window_size.height = MIN(current_size.height, display_size.height);
    }

    if (is_optimal_size(window_size, content_size)) {
        return window_size;
    }

    bool keep_width = content_size.width * window_size.height
                    > content_size.height * window_size.width;
    if (keep_width) {
        // remove black borders on top and bottom
        window_size.height = content_size.height * window_size.width
                           / content_size.width;
    } else {
        // remove black borders on left and right (or none at all if it already
        // fits)
        window_size.width = content_size.width * window_size.height
                          / content_size.height;
    }

    return window_size;
}

// initially, there is no current size, so use the frame size as current size
// req_width and req_height, if not 0, are the sizes requested by the user
static inline struct sc_size
get_initial_optimal_size(struct sc_size content_size, uint16_t req_width,
                         uint16_t req_height) {
    struct sc_size window_size;
    if (!req_width && !req_height) {
        window_size = get_optimal_size(content_size, content_size, true);
    } else {
        if (req_width) {
            window_size.width = req_width;
        } else {
            // compute from the requested height
            window_size.width = (uint32_t) req_height * content_size.width
                              / content_size.height;
        }
        if (req_height) {
            window_size.height = req_height;
        } else {
            // compute from the requested width
            window_size.height = (uint32_t) req_width * content_size.height
                               / content_size.width;
        }
    }
    return window_size;
}

static inline bool
sc_screen_is_relative_mode(struct sc_screen *screen) {
    // screen->im.mp may be NULL if --no-control
    return screen->im.mp && screen->im.mp->relative_mode;
}

static void
sc_screen_update_ui_rects(struct sc_screen *screen) {
    struct sc_size drawable_size = get_drawable_size(screen);
    int panel_width = scale_window_to_drawable(screen, UI_PANEL_WIDTH, true);
    panel_width = CLAMP(panel_width, 0, drawable_size.width);

    screen->panel_rect.x = drawable_size.width - panel_width;
    screen->panel_rect.y = 0;
    screen->panel_rect.w = panel_width;
    screen->panel_rect.h = drawable_size.height;

    int margin = scale_window_to_drawable(screen, UI_MARGIN, true);
    int button_height =
        scale_window_to_drawable(screen, UI_BUTTON_HEIGHT, false);
    button_height = MAX(button_height, 28);

    int button_width = panel_width - 2 * margin;
    button_width = MAX(button_width, 0);

    screen->screenshot_button_rect.x = screen->panel_rect.x + margin;
    screen->screenshot_button_rect.y = margin;
    screen->screenshot_button_rect.w = button_width;
    screen->screenshot_button_rect.h = button_height;
}

static void
sc_screen_update_content_rect(struct sc_screen *screen) {
    assert(screen->video);

    struct sc_size drawable_size = get_drawable_size(screen);
    sc_screen_update_ui_rects(screen);

    struct sc_size content_size = screen->content_size;
    struct sc_size video_viewport = {
        .width = MAX(0, drawable_size.width - screen->panel_rect.w),
        .height = drawable_size.height,
    };

    SDL_Rect *rect = &screen->rect;

    if (!video_viewport.width || !video_viewport.height) {
        rect->x = 0;
        rect->y = 0;
        rect->w = 0;
        rect->h = 0;
        return;
    }

    if (is_optimal_size(video_viewport, content_size)) {
        rect->x = 0;
        rect->y = 0;
        rect->w = video_viewport.width;
        rect->h = video_viewport.height;
        return;
    }

    bool keep_width = content_size.width * video_viewport.height
                    > content_size.height * video_viewport.width;
    if (keep_width) {
        rect->x = 0;
        rect->w = video_viewport.width;
        rect->h = video_viewport.width * content_size.height
                                      / content_size.width;
        rect->y = (video_viewport.height - rect->h) / 2;
    } else {
        rect->y = 0;
        rect->h = video_viewport.height;
        rect->w = video_viewport.height * content_size.width
                                       / content_size.height;
        rect->x = (video_viewport.width - rect->w) / 2;
    }
}

// render the texture to the renderer
//
// Set the update_content_rect flag if the window or content size may have
// changed, so that the content rectangle is recomputed
static void
sc_screen_draw_idle_placeholder(struct sc_screen *screen) {
    SDL_Renderer *renderer = screen->display.renderer;

    int viewport_width = screen->panel_rect.x;
    int viewport_height = screen->panel_rect.h;
    if (viewport_width <= 0 || viewport_height <= 0) {
        return;
    }

    int min_side = MIN(viewport_width, viewport_height);
    int phone_width = min_side * 2 / 5;
    int phone_height = phone_width * 16 / 9;
    if (phone_height > viewport_height * 4 / 5) {
        phone_height = viewport_height * 4 / 5;
        phone_width = phone_height * 9 / 16;
    }

    int cx = viewport_width / 2;
    int cy = viewport_height / 2;

    SDL_Rect phone = {
        .x = cx - phone_width / 2,
        .y = cy - phone_height / 2,
        .w = phone_width,
        .h = phone_height,
    };

    uint8_t border_r = 95;
    uint8_t border_g = 108;
    uint8_t border_b = 120;
    if (screen->connection_state == SC_SCREEN_CONNECTION_FAILED) {
        border_r = 160;
        border_g = 84;
        border_b = 84;
    } else if (screen->connection_state == SC_SCREEN_CONNECTION_DISCONNECTED) {
        border_r = 167;
        border_g = 115;
        border_b = 72;
    } else if (screen->connection_state == SC_SCREEN_CONNECTION_RUNNING) {
        border_r = 81;
        border_g = 130;
        border_b = 178;
    }

    SDL_SetRenderDrawColor(renderer, border_r, border_g, border_b, 255);
    SDL_RenderDrawRect(renderer, &phone);

    int thickness =
        MAX(2, scale_window_to_drawable(screen, UI_PLACEHOLDER_THICKNESS,
                                        true));
    SDL_Rect inner = {
        .x = phone.x + thickness,
        .y = phone.y + thickness,
        .w = MAX(0, phone.w - 2 * thickness),
        .h = MAX(0, phone.h - 2 * thickness),
    };

    SDL_SetRenderDrawColor(renderer, 29, 35, 42, 255);
    SDL_RenderFillRect(renderer, &inner);

    SDL_Rect sensor = {
        .x = phone.x + phone.w / 2 - phone.w / 8,
        .y = phone.y + thickness,
        .w = phone.w / 4,
        .h = MAX(2, thickness / 2),
    };
    SDL_SetRenderDrawColor(renderer, border_r, border_g, border_b, 255);
    SDL_RenderFillRect(renderer, &sensor);
}

static const uint8_t *
sc_screen_get_button_glyph(char c) {
    static const uint8_t glyph_space[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t glyph_c[7] = {
        0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E,
    };
    static const uint8_t glyph_e[7] = {
        0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F,
    };
    static const uint8_t glyph_h[7] = {
        0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11,
    };
    static const uint8_t glyph_n[7] = {
        0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11,
    };
    static const uint8_t glyph_o[7] = {
        0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E,
    };
    static const uint8_t glyph_p[7] = {
        0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10,
    };
    static const uint8_t glyph_r[7] = {
        0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11,
    };
    static const uint8_t glyph_s[7] = {
        0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E,
    };
    static const uint8_t glyph_t[7] = {
        0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    };
    static const uint8_t glyph_y[7] = {
        0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04,
    };

    switch (toupper((unsigned char) c)) {
        case ' ':
            return glyph_space;
        case 'C':
            return glyph_c;
        case 'E':
            return glyph_e;
        case 'H':
            return glyph_h;
        case 'N':
            return glyph_n;
        case 'O':
            return glyph_o;
        case 'P':
            return glyph_p;
        case 'R':
            return glyph_r;
        case 'S':
            return glyph_s;
        case 'T':
            return glyph_t;
        case 'Y':
            return glyph_y;
        default:
            return glyph_space;
    }
}

static void
sc_screen_draw_button_label(SDL_Renderer *renderer, const SDL_Rect *button,
                            bool enabled) {
    const char *label = UI_SCREENSHOT_LABEL;
    size_t len = strlen(label);
    if (!len || !button->w || !button->h) {
        return;
    }

    int padding = MAX(2, button->h / 8);
    int max_scale_w =
        (button->w - 2 * padding) / (int) (len * 5 + (len - 1));
    int max_scale_h = (button->h - 2 * padding) / 7;
    int scale = MAX(1, MIN(max_scale_w, max_scale_h));

    int glyph_width = 5 * scale;
    int spacing = scale;
    int text_width = (int) len * glyph_width + (int) (len - 1) * spacing;
    int text_height = 7 * scale;
    int start_x = button->x + (button->w - text_width) / 2;
    int start_y = button->y + (button->h - text_height) / 2;

    if (enabled) {
        SDL_SetRenderDrawColor(renderer, 239, 246, 255, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 216, 224, 232, 255);
    }

    int x = start_x;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t *rows = sc_screen_get_button_glyph(label[i]);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (!(rows[row] & (1 << (4 - col)))) {
                    continue;
                }
                SDL_Rect pixel = {
                    .x = x + col * scale,
                    .y = start_y + row * scale,
                    .w = scale,
                    .h = scale,
                };
                SDL_RenderFillRect(renderer, &pixel);
            }
        }
        x += glyph_width + spacing;
    }
}

static void
sc_screen_draw_panel(struct sc_screen *screen) {
    SDL_Renderer *renderer = screen->display.renderer;

    SDL_SetRenderDrawColor(renderer, 20, 23, 27, 255);
    SDL_RenderFillRect(renderer, &screen->panel_rect);

    int margin = scale_window_to_drawable(screen, UI_MARGIN, true);
    SDL_Rect separator = {
        .x = MAX(0, screen->panel_rect.x - 1),
        .y = 0,
        .w = 1,
        .h = screen->panel_rect.h,
    };
    SDL_SetRenderDrawColor(renderer, 44, 50, 58, 255);
    SDL_RenderFillRect(renderer, &separator);

    SDL_Rect button = screen->screenshot_button_rect;
    bool enabled = screen->has_frame;

    uint8_t r = 82;
    uint8_t g = 98;
    uint8_t b = 115;
    if (enabled) {
        r = 40;
        g = 122;
        b = 199;
        if (screen->screenshot_button_pressed) {
            r = 33;
            g = 104;
            b = 170;
        } else if (screen->screenshot_button_hovered) {
            r = 56;
            g = 140;
            b = 216;
        }
    }

    SDL_SetRenderDrawColor(renderer, r, g, b, 255);
    SDL_RenderFillRect(renderer, &button);
    SDL_SetRenderDrawColor(renderer, 28, 32, 37, 255);
    SDL_RenderDrawRect(renderer, &button);
    sc_screen_draw_button_label(renderer, &button, enabled);

    SDL_Rect status = {
        .x = screen->panel_rect.x + margin,
        .y = button.y + button.h + margin,
        .w = MAX(0, screen->panel_rect.w - 2 * margin),
        .h = MAX(3, margin / 2),
    };

    uint8_t status_r = 194;
    uint8_t status_g = 170;
    uint8_t status_b = 75;
    switch (screen->connection_state) {
        case SC_SCREEN_CONNECTION_RUNNING:
            status_r = 128;
            status_g = 136;
            status_b = 146;
            break;
        case SC_SCREEN_CONNECTION_DISCONNECTED:
            status_r = 192;
            status_g = 131;
            status_b = 77;
            break;
        case SC_SCREEN_CONNECTION_FAILED:
            status_r = 186;
            status_g = 87;
            status_b = 87;
            break;
        case SC_SCREEN_CONNECTION_CONNECTING:
            break;
    }
    SDL_SetRenderDrawColor(renderer, status_r, status_g, status_b, 255);
    SDL_RenderFillRect(renderer, &status);
}

static enum sc_display_result
sc_screen_draw_video(struct sc_screen *screen, bool update_content_rect) {
    assert(screen->video);

    if (update_content_rect) {
        sc_screen_update_content_rect(screen);
    }

    enum sc_display_result res = sc_display_render(&screen->display,
                                                   &screen->rect,
                                                   screen->orientation);
    if (res == SC_DISPLAY_RESULT_OK) {
        sc_screen_draw_panel(screen);
    }
    return res;
}

static void
sc_screen_render(struct sc_screen *screen, bool update_content_rect) {
    enum sc_display_result res = sc_screen_draw_video(screen,
                                                      update_content_rect);
    if (res == SC_DISPLAY_RESULT_OK) {
        sc_display_present(&screen->display);
    }
    (void) res; // any error already logged
}

static void
sc_screen_render_idle(struct sc_screen *screen) {
    assert(screen->video);

    sc_screen_update_ui_rects(screen);

    SDL_Renderer *renderer = screen->display.renderer;
    SDL_SetRenderDrawColor(renderer, 17, 20, 24, 255);
    SDL_RenderClear(renderer);
    sc_screen_draw_idle_placeholder(screen);
    sc_screen_draw_panel(screen);
    sc_display_present(&screen->display);
}

static void
sc_screen_render_novideo(struct sc_screen *screen) {
    enum sc_display_result res =
        sc_display_render(&screen->display, NULL, SC_ORIENTATION_0);
    if (res == SC_DISPLAY_RESULT_OK) {
        sc_display_present(&screen->display);
    }
    (void) res; // any error already logged
}

static void
sc_screen_render_current_state(struct sc_screen *screen, bool update_content) {
    if (!screen->video) {
        sc_screen_render_novideo(screen);
        return;
    }

    if (screen->has_frame) {
        sc_screen_render(screen, update_content);
    } else {
        sc_screen_render_idle(screen);
    }
}

static bool
sc_screen_copy_screenshot_to_clipboard(struct sc_screen *screen) {
    assert(screen->video);

    if (!screen->has_frame || !screen->rect.w || !screen->rect.h) {
        LOGW("No video frame available to capture");
        return false;
    }

    enum sc_display_result res = sc_screen_draw_video(screen, false);
    if (res != SC_DISPLAY_RESULT_OK) {
        LOGW("Could not prepare screenshot frame");
        return false;
    }

    int width = screen->rect.w;
    int height = screen->rect.h;
    if (width <= 0 || height <= 0) {
        sc_display_present(&screen->display);
        LOGW("Invalid screenshot size");
        return false;
    }

    if ((size_t) width > SIZE_MAX / 4) {
        sc_display_present(&screen->display);
        LOGW("Screenshot size is too large");
        return false;
    }

    size_t pitch = (size_t) width * 4;
    if ((size_t) height > SIZE_MAX / pitch) {
        sc_display_present(&screen->display);
        LOGW("Screenshot buffer is too large");
        return false;
    }

    size_t size = (size_t) height * pitch;
    uint8_t *pixels = malloc(size);
    if (!pixels) {
        sc_display_present(&screen->display);
        LOG_OOM();
        return false;
    }

    int ret = SDL_RenderReadPixels(screen->display.renderer, &screen->rect,
                                   SDL_PIXELFORMAT_ABGR8888, pixels, pitch);
    bool ok = false;
    if (ret) {
        LOGW("Could not read pixels for screenshot: %s", SDL_GetError());
    } else {
#ifdef __APPLE__
        ok = sc_darwin_clipboard_set_image_rgba8888(pixels, pitch, width,
                                                    height);
        if (!ok) {
            LOGW("Could not copy screenshot image to the macOS clipboard");
        }
#else
        LOGW("Screenshot clipboard image is only implemented on macOS");
#endif
    }

    free(pixels);
    sc_display_present(&screen->display);

    if (ok) {
        LOGI("Screenshot copied to clipboard");
    }
    return ok;
}

static void
sc_screen_flash_screenshot_feedback(struct sc_screen *screen) {
    struct sc_size drawable_size = get_drawable_size(screen);
    if (!drawable_size.width || !drawable_size.height) {
        return;
    }

    SDL_Renderer *renderer = screen->display.renderer;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);

    SDL_Rect flash = {
        .x = 0,
        .y = 0,
        .w = drawable_size.width,
        .h = drawable_size.height,
    };
    SDL_RenderFillRect(renderer, &flash);
    sc_display_present(&screen->display);

    SDL_Delay(500);

    sc_screen_render_current_state(screen, false);
}

static bool
sc_screen_handle_panel_event(struct sc_screen *screen, const SDL_Event *event) {
    assert(screen->video);

    sc_screen_update_ui_rects(screen);
    if (!screen->panel_rect.w) {
        return false;
    }

    switch (event->type) {
        case SDL_MOUSEMOTION: {
            int32_t x = event->motion.x;
            int32_t y = event->motion.y;
            sc_screen_hidpi_scale_coords(screen, &x, &y);
            bool in_button = screen->has_frame
                          && point_in_rect(x, y, &screen->screenshot_button_rect);
            bool in_panel = point_in_rect(x, y, &screen->panel_rect);

            if (in_button != screen->screenshot_button_hovered) {
                screen->screenshot_button_hovered = in_button;
                if (!in_button) {
                    screen->screenshot_button_pressed = false;
                }
                sc_screen_render_current_state(screen, false);
            }
            return in_panel;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int32_t x = event->button.x;
            int32_t y = event->button.y;
            sc_screen_hidpi_scale_coords(screen, &x, &y);
            bool in_panel = point_in_rect(x, y, &screen->panel_rect);
            bool in_button = screen->has_frame
                          && point_in_rect(x, y, &screen->screenshot_button_rect);

            if (event->button.button == SDL_BUTTON_LEFT) {
                bool down = event->type == SDL_MOUSEBUTTONDOWN;
                if (down && in_button) {
                    screen->screenshot_button_pressed = true;
                    sc_screen_render_current_state(screen, false);
                    return true;
                }

                if (!down && screen->screenshot_button_pressed) {
                    bool activate = in_button;
                    screen->screenshot_button_pressed = false;
                    sc_screen_render_current_state(screen, false);
                    if (activate) {
                        sc_screen_copy_screenshot_to_clipboard(screen);
                        sc_screen_flash_screenshot_feedback(screen);
                    }
                    return true;
                }
            }

            return in_panel;
        }
        case SDL_MOUSEWHEEL: {
            int32_t x;
            int32_t y;
            SDL_GetMouseState(&x, &y);
            sc_screen_hidpi_scale_coords(screen, &x, &y);
            return point_in_rect(x, y, &screen->panel_rect);
        }
        default:
            return false;
    }
}

#if defined(__APPLE__) || defined(_WIN32)
# define CONTINUOUS_RESIZING_WORKAROUND
#endif

#ifdef CONTINUOUS_RESIZING_WORKAROUND
// On Windows and MacOS, resizing blocks the event loop, so resizing events are
// not triggered. As a workaround, handle them in an event handler.
//
// <https://bugzilla.libsdl.org/show_bug.cgi?id=2077>
// <https://stackoverflow.com/a/40693139/1987178>
static int
event_watcher(void *data, SDL_Event *event) {
    struct sc_screen *screen = data;
    assert(screen->video);

    if (event->type == SDL_WINDOWEVENT
            && event->window.event == SDL_WINDOWEVENT_RESIZED) {
        // In practice, it seems to always be called from the same thread in
        // that specific case. Anyway, it's just a workaround.
        sc_screen_render_current_state(screen, true);
    }
    return 0;
}
#endif

static bool
sc_screen_frame_sink_open(struct sc_frame_sink *sink,
                          const AVCodecContext *ctx) {
    assert(ctx->pix_fmt == AV_PIX_FMT_YUV420P);
    (void) ctx;

    struct sc_screen *screen = DOWNCAST(sink);

    if (ctx->width <= 0 || ctx->width > 0xFFFF
            || ctx->height <= 0 || ctx->height > 0xFFFF) {
        LOGE("Invalid video size: %dx%d", ctx->width, ctx->height);
        return false;
    }

    assert(ctx->width > 0 && ctx->width <= 0xFFFF);
    assert(ctx->height > 0 && ctx->height <= 0xFFFF);
    // screen->frame_size is never used before the event is pushed, and the
    // event acts as a memory barrier so it is safe without mutex
    screen->frame_size.width = ctx->width;
    screen->frame_size.height = ctx->height;

    // Post the event on the UI thread (the texture must be created from there)
    bool ok = sc_push_event(SC_EVENT_SCREEN_INIT_SIZE);
    if (!ok) {
        return false;
    }

#ifndef NDEBUG
    screen->open = true;
#endif

    // nothing to do, the screen is already open on the main thread
    return true;
}

static void
sc_screen_frame_sink_close(struct sc_frame_sink *sink) {
    struct sc_screen *screen = DOWNCAST(sink);
    (void) screen;
#ifndef NDEBUG
    screen->open = false;
#endif

    // nothing to do, the screen lifecycle is not managed by the frame producer
}

static bool
sc_screen_frame_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_screen *screen = DOWNCAST(sink);
    assert(screen->video);

    bool previous_skipped;
    bool ok = sc_frame_buffer_push(&screen->fb, frame, &previous_skipped);
    if (!ok) {
        return false;
    }

    if (previous_skipped) {
        sc_fps_counter_add_skipped_frame(&screen->fps_counter);
        // The SC_EVENT_NEW_FRAME triggered for the previous frame will consume
        // this new frame instead
    } else {
        // Post the event on the UI thread
        bool ok = sc_push_event(SC_EVENT_NEW_FRAME);
        if (!ok) {
            return false;
        }
    }

    return true;
}

bool
sc_screen_init(struct sc_screen *screen,
               const struct sc_screen_params *params) {
    screen->resize_pending = false;
    screen->has_frame = false;
    screen->frame_size = (struct sc_size) {0, 0};
    screen->content_size = (struct sc_size) {0, 0};
    screen->rect = (SDL_Rect) {0, 0, 0, 0};
    screen->panel_rect = (SDL_Rect) {0, 0, 0, 0};
    screen->screenshot_button_rect = (SDL_Rect) {0, 0, 0, 0};
    screen->screenshot_button_hovered = false;
    screen->screenshot_button_pressed = false;
    screen->connection_state = SC_SCREEN_CONNECTION_CONNECTING;
    screen->fullscreen = false;
    screen->maximized = false;
    screen->minimized = false;
    screen->paused = false;
    screen->resume_frame = NULL;
    screen->orientation = SC_ORIENTATION_0;

    screen->video = params->video;

    screen->req.x = params->window_x;
    screen->req.y = params->window_y;
    screen->req.width = params->window_width;
    screen->req.height = params->window_height;
    screen->req.fullscreen = params->fullscreen;
    screen->req.start_fps_counter = params->start_fps_counter;

    bool ok = sc_frame_buffer_init(&screen->fb);
    if (!ok) {
        return false;
    }

    if (!sc_fps_counter_init(&screen->fps_counter)) {
        goto error_destroy_frame_buffer;
    }

    if (screen->video) {
        screen->orientation = params->orientation;
        if (screen->orientation != SC_ORIENTATION_0) {
            LOGI("Initial display orientation set to %s",
                 sc_orientation_get_name(screen->orientation));
        }
    }

    uint32_t window_flags = SDL_WINDOW_ALLOW_HIGHDPI;
    if (params->always_on_top) {
        window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (params->window_borderless) {
        window_flags |= SDL_WINDOW_BORDERLESS;
    }
    if (params->video) {
        // Show it once initialized in idle mode
        window_flags |= SDL_WINDOW_HIDDEN
                      | SDL_WINDOW_RESIZABLE;
    }

    const char *title = params->window_title;
    assert(title);

    int x = SDL_WINDOWPOS_UNDEFINED;
    int y = SDL_WINDOWPOS_UNDEFINED;
    int width = params->video ? 1024 : 256;
    int height = params->video ? 640 : 256;
    if (params->window_x != SC_WINDOW_POSITION_UNDEFINED) {
        x = params->window_x;
    }
    if (params->window_y != SC_WINDOW_POSITION_UNDEFINED) {
        y = params->window_y;
    }
    if (params->window_width) {
        width = params->window_width;
    }
    if (params->window_height) {
        height = params->window_height;
    }

    screen->window = SDL_CreateWindow(title, x, y, width, height, window_flags);
    if (!screen->window) {
        LOGE("Could not create window: %s", SDL_GetError());
        goto error_destroy_fps_counter;
    }

    SDL_Surface *icon = scrcpy_icon_load();
    if (icon) {
        SDL_SetWindowIcon(screen->window, icon);
    } else if (params->video) {
        // just a warning
        LOGW("Could not load icon");
    } else {
        // without video, the icon is used as window content, it must be present
        LOGE("Could not load icon");
        goto error_destroy_window;
    }

    SDL_Surface *icon_novideo = params->video ? NULL : icon;
    bool mipmaps = params->video && params->mipmaps;
    ok = sc_display_init(&screen->display, screen->window, icon_novideo,
                         mipmaps);
    if (icon) {
        scrcpy_icon_destroy(icon);
    }
    if (!ok) {
        goto error_destroy_window;
    }

    screen->frame = av_frame_alloc();
    if (!screen->frame) {
        LOG_OOM();
        goto error_destroy_display;
    }

    struct sc_input_manager_params im_params = {
        .controller = params->controller,
        .fp = params->fp,
        .screen = screen,
        .kp = params->kp,
        .mp = params->mp,
        .gp = params->gp,
        .mouse_bindings = params->mouse_bindings,
        .legacy_paste = params->legacy_paste,
        .clipboard_autosync = params->clipboard_autosync,
        .shortcut_mods = params->shortcut_mods,
    };

    sc_input_manager_init(&screen->im, &im_params);

    // Initialize even if not used for simplicity
    sc_mouse_capture_init(&screen->mc, screen->window, params->shortcut_mods);

#ifdef CONTINUOUS_RESIZING_WORKAROUND
    if (screen->video) {
        SDL_AddEventWatch(event_watcher, screen);
    }
#endif

    static const struct sc_frame_sink_ops ops = {
        .open = sc_screen_frame_sink_open,
        .close = sc_screen_frame_sink_close,
        .push = sc_screen_frame_sink_push,
    };

    screen->frame_sink.ops = &ops;

#ifndef NDEBUG
    screen->open = false;
#endif

    if (screen->video) {
        sc_screen_show_idle_window(screen);
    } else if (sc_screen_is_relative_mode(screen)) {
        // Capture mouse immediately if video mirroring is disabled
        sc_mouse_capture_set_active(&screen->mc, true);
    }

    return true;

error_destroy_display:
    sc_display_destroy(&screen->display);
error_destroy_window:
    SDL_DestroyWindow(screen->window);
error_destroy_fps_counter:
    sc_fps_counter_destroy(&screen->fps_counter);
error_destroy_frame_buffer:
    sc_frame_buffer_destroy(&screen->fb);

    return false;
}

static void
sc_screen_show_idle_window(struct sc_screen *screen) {
    assert(screen->video);

    int x = screen->req.x != SC_WINDOW_POSITION_UNDEFINED
          ? screen->req.x : (int) SDL_WINDOWPOS_CENTERED;
    int y = screen->req.y != SC_WINDOW_POSITION_UNDEFINED
          ? screen->req.y : (int) SDL_WINDOWPOS_CENTERED;

    SDL_SetWindowPosition(screen->window, x, y);
    SDL_ShowWindow(screen->window);

    if (screen->req.fullscreen) {
        sc_screen_toggle_fullscreen(screen);
    }

    sc_screen_render_idle(screen);
}

static void
sc_screen_show_initial_window(struct sc_screen *screen) {
    int x = screen->req.x != SC_WINDOW_POSITION_UNDEFINED
          ? screen->req.x : (int) SDL_WINDOWPOS_CENTERED;
    int y = screen->req.y != SC_WINDOW_POSITION_UNDEFINED
          ? screen->req.y : (int) SDL_WINDOWPOS_CENTERED;

    struct sc_size window_size =
        get_initial_optimal_size(screen->content_size, screen->req.width,
                                                       screen->req.height);
    uint32_t width = (uint32_t) window_size.width + UI_PANEL_WIDTH;
    window_size.width = MIN(width, 0x7FFF);

    if (!screen->fullscreen && !screen->maximized && !screen->minimized) {
        set_window_size(screen, window_size);
    }
    SDL_SetWindowPosition(screen->window, x, y);

    if (screen->req.fullscreen && !screen->fullscreen) {
        sc_screen_toggle_fullscreen(screen);
    }

    if (screen->req.start_fps_counter) {
        sc_fps_counter_start(&screen->fps_counter);
    }

    sc_screen_update_content_rect(screen);
}

void
sc_screen_hide_window(struct sc_screen *screen) {
    SDL_HideWindow(screen->window);
}

void
sc_screen_interrupt(struct sc_screen *screen) {
    sc_fps_counter_interrupt(&screen->fps_counter);
}

void
sc_screen_join(struct sc_screen *screen) {
    sc_fps_counter_join(&screen->fps_counter);
}

void
sc_screen_destroy(struct sc_screen *screen) {
#ifndef NDEBUG
    assert(!screen->open);
#endif
    sc_display_destroy(&screen->display);
    av_frame_free(&screen->frame);
    SDL_DestroyWindow(screen->window);
    sc_fps_counter_destroy(&screen->fps_counter);
    sc_frame_buffer_destroy(&screen->fb);
}

static void
resize_for_content(struct sc_screen *screen, struct sc_size old_content_size,
                   struct sc_size new_content_size) {
    assert(screen->video);

    struct sc_size window_size = get_window_size(screen);
    struct sc_size viewport_size = {
        .width = MAX(1, window_size.width - UI_PANEL_WIDTH),
        .height = window_size.height,
    };

    struct sc_size target_viewport_size = {
        .width = (uint32_t) viewport_size.width * new_content_size.width
                / old_content_size.width,
        .height = (uint32_t) viewport_size.height * new_content_size.height
                 / old_content_size.height,
    };

    target_viewport_size =
        get_optimal_size(target_viewport_size, new_content_size, true);
    uint32_t target_width = (uint32_t) target_viewport_size.width
                          + UI_PANEL_WIDTH;
    struct sc_size target_size = {
        .width = MIN(target_width, 0xFFFF),
        .height = target_viewport_size.height,
    };
    set_window_size(screen, target_size);
}

static void
set_content_size(struct sc_screen *screen, struct sc_size new_content_size) {
    assert(screen->video);

    if (!screen->fullscreen && !screen->maximized && !screen->minimized) {
        resize_for_content(screen, screen->content_size, new_content_size);
    } else if (!screen->resize_pending) {
        // Store the windowed size to be able to compute the optimal size once
        // fullscreen/maximized/minimized are disabled
        screen->windowed_content_size = screen->content_size;
        screen->resize_pending = true;
    }

    screen->content_size = new_content_size;
}

static void
apply_pending_resize(struct sc_screen *screen) {
    assert(screen->video);

    assert(!screen->fullscreen);
    assert(!screen->maximized);
    assert(!screen->minimized);
    if (screen->resize_pending) {
        resize_for_content(screen, screen->windowed_content_size,
                                   screen->content_size);
        screen->resize_pending = false;
    }
}

void
sc_screen_set_orientation(struct sc_screen *screen,
                          enum sc_orientation orientation) {
    assert(screen->video);

    if (orientation == screen->orientation) {
        return;
    }

    if (!screen->frame_size.width || !screen->frame_size.height) {
        screen->orientation = orientation;
        LOGI("Display orientation set to %s",
             sc_orientation_get_name(orientation));
        return;
    }

    struct sc_size new_content_size =
        get_oriented_size(screen->frame_size, orientation);

    set_content_size(screen, new_content_size);

    screen->orientation = orientation;
    LOGI("Display orientation set to %s", sc_orientation_get_name(orientation));

    sc_screen_render(screen, true);
}

static bool
sc_screen_init_size(struct sc_screen *screen) {
    // Before first frame
    assert(!screen->has_frame);

    // The requested size is passed via screen->frame_size

    struct sc_size content_size =
        get_oriented_size(screen->frame_size, screen->orientation);
    screen->content_size = content_size;

    enum sc_display_result res =
        sc_display_set_texture_size(&screen->display, screen->frame_size);
    return res != SC_DISPLAY_RESULT_ERROR;
}

// recreate the texture and resize the window if the frame size has changed
static enum sc_display_result
prepare_for_frame(struct sc_screen *screen, struct sc_size new_frame_size) {
    assert(screen->video);

    if (screen->frame_size.width == new_frame_size.width
            && screen->frame_size.height == new_frame_size.height) {
        return SC_DISPLAY_RESULT_OK;
    }

    // frame dimension changed
    screen->frame_size = new_frame_size;

    struct sc_size new_content_size =
        get_oriented_size(new_frame_size, screen->orientation);
    set_content_size(screen, new_content_size);

    sc_screen_update_content_rect(screen);

    return sc_display_set_texture_size(&screen->display, screen->frame_size);
}

static bool
sc_screen_apply_frame(struct sc_screen *screen) {
    assert(screen->video);

    sc_fps_counter_add_rendered_frame(&screen->fps_counter);

    AVFrame *frame = screen->frame;
    struct sc_size new_frame_size = {frame->width, frame->height};
    enum sc_display_result res = prepare_for_frame(screen, new_frame_size);
    if (res == SC_DISPLAY_RESULT_ERROR) {
        return false;
    }
    if (res == SC_DISPLAY_RESULT_PENDING) {
        // Not an error, but do not continue
        return true;
    }

    res = sc_display_update_texture(&screen->display, frame);
    if (res == SC_DISPLAY_RESULT_ERROR) {
        return false;
    }
    if (res == SC_DISPLAY_RESULT_PENDING) {
        // Not an error, but do not continue
        return true;
    }

    if (!screen->has_frame) {
        screen->has_frame = true;
        screen->connection_state = SC_SCREEN_CONNECTION_RUNNING;
        // this is the very first frame, show the window
        sc_screen_show_initial_window(screen);

        if (sc_screen_is_relative_mode(screen)) {
            // Capture mouse on start
            sc_mouse_capture_set_active(&screen->mc, true);
        }
    }

    sc_screen_render(screen, false);
    return true;
}

static bool
sc_screen_update_frame(struct sc_screen *screen) {
    assert(screen->video);

    if (screen->paused) {
        if (!screen->resume_frame) {
            screen->resume_frame = av_frame_alloc();
            if (!screen->resume_frame) {
                LOG_OOM();
                return false;
            }
        } else {
            av_frame_unref(screen->resume_frame);
        }
        sc_frame_buffer_consume(&screen->fb, screen->resume_frame);
        return true;
    }

    av_frame_unref(screen->frame);
    sc_frame_buffer_consume(&screen->fb, screen->frame);
    return sc_screen_apply_frame(screen);
}

void
sc_screen_set_paused(struct sc_screen *screen, bool paused) {
    assert(screen->video);

    if (!paused && !screen->paused) {
        // nothing to do
        return;
    }

    if (screen->paused && screen->resume_frame) {
        // If display screen was paused, refresh the frame immediately, even if
        // the new state is also paused.
        av_frame_free(&screen->frame);
        screen->frame = screen->resume_frame;
        screen->resume_frame = NULL;
        sc_screen_apply_frame(screen);
    }

    if (!paused) {
        LOGI("Display screen unpaused");
    } else if (!screen->paused) {
        LOGI("Display screen paused");
    } else {
        LOGI("Display screen re-paused");
    }

    screen->paused = paused;
}

void
sc_screen_set_connection_state(struct sc_screen *screen,
                               enum sc_screen_connection_state state) {
    screen->connection_state = state;

    if (!screen->video) {
        return;
    }

    if (state != SC_SCREEN_CONNECTION_RUNNING) {
        screen->has_frame = false;
        screen->paused = false;
        screen->screenshot_button_hovered = false;
        screen->screenshot_button_pressed = false;

        if (sc_screen_is_relative_mode(screen)) {
            sc_mouse_capture_set_active(&screen->mc, false);
        }

        sc_screen_render_idle(screen);
        return;
    }

    sc_screen_render_current_state(screen, false);
}

void
sc_screen_set_input_processors(struct sc_screen *screen,
                               struct sc_controller *controller,
                               struct sc_file_pusher *fp,
                               struct sc_key_processor *kp,
                               struct sc_mouse_processor *mp,
                               struct sc_gamepad_processor *gp) {
    bool was_relative_mode = sc_screen_is_relative_mode(screen);
    sc_input_manager_configure(&screen->im, controller, fp, kp, mp, gp);
    bool relative_mode = sc_screen_is_relative_mode(screen);

    if (relative_mode != was_relative_mode) {
        bool active = relative_mode && (!screen->video || screen->has_frame);
        sc_mouse_capture_set_active(&screen->mc, active);
    }
}

void
sc_screen_set_window_title(struct sc_screen *screen, const char *title) {
    assert(title);
    SDL_SetWindowTitle(screen->window, title);
}

void
sc_screen_toggle_fullscreen(struct sc_screen *screen) {
    assert(screen->video);

    uint32_t new_mode = screen->fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (SDL_SetWindowFullscreen(screen->window, new_mode)) {
        LOGW("Could not switch fullscreen mode: %s", SDL_GetError());
        return;
    }

    screen->fullscreen = !screen->fullscreen;
    if (!screen->fullscreen && !screen->maximized && !screen->minimized) {
        apply_pending_resize(screen);
    }

    LOGD("Switched to %s mode", screen->fullscreen ? "fullscreen" : "windowed");
    sc_screen_render_current_state(screen, true);
}

void
sc_screen_resize_to_fit(struct sc_screen *screen) {
    assert(screen->video);

    if (!screen->has_frame) {
        return;
    }

    if (screen->fullscreen || screen->maximized || screen->minimized) {
        return;
    }

    struct sc_point point = get_window_position(screen);
    struct sc_size window_size = get_window_size(screen);
    struct sc_size viewport_size = {
        .width = MAX(1, window_size.width - UI_PANEL_WIDTH),
        .height = window_size.height,
    };

    struct sc_size optimal_viewport_size =
        get_optimal_size(viewport_size, screen->content_size, false);
    uint32_t optimal_width = (uint32_t) optimal_viewport_size.width
                           + UI_PANEL_WIDTH;
    struct sc_size optimal_size = {
        .width = MIN(optimal_width, 0xFFFF),
        .height = optimal_viewport_size.height,
    };

    // Center the window related to the device screen
    assert(optimal_size.width <= window_size.width);
    assert(optimal_size.height <= window_size.height);
    uint32_t new_x = point.x + (window_size.width - optimal_size.width) / 2;
    uint32_t new_y = point.y + (window_size.height - optimal_size.height) / 2;

    SDL_SetWindowSize(screen->window, optimal_size.width, optimal_size.height);
    SDL_SetWindowPosition(screen->window, new_x, new_y);
    LOGD("Resized to optimal size: %ux%u", optimal_size.width,
                                           optimal_size.height);
}

void
sc_screen_resize_to_pixel_perfect(struct sc_screen *screen) {
    assert(screen->video);

    if (!screen->has_frame) {
        return;
    }

    if (screen->fullscreen || screen->minimized) {
        return;
    }

    if (screen->maximized) {
        SDL_RestoreWindow(screen->window);
        screen->maximized = false;
    }

    struct sc_size content_size = screen->content_size;
    SDL_SetWindowSize(screen->window, content_size.width + UI_PANEL_WIDTH,
                      content_size.height);
    LOGD("Resized to pixel-perfect: %ux%u",
         content_size.width + UI_PANEL_WIDTH, content_size.height);
}

bool
sc_screen_handle_event(struct sc_screen *screen, const SDL_Event *event) {
    switch (event->type) {
        case SC_EVENT_SCREEN_INIT_SIZE: {
            // The initial size is passed via screen->frame_size
            bool ok = sc_screen_init_size(screen);
            if (!ok) {
                LOGE("Could not initialize screen size");
                return false;
            }
            return true;
        }
        case SC_EVENT_NEW_FRAME: {
            bool ok = sc_screen_update_frame(screen);
            if (!ok) {
                LOGE("Frame update failed\n");
                return false;
            }
            return true;
        }
        case SDL_WINDOWEVENT:
            if (!screen->video) {
                if (event->window.event == SDL_WINDOWEVENT_EXPOSED) {
                    sc_screen_render_novideo(screen);
                }
                return true;
            }

            switch (event->window.event) {
                case SDL_WINDOWEVENT_EXPOSED:
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                    sc_screen_render_current_state(screen, true);
                    break;
                case SDL_WINDOWEVENT_MAXIMIZED:
                    screen->maximized = true;
                    break;
                case SDL_WINDOWEVENT_MINIMIZED:
                    screen->minimized = true;
                    break;
                case SDL_WINDOWEVENT_RESTORED:
                    if (screen->fullscreen) {
                        // On Windows, in maximized+fullscreen, disabling
                        // fullscreen mode unexpectedly triggers the "restored"
                        // then "maximized" events, leaving the window in a
                        // weird state (maximized according to the events, but
                        // not maximized visually).
                        break;
                    }
                    screen->maximized = false;
                    screen->minimized = false;
                    if (screen->has_frame) {
                        apply_pending_resize(screen);
                    }
                    sc_screen_render_current_state(screen, true);
                    break;
            }
            return true;
    }

    if (screen->video && sc_screen_handle_panel_event(screen, event)) {
        // The side panel consumed the event.
        return true;
    }

    if (screen->video && !screen->has_frame) {
        switch (event->type) {
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEWHEEL:
            case SDL_FINGERMOTION:
            case SDL_FINGERDOWN:
            case SDL_FINGERUP:
                return true;
            default:
                break;
        }
    }

    if (sc_screen_is_relative_mode(screen)
            && sc_mouse_capture_handle_event(&screen->mc, event)) {
        // The mouse capture handler consumed the event
        return true;
    }

    sc_input_manager_handle_event(&screen->im, event);
    return true;
}

struct sc_point
sc_screen_convert_drawable_to_frame_coords(struct sc_screen *screen,
                                           int32_t x, int32_t y) {
    assert(screen->video);

    enum sc_orientation orientation = screen->orientation;

    int32_t w = screen->content_size.width;
    int32_t h = screen->content_size.height;

    // screen->rect must be initialized to avoid a division by zero
    assert(screen->rect.w && screen->rect.h);

    x = (int64_t) (x - screen->rect.x) * w / screen->rect.w;
    y = (int64_t) (y - screen->rect.y) * h / screen->rect.h;

    struct sc_point result;
    switch (orientation) {
        case SC_ORIENTATION_0:
            result.x = x;
            result.y = y;
            break;
        case SC_ORIENTATION_90:
            result.x = y;
            result.y = w - x;
            break;
        case SC_ORIENTATION_180:
            result.x = w - x;
            result.y = h - y;
            break;
        case SC_ORIENTATION_270:
            result.x = h - y;
            result.y = x;
            break;
        case SC_ORIENTATION_FLIP_0:
            result.x = w - x;
            result.y = y;
            break;
        case SC_ORIENTATION_FLIP_90:
            result.x = h - y;
            result.y = w - x;
            break;
        case SC_ORIENTATION_FLIP_180:
            result.x = x;
            result.y = h - y;
            break;
        default:
            assert(orientation == SC_ORIENTATION_FLIP_270);
            result.x = y;
            result.y = x;
            break;
    }

    return result;
}

struct sc_point
sc_screen_convert_window_to_frame_coords(struct sc_screen *screen,
                                         int32_t x, int32_t y) {
    sc_screen_hidpi_scale_coords(screen, &x, &y);
    return sc_screen_convert_drawable_to_frame_coords(screen, x, y);
}

void
sc_screen_hidpi_scale_coords(struct sc_screen *screen, int32_t *x, int32_t *y) {
    // take the HiDPI scaling (dw/ww and dh/wh) into account
    int ww, wh, dw, dh;
    SDL_GetWindowSize(screen->window, &ww, &wh);
    SDL_GL_GetDrawableSize(screen->window, &dw, &dh);

    // scale for HiDPI (64 bits for intermediate multiplications)
    *x = (int64_t) *x * dw / ww;
    *y = (int64_t) *y * dh / wh;
}
