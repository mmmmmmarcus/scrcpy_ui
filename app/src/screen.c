#include "screen.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL2/SDL.h>
#include <libswscale/swscale.h>

#include "events.h"
#include "icon.h"
#include "options.h"
#include "util/log.h"
#ifdef __APPLE__
# include "sys/darwin/clipboard.h"
# include "sys/darwin/window.h"
#endif

#define DISPLAY_MARGINS 96
#define UI_PANEL_WIDTH 72
#define UI_LEFT_PADDING_X 24
#define UI_LEFT_PADDING_Y 48
#define UI_MIRROR_ASPECT_W 249
#define UI_MIRROR_ASPECT_H 433
#define UI_BUTTON_WIDTH 40
#define UI_BUTTON_HEIGHT 88
#define UI_TOGGLE_BUTTON_SIZE 40
#define UI_TOGGLE_TOP_OFFSET 20
#define UI_SETTINGS_BUTTON_SIZE 40
#define UI_SETTINGS_BOTTOM_OFFSET 20
#define UI_SETTINGS_MENU_WIDTH 232
#define UI_SETTINGS_MENU_ITEM_HEIGHT 32
#define UI_SETTINGS_MENU_PADDING 8
#define UI_SETTINGS_MENU_GAP 6
#define UI_SETTINGS_MENU_MARGIN_RIGHT 10
#define UI_BUTTON_ICON_SIZE 24
#define UI_BUTTON_FEEDBACK_IN_MS 150
#define UI_BUTTON_FEEDBACK_HOLD_MS 1000
#define UI_BUTTON_FEEDBACK_OUT_MS 150
#define UI_BUTTON_FEEDBACK_DURATION_MS \
    (UI_BUTTON_FEEDBACK_IN_MS + UI_BUTTON_FEEDBACK_HOLD_MS \
     + UI_BUTTON_FEEDBACK_OUT_MS)
#define UI_WAITING_LABEL "PLEASE CONNECT A DEVICE"
#define UI_SECURE_LABEL "please unlcok your device"
#define UI_SCREENSHOT_ICON_PATH_ENV "SCRCPY_SCREENSHOT_ICON_PATH"
#define UI_SCREENSHOT_CHECK_ICON_PATH_ENV "SCRCPY_SCREENSHOT_CHECK_ICON_PATH"
#define UI_SCREENSHOT_BUTTON_BG_PATH_ENV "SCRCPY_SCREENSHOT_BUTTON_BG_PATH"
#define UI_INPUT_TOGGLE_ICON_PATH_ENV "SCRCPY_INPUT_TOGGLE_ICON_PATH"
#define UI_INPUT_TOGGLE_BUTTON_BG_PATH_ENV "SCRCPY_INPUT_TOGGLE_BUTTON_BG_PATH"
#define UI_SETTINGS_ICON_PATH_ENV "SCRCPY_SETTINGS_ICON_PATH"
#define UI_SETTINGS_COPY_LABEL "COPY TO CLIPBOARD"
#define UI_SETTINGS_SAVE_LABEL "SAVE IMAGE TO"
#define UI_SETTINGS_FOLDER_LABEL "SELECT FOLDER"
#define UI_SETTINGS_FOLDER_SET_LABEL "FOLDER SELECTED"

#define DOWNCAST(SINK) container_of(SINK, struct sc_screen, frame_sink)

static void
sc_screen_show_idle_window(struct sc_screen *screen);

static void
sc_screen_draw_text_centered(SDL_Renderer *renderer, const SDL_Rect *area,
                             const char *text, uint8_t r, uint8_t g, uint8_t b);

static bool
sc_screen_load_screenshot_icon(struct sc_screen *screen);

static bool
sc_screen_load_screenshot_check_icon(struct sc_screen *screen);

static bool
sc_screen_load_screenshot_button_bg(struct sc_screen *screen);

static bool
sc_screen_load_input_toggle_icon(struct sc_screen *screen);

static bool
sc_screen_load_input_toggle_button_bg(struct sc_screen *screen);

static bool
sc_screen_load_settings_icon(struct sc_screen *screen);

static void
sc_screen_set_input_enabled(struct sc_screen *screen, bool enabled);

static void
sc_screen_fill_rounded_rect(SDL_Renderer *renderer, const SDL_Rect *rect,
                            int radius);

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
    bool show_panel = screen->connection_state == SC_SCREEN_CONNECTION_RUNNING;
    int panel_width = 0;
    if (show_panel) {
        panel_width = scale_window_to_drawable(screen, UI_PANEL_WIDTH, true);
        panel_width = CLAMP(panel_width, 0, drawable_size.width);
    }

    screen->panel_rect.x = drawable_size.width - panel_width;
    screen->panel_rect.y = 0;
    screen->panel_rect.w = panel_width;
    screen->panel_rect.h = drawable_size.height;

    int button_width = scale_window_to_drawable(screen, UI_BUTTON_WIDTH, true);
    int button_height = scale_window_to_drawable(screen, UI_BUTTON_HEIGHT, false);
    int toggle_button_size =
        scale_window_to_drawable(screen, UI_TOGGLE_BUTTON_SIZE, true);
    int toggle_top =
        scale_window_to_drawable(screen, UI_TOGGLE_TOP_OFFSET, false);
    int settings_button_size =
        scale_window_to_drawable(screen, UI_SETTINGS_BUTTON_SIZE, true);
    int settings_bottom =
        scale_window_to_drawable(screen, UI_SETTINGS_BOTTOM_OFFSET, false);
    button_width = MIN(button_width, panel_width);
    button_height = MIN(button_height, drawable_size.height);
    toggle_button_size = MIN(toggle_button_size, panel_width);
    settings_button_size = MIN(settings_button_size, panel_width);

    screen->screenshot_button_rect.x = screen->panel_rect.x
                                     + (panel_width - button_width) / 2;
    screen->screenshot_button_rect.y = (drawable_size.height - button_height) / 2;
    screen->screenshot_button_rect.w = button_width;
    screen->screenshot_button_rect.h = button_height;

    screen->input_toggle_button_rect.x = screen->screenshot_button_rect.x;
    screen->input_toggle_button_rect.y = toggle_top;
    screen->input_toggle_button_rect.w = toggle_button_size;
    screen->input_toggle_button_rect.h = toggle_button_size;

    screen->settings_button_rect.x = screen->panel_rect.x
                                   + (panel_width - settings_button_size) / 2;
    screen->settings_button_rect.y =
        MAX(0, drawable_size.height - settings_bottom - settings_button_size);
    screen->settings_button_rect.w = settings_button_size;
    screen->settings_button_rect.h = settings_button_size;

    int menu_width = scale_window_to_drawable(screen, UI_SETTINGS_MENU_WIDTH,
                                              true);
    int menu_item_height =
        scale_window_to_drawable(screen, UI_SETTINGS_MENU_ITEM_HEIGHT, false);
    int menu_padding =
        scale_window_to_drawable(screen, UI_SETTINGS_MENU_PADDING, true);
    int menu_gap = scale_window_to_drawable(screen, UI_SETTINGS_MENU_GAP, false);
    int menu_margin =
        scale_window_to_drawable(screen, UI_SETTINGS_MENU_MARGIN_RIGHT, true);
    menu_width = CLAMP(menu_width, 0, drawable_size.width);
    menu_padding = MAX(0, menu_padding);
    menu_gap = MAX(0, menu_gap);
    menu_item_height = MAX(1, menu_item_height);
    int menu_height =
        menu_padding * 2 + menu_item_height * 3 + menu_gap * 2;

    if (!show_panel || !menu_width || !menu_height) {
        screen->settings_menu_rect = (SDL_Rect) {0, 0, 0, 0};
        screen->settings_menu_copy_rect = (SDL_Rect) {0, 0, 0, 0};
        screen->settings_menu_save_rect = (SDL_Rect) {0, 0, 0, 0};
        screen->settings_menu_directory_rect = (SDL_Rect) {0, 0, 0, 0};
        return;
    }

    int menu_x = screen->panel_rect.x - menu_margin - menu_width;
    int menu_y =
        screen->settings_button_rect.y + screen->settings_button_rect.h - menu_height;
    menu_x = CLAMP(menu_x, 0, drawable_size.width - menu_width);
    menu_y = CLAMP(menu_y, 0, drawable_size.height - menu_height);

    screen->settings_menu_rect = (SDL_Rect) {
        .x = menu_x,
        .y = menu_y,
        .w = menu_width,
        .h = menu_height,
    };

    int item_x = menu_x + menu_padding;
    int item_w = MAX(1, menu_width - 2 * menu_padding);
    int item_y = menu_y + menu_padding;
    screen->settings_menu_copy_rect = (SDL_Rect) {
        .x = item_x,
        .y = item_y,
        .w = item_w,
        .h = menu_item_height,
    };
    item_y += menu_item_height + menu_gap;
    screen->settings_menu_save_rect = (SDL_Rect) {
        .x = item_x,
        .y = item_y,
        .w = item_w,
        .h = menu_item_height,
    };
    item_y += menu_item_height + menu_gap;
    screen->settings_menu_directory_rect = (SDL_Rect) {
        .x = item_x,
        .y = item_y,
        .w = item_w,
        .h = menu_item_height,
    };
}

static SDL_Rect
sc_screen_get_mirror_slot(struct sc_screen *screen) {
    int viewport_width = screen->panel_rect.x;
    int viewport_height = screen->panel_rect.h;
    if (viewport_width <= 0 || viewport_height <= 0) {
        return (SDL_Rect) {0, 0, 0, 0};
    }

    int pad_x = scale_window_to_drawable(screen, UI_LEFT_PADDING_X, true);
    int pad_y = scale_window_to_drawable(screen, UI_LEFT_PADDING_Y, false);
    int slot_w = MAX(0, viewport_width - 2 * pad_x);
    int slot_h = MAX(0, viewport_height - 2 * pad_y);
    if (!slot_w || !slot_h) {
        return (SDL_Rect) {0, 0, 0, 0};
    }

    int rect_w = slot_w;
    int rect_h = slot_h;
    bool keep_width = (int64_t) slot_w * UI_MIRROR_ASPECT_H
                    <= (int64_t) slot_h * UI_MIRROR_ASPECT_W;
    if (keep_width) {
        rect_h = (int64_t) slot_w * UI_MIRROR_ASPECT_H / UI_MIRROR_ASPECT_W;
    } else {
        rect_w = (int64_t) slot_h * UI_MIRROR_ASPECT_W / UI_MIRROR_ASPECT_H;
    }

    SDL_Rect rect = {
        .x = (viewport_width - rect_w) / 2,
        .y = (viewport_height - rect_h) / 2,
        .w = rect_w,
        .h = rect_h,
    };
    return rect;
}

static void
sc_screen_update_content_rect(struct sc_screen *screen) {
    assert(screen->video);

    sc_screen_update_ui_rects(screen);
    struct sc_size content_size = screen->content_size;

    SDL_Rect *rect = &screen->rect;
    SDL_Rect mirror_slot = sc_screen_get_mirror_slot(screen);
    if (!mirror_slot.w || !mirror_slot.h) {
        rect->x = 0;
        rect->y = 0;
        rect->w = 0;
        rect->h = 0;
        return;
    }

    if (!content_size.width || !content_size.height) {
        *rect = mirror_slot;
        return;
    }

    bool keep_width = (int64_t) content_size.width * mirror_slot.h
                    > (int64_t) content_size.height * mirror_slot.w;
    if (keep_width) {
        rect->w = mirror_slot.w;
        rect->h = (int64_t) mirror_slot.w * content_size.height
                                     / content_size.width;
        rect->x = mirror_slot.x;
        rect->y = mirror_slot.y + (mirror_slot.h - rect->h) / 2;
    } else {
        rect->h = mirror_slot.h;
        rect->w = (int64_t) mirror_slot.h * content_size.width
                                     / content_size.height;
        rect->x = mirror_slot.x + (mirror_slot.w - rect->w) / 2;
        rect->y = mirror_slot.y;
    }
}

static bool
sc_screen_load_screenshot_icon(struct sc_screen *screen) {
    const char *path = getenv(UI_SCREENSHOT_ICON_PATH_ENV);
    if (!path || !*path) {
        return true;
    }

    SDL_Surface *surface = scrcpy_icon_load_from_path(path);
    if (!surface) {
        LOGW("Could not load screenshot icon: %s", path);
        return false;
    }

    SDL_Texture *texture =
        SDL_CreateTextureFromSurface(screen->display.renderer, surface);
    if (!texture) {
        LOGW("Could not create screenshot icon texture: %s", SDL_GetError());
        scrcpy_icon_destroy(surface);
        return false;
    }

    screen->screenshot_icon = texture;
    screen->screenshot_icon_width = surface->w;
    screen->screenshot_icon_height = surface->h;
    scrcpy_icon_destroy(surface);
    return true;
}

static bool
sc_screen_load_screenshot_check_icon(struct sc_screen *screen) {
    const char *path = getenv(UI_SCREENSHOT_CHECK_ICON_PATH_ENV);
    if (!path || !*path) {
        return true;
    }

    SDL_Surface *surface = scrcpy_icon_load_from_path(path);
    if (!surface) {
        LOGW("Could not load screenshot check icon: %s", path);
        return false;
    }

    SDL_Texture *texture =
        SDL_CreateTextureFromSurface(screen->display.renderer, surface);
    if (!texture) {
        LOGW("Could not create screenshot check icon texture: %s",
             SDL_GetError());
        scrcpy_icon_destroy(surface);
        return false;
    }

    screen->screenshot_check_icon = texture;
    screen->screenshot_check_icon_width = surface->w;
    screen->screenshot_check_icon_height = surface->h;
    scrcpy_icon_destroy(surface);
    return true;
}

static bool
sc_screen_load_screenshot_button_bg(struct sc_screen *screen) {
    const char *path = getenv(UI_SCREENSHOT_BUTTON_BG_PATH_ENV);
    if (!path || !*path) {
        return true;
    }

    SDL_Surface *surface = scrcpy_icon_load_from_path(path);
    if (!surface) {
        LOGW("Could not load screenshot button background: %s", path);
        return false;
    }

    SDL_Texture *texture =
        SDL_CreateTextureFromSurface(screen->display.renderer, surface);
    if (!texture) {
        LOGW("Could not create screenshot button background texture: %s",
             SDL_GetError());
        scrcpy_icon_destroy(surface);
        return false;
    }

    screen->screenshot_button_bg = texture;
    screen->screenshot_button_bg_width = surface->w;
    screen->screenshot_button_bg_height = surface->h;
    scrcpy_icon_destroy(surface);
    return true;
}

static bool
sc_screen_load_input_toggle_icon(struct sc_screen *screen) {
    const char *path = getenv(UI_INPUT_TOGGLE_ICON_PATH_ENV);
    if (!path || !*path) {
        return true;
    }

    SDL_Surface *surface = scrcpy_icon_load_from_path(path);
    if (!surface) {
        LOGW("Could not load input toggle icon: %s", path);
        return false;
    }

    SDL_Texture *texture =
        SDL_CreateTextureFromSurface(screen->display.renderer, surface);
    if (!texture) {
        LOGW("Could not create input toggle icon texture: %s", SDL_GetError());
        scrcpy_icon_destroy(surface);
        return false;
    }

    screen->input_toggle_icon = texture;
    screen->input_toggle_icon_width = surface->w;
    screen->input_toggle_icon_height = surface->h;
    scrcpy_icon_destroy(surface);
    return true;
}

static bool
sc_screen_load_input_toggle_button_bg(struct sc_screen *screen) {
    const char *path = getenv(UI_INPUT_TOGGLE_BUTTON_BG_PATH_ENV);
    if (!path || !*path) {
        return true;
    }

    SDL_Surface *surface = scrcpy_icon_load_from_path(path);
    if (!surface) {
        LOGW("Could not load input toggle button background: %s", path);
        return false;
    }

    SDL_Texture *texture =
        SDL_CreateTextureFromSurface(screen->display.renderer, surface);
    if (!texture) {
        LOGW("Could not create input toggle button background texture: %s",
             SDL_GetError());
        scrcpy_icon_destroy(surface);
        return false;
    }

    screen->input_toggle_button_bg = texture;
    screen->input_toggle_button_bg_width = surface->w;
    screen->input_toggle_button_bg_height = surface->h;
    scrcpy_icon_destroy(surface);
    return true;
}

static bool
sc_screen_load_settings_icon(struct sc_screen *screen) {
    const char *path = getenv(UI_SETTINGS_ICON_PATH_ENV);
    if (!path || !*path) {
        return true;
    }

    SDL_Surface *surface = scrcpy_icon_load_from_path(path);
    if (!surface) {
        LOGW("Could not load settings icon: %s", path);
        return false;
    }

    SDL_Texture *texture =
        SDL_CreateTextureFromSurface(screen->display.renderer, surface);
    if (!texture) {
        LOGW("Could not create settings icon texture: %s", SDL_GetError());
        scrcpy_icon_destroy(surface);
        return false;
    }

    screen->settings_icon = texture;
    screen->settings_icon_width = surface->w;
    screen->settings_icon_height = surface->h;
    scrcpy_icon_destroy(surface);
    return true;
}

// render the texture to the renderer
//
// Set the update_content_rect flag if the window or content size may have
// changed, so that the content rectangle is recomputed
static void
sc_screen_draw_idle_placeholder(struct sc_screen *screen) {
    SDL_Renderer *renderer = screen->display.renderer;
    SDL_Rect mirror = sc_screen_get_mirror_slot(screen);
    if (!mirror.w || !mirror.h) {
        return;
    }

    SDL_SetRenderDrawColor(renderer, 41, 41, 41, 255);
    SDL_RenderFillRect(renderer, &mirror);

    if (screen->connection_state != SC_SCREEN_CONNECTION_RUNNING) {
        SDL_Rect label_area = {
            .x = 0,
            .y = mirror.y + mirror.h / 2 - 14,
            .w = screen->panel_rect.x,
            .h = 28,
        };
        sc_screen_draw_text_centered(renderer, &label_area, UI_WAITING_LABEL,
                                     176, 183, 191);
    }
}

static const uint8_t *
sc_screen_get_button_glyph(char c) {
    static const uint8_t glyph_space[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t glyph_a[7] = {
        0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11,
    };
    static const uint8_t glyph_b[7] = {
        0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E,
    };
    static const uint8_t glyph_c[7] = {
        0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E,
    };
    static const uint8_t glyph_d[7] = {
        0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E,
    };
    static const uint8_t glyph_e[7] = {
        0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F,
    };
    static const uint8_t glyph_f[7] = {
        0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10,
    };
    static const uint8_t glyph_g[7] = {
        0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E,
    };
    static const uint8_t glyph_h[7] = {
        0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11,
    };
    static const uint8_t glyph_i[7] = {
        0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F,
    };
    static const uint8_t glyph_k[7] = {
        0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11,
    };
    static const uint8_t glyph_l[7] = {
        0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F,
    };
    static const uint8_t glyph_n[7] = {
        0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11,
    };
    static const uint8_t glyph_o[7] = {
        0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E,
    };
    static const uint8_t glyph_m[7] = {
        0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11,
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
    static const uint8_t glyph_u[7] = {
        0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E,
    };
    static const uint8_t glyph_v[7] = {
        0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04,
    };
    static const uint8_t glyph_y[7] = {
        0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04,
    };

    switch (toupper((unsigned char) c)) {
        case ' ':
            return glyph_space;
        case 'A':
            return glyph_a;
        case 'B':
            return glyph_b;
        case 'C':
            return glyph_c;
        case 'D':
            return glyph_d;
        case 'E':
            return glyph_e;
        case 'F':
            return glyph_f;
        case 'G':
            return glyph_g;
        case 'H':
            return glyph_h;
        case 'I':
            return glyph_i;
        case 'K':
            return glyph_k;
        case 'L':
            return glyph_l;
        case 'M':
            return glyph_m;
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
        case 'U':
            return glyph_u;
        case 'V':
            return glyph_v;
        case 'Y':
            return glyph_y;
        default:
            return glyph_space;
    }
}

static void
sc_screen_draw_text_centered(SDL_Renderer *renderer, const SDL_Rect *area,
                             const char *text, uint8_t r, uint8_t g, uint8_t b) {
    size_t len = strlen(text);
    if (!len || !area->w || !area->h) {
        return;
    }

    int padding = MAX(2, area->h / 8);
    int max_scale_w =
        (area->w - 2 * padding) / (int) (len * 5 + (len - 1));
    int max_scale_h = (area->h - 2 * padding) / 7;
    int scale = MAX(1, MIN(max_scale_w, max_scale_h));

    int glyph_width = 5 * scale;
    int spacing = scale;
    int text_width = (int) len * glyph_width + (int) (len - 1) * spacing;
    int text_height = 7 * scale;
    int start_x = area->x + (area->w - text_width) / 2;
    int start_y = area->y + (area->h - text_height) / 2;

    SDL_SetRenderDrawColor(renderer, r, g, b, 255);

    int x = start_x;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t *rows = sc_screen_get_button_glyph(text[i]);
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

static SDL_Rect
sc_screen_get_icon_rect(struct sc_screen *screen, const SDL_Rect *button) {
    int icon_w = scale_window_to_drawable(screen, UI_BUTTON_ICON_SIZE, true);
    int icon_h = scale_window_to_drawable(screen, UI_BUTTON_ICON_SIZE, false);
    int available_w = MAX(1, button->w - button->w / 3);
    int available_h = MAX(1, button->h - button->h / 3);
    icon_w = MIN(icon_w, available_w);
    icon_h = MIN(icon_h, available_h);
    if (!icon_w || !icon_h) {
        return (SDL_Rect) {0, 0, 0, 0};
    }
    return (SDL_Rect) {
        .x = button->x + (button->w - icon_w) / 2,
        .y = button->y + (button->h - icon_h) / 2,
        .w = icon_w,
        .h = icon_h,
    };
}

static float
sc_ease_cubic_0_04_04_1(float x) {
    // cubic-bezier(0, 0.4, 0.4, 1)
    // Solve t from x(t), then return y(t).
    float lo = 0.0f;
    float hi = 1.0f;
    float t = x;
    for (int i = 0; i < 12; ++i) {
        t = (lo + hi) * 0.5f;
        float omt = 1.0f - t;
        float xt = 3.0f * omt * t * t * 0.4f + t * t * t;
        if (xt < x) {
            lo = t;
        } else {
            hi = t;
        }
    }

    float omt = 1.0f - t;
    float yt = 3.0f * omt * omt * t * 0.4f + 3.0f * omt * t * t + t * t * t;
    return CLAMP(yt, 0.0f, 1.0f);
}

static float
sc_screen_get_screenshot_button_feedback_progress(struct sc_screen *screen) {
    if (!screen->screenshot_button_feedback_active) {
        return screen->screenshot_button_feedback_progress;
    }

    uint32_t now = SDL_GetTicks();
    uint32_t elapsed = now - screen->screenshot_button_feedback_start_ms;
    if (elapsed >= UI_BUTTON_FEEDBACK_DURATION_MS) {
        return 0.0f;
    }

    if (elapsed < UI_BUTTON_FEEDBACK_IN_MS) {
        float phase = (float) elapsed / UI_BUTTON_FEEDBACK_IN_MS;
        return sc_ease_cubic_0_04_04_1(phase);
    }

    if (elapsed < UI_BUTTON_FEEDBACK_IN_MS + UI_BUTTON_FEEDBACK_HOLD_MS) {
        return 1.0f;
    }

    uint32_t out_elapsed =
        elapsed - UI_BUTTON_FEEDBACK_IN_MS - UI_BUTTON_FEEDBACK_HOLD_MS;
    float phase = (float) out_elapsed / UI_BUTTON_FEEDBACK_OUT_MS;
    return 1.0f - sc_ease_cubic_0_04_04_1(phase);
}

static void
sc_screen_draw_button_icon(struct sc_screen *screen, const SDL_Rect *button) {
    if (!screen->screenshot_icon) {
        return;
    }

    SDL_Rect dst = sc_screen_get_icon_rect(screen, button);
    if (!dst.w || !dst.h) {
        return;
    }

    float progress = sc_screen_get_screenshot_button_feedback_progress(screen);
    uint8_t camera_alpha = (uint8_t) ((1.0f - progress) * 255.0f);
    uint8_t check_alpha = (uint8_t) (progress * 255.0f);

    SDL_SetTextureColorMod(screen->screenshot_icon, 40, 40, 48);
    SDL_SetTextureAlphaMod(screen->screenshot_icon, camera_alpha);
    SDL_RenderCopy(screen->display.renderer, screen->screenshot_icon, NULL, &dst);

    if (screen->screenshot_check_icon && check_alpha) {
        SDL_SetTextureColorMod(screen->screenshot_check_icon, 40, 40, 48);
        SDL_SetTextureAlphaMod(screen->screenshot_check_icon, check_alpha);
        SDL_RenderCopy(screen->display.renderer, screen->screenshot_check_icon,
                       NULL, &dst);
    }
}

static void
sc_screen_draw_toggle_icon(struct sc_screen *screen, const SDL_Rect *button) {
    if (!screen->input_toggle_icon) {
        return;
    }

    int icon_w = scale_window_to_drawable(screen, UI_BUTTON_ICON_SIZE, true);
    int icon_h = scale_window_to_drawable(screen, UI_BUTTON_ICON_SIZE, false);
    icon_w = MIN(icon_w, button->w);
    icon_h = MIN(icon_h, button->h);
    if (!icon_w || !icon_h) {
        return;
    }

    SDL_Rect dst = {
        .x = button->x + (button->w - icon_w) / 2,
        .y = button->y + (button->h - icon_h) / 2,
        .w = icon_w,
        .h = icon_h,
    };

    SDL_SetTextureColorMod(screen->input_toggle_icon, 40, 40, 48);
    SDL_RenderCopy(screen->display.renderer, screen->input_toggle_icon, NULL,
                   &dst);
}

static void
sc_screen_draw_settings_icon(struct sc_screen *screen, const SDL_Rect *button) {
    if (!screen->settings_icon) {
        sc_screen_draw_text_centered(screen->display.renderer, button, "S",
                                     40, 40, 48);
        return;
    }

    int icon_w = scale_window_to_drawable(screen, UI_BUTTON_ICON_SIZE, true);
    int icon_h = scale_window_to_drawable(screen, UI_BUTTON_ICON_SIZE, false);
    icon_w = MIN(icon_w, button->w);
    icon_h = MIN(icon_h, button->h);
    if (!icon_w || !icon_h) {
        return;
    }

    SDL_Rect dst = {
        .x = button->x + (button->w - icon_w) / 2,
        .y = button->y + (button->h - icon_h) / 2,
        .w = icon_w,
        .h = icon_h,
    };

    SDL_SetTextureColorMod(screen->settings_icon, 40, 40, 48);
    SDL_RenderCopy(screen->display.renderer, screen->settings_icon, NULL, &dst);
}

#ifndef __APPLE__
static void
sc_screen_draw_settings_menu_item(struct sc_screen *screen,
                                  const SDL_Rect *rect, const char *label,
                                  bool selected, bool hovered) {
    uint8_t r = 63;
    uint8_t g = 63;
    uint8_t b = 67;
    if (selected) {
        r = 255;
        g = 199;
        b = 0;
        if (hovered) {
            r = 255;
            g = 212;
            b = 38;
        }
    } else if (hovered) {
        r = 78;
        g = 78;
        b = 82;
    }

    SDL_SetRenderDrawColor(screen->display.renderer, r, g, b, 255);
    sc_screen_fill_rounded_rect(screen->display.renderer, rect, rect->h / 2);

    if (selected) {
        sc_screen_draw_text_centered(screen->display.renderer, rect, label,
                                     40, 40, 48);
    } else {
        sc_screen_draw_text_centered(screen->display.renderer, rect, label,
                                     226, 227, 230);
    }
}

static void
sc_screen_draw_settings_menu(struct sc_screen *screen) {
    if (!screen->settings_menu_open || !screen->settings_menu_rect.w) {
        return;
    }

    SDL_SetRenderDrawColor(screen->display.renderer, 44, 44, 48, 255);
    sc_screen_fill_rounded_rect(screen->display.renderer, &screen->settings_menu_rect,
                                screen->settings_menu_rect.h / 8);

    sc_screen_draw_settings_menu_item(screen, &screen->settings_menu_copy_rect,
                                      UI_SETTINGS_COPY_LABEL,
                                      screen->screenshot_action
                                          == SC_SCREENSHOT_ACTION_COPY_TO_CLIPBOARD,
                                      screen->settings_menu_copy_hovered);

    sc_screen_draw_settings_menu_item(screen, &screen->settings_menu_save_rect,
                                      UI_SETTINGS_SAVE_LABEL,
                                      screen->screenshot_action
                                          == SC_SCREENSHOT_ACTION_SAVE_TO_DIRECTORY,
                                      screen->settings_menu_save_hovered);

    const char *folder_label = screen->screenshot_directory[0]
                             ? UI_SETTINGS_FOLDER_SET_LABEL
                             : UI_SETTINGS_FOLDER_LABEL;
    sc_screen_draw_settings_menu_item(screen, &screen->settings_menu_directory_rect,
                                      folder_label, false,
                                      screen->settings_menu_directory_hovered);
}
#endif

static void
sc_screen_close_settings_menu(struct sc_screen *screen) {
    screen->settings_menu_open = false;
    screen->settings_menu_copy_hovered = false;
    screen->settings_menu_save_hovered = false;
    screen->settings_menu_directory_hovered = false;
}

static uint8_t
sc_color_lerp(uint8_t from, uint8_t to, float t) {
    float mixed = from + (to - from) * t;
    mixed = CLAMP(mixed, 0.0f, 255.0f);
    return (uint8_t) mixed;
}

static void
sc_screen_fill_circle(SDL_Renderer *renderer, int cx, int cy, int radius) {
    for (int y = -radius; y <= radius; ++y) {
        int dx = radius;
        while (dx > 0 && dx * dx + y * y > radius * radius) {
            --dx;
        }
        SDL_Rect row = {
            .x = cx - dx,
            .y = cy + y,
            .w = 2 * dx + 1,
            .h = 1,
        };
        SDL_RenderFillRect(renderer, &row);
    }
}

static void
sc_screen_fill_rounded_rect(SDL_Renderer *renderer, const SDL_Rect *rect,
                            int radius) {
    radius = CLAMP(radius, 0, MIN(rect->w, rect->h) / 2);
    if (!radius) {
        SDL_RenderFillRect(renderer, rect);
        return;
    }

    SDL_Rect middle = {
        .x = rect->x + radius,
        .y = rect->y,
        .w = rect->w - 2 * radius,
        .h = rect->h,
    };
    SDL_Rect left = {
        .x = rect->x,
        .y = rect->y + radius,
        .w = radius,
        .h = rect->h - 2 * radius,
    };
    SDL_Rect right = {
        .x = rect->x + rect->w - radius,
        .y = rect->y + radius,
        .w = radius,
        .h = rect->h - 2 * radius,
    };
    SDL_RenderFillRect(renderer, &middle);
    SDL_RenderFillRect(renderer, &left);
    SDL_RenderFillRect(renderer, &right);

    sc_screen_fill_circle(renderer, rect->x + radius, rect->y + radius, radius);
    sc_screen_fill_circle(renderer, rect->x + rect->w - radius - 1,
                          rect->y + radius, radius);
    sc_screen_fill_circle(renderer, rect->x + radius,
                          rect->y + rect->h - radius - 1, radius);
    sc_screen_fill_circle(renderer, rect->x + rect->w - radius - 1,
                          rect->y + rect->h - radius - 1, radius);
}

static void
sc_screen_draw_panel(struct sc_screen *screen) {
    SDL_Renderer *renderer = screen->display.renderer;
    if (!screen->panel_rect.w) {
        return;
    }

    SDL_SetRenderDrawColor(renderer, 50, 50, 50, 255);
    SDL_RenderFillRect(renderer, &screen->panel_rect);

    SDL_Rect button = screen->screenshot_button_rect;
    bool enabled = screen->has_frame;

    uint8_t r = 196;
    uint8_t g = 197;
    uint8_t b = 201;
    if (enabled) {
        r = 213;
        g = 214;
        b = 217;
        if (screen->screenshot_button_pressed) {
            r = 184;
            g = 186;
            b = 191;
        } else if (screen->screenshot_button_hovered) {
            r = 206;
            g = 207;
            b = 211;
        }
    }

    float feedback = sc_screen_get_screenshot_button_feedback_progress(screen);
    if (feedback > 0.0f) {
        r = sc_color_lerp(r, 29, feedback);
        g = sc_color_lerp(g, 177, feedback);
        b = sc_color_lerp(b, 89, feedback);
    }

    if (screen->screenshot_button_bg) {
        SDL_SetTextureColorMod(screen->screenshot_button_bg, r, g, b);
        SDL_RenderCopy(renderer, screen->screenshot_button_bg, NULL, &button);
    } else {
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        sc_screen_fill_rounded_rect(renderer, &button, button.w / 2);
    }
    sc_screen_draw_button_icon(screen, &button);

    SDL_Rect toggle = screen->input_toggle_button_rect;
    uint8_t tr;
    uint8_t tg;
    uint8_t tb;
    if (screen->input_enabled) {
        tr = 255;
        tg = 199;
        tb = 0;
        if (screen->input_toggle_button_pressed) {
            tr = 242;
            tg = 186;
            tb = 0;
        } else if (screen->input_toggle_button_hovered) {
            tr = 255;
            tg = 210;
            tb = 38;
        }
    } else {
        tr = 217;
        tg = 217;
        tb = 217;
        if (screen->input_toggle_button_pressed) {
            tr = 201;
            tg = 201;
            tb = 201;
        } else if (screen->input_toggle_button_hovered) {
            tr = 229;
            tg = 229;
            tb = 229;
        }
    }

    if (screen->input_toggle_button_bg) {
        SDL_SetTextureColorMod(screen->input_toggle_button_bg, tr, tg, tb);
        SDL_RenderCopy(renderer, screen->input_toggle_button_bg, NULL, &toggle);
    } else {
        SDL_SetRenderDrawColor(renderer, tr, tg, tb, 255);
        sc_screen_fill_rounded_rect(renderer, &toggle, toggle.w / 2);
    }
    sc_screen_draw_toggle_icon(screen, &toggle);

    SDL_Rect settings = screen->settings_button_rect;
    uint8_t sr = 217;
    uint8_t sg = 217;
    uint8_t sb = 217;
    if (screen->settings_button_pressed) {
        sr = 201;
        sg = 201;
        sb = 201;
    } else if (screen->settings_button_hovered || screen->settings_menu_open) {
        sr = 229;
        sg = 229;
        sb = 229;
    }

    if (screen->input_toggle_button_bg) {
        SDL_SetTextureColorMod(screen->input_toggle_button_bg, sr, sg, sb);
        SDL_RenderCopy(renderer, screen->input_toggle_button_bg, NULL, &settings);
    } else {
        SDL_SetRenderDrawColor(renderer, sr, sg, sb, 255);
        sc_screen_fill_rounded_rect(renderer, &settings, settings.w / 2);
    }
    sc_screen_draw_settings_icon(screen, &settings);

#ifndef __APPLE__
    sc_screen_draw_settings_menu(screen);
#endif
}

static enum sc_display_result
sc_screen_draw_video(struct sc_screen *screen, bool update_content_rect) {
    assert(screen->video);

    if (update_content_rect) {
        sc_screen_update_content_rect(screen);
    }

    SDL_SetRenderDrawColor(screen->display.renderer, 28, 28, 28, 255);
    enum sc_display_result res = sc_display_render(&screen->display,
                                                   &screen->rect,
                                                   screen->orientation);
    if (res == SC_DISPLAY_RESULT_OK) {
        if (screen->secure_content_detected) {
            int label_h = MAX(12, scale_window_to_drawable(screen, 16, false));
            SDL_Rect label_area = {
                .x = 0,
                .y = screen->rect.y + screen->rect.h / 2 - label_h / 2,
                .w = screen->panel_rect.x,
                .h = label_h,
            };
            label_area.y = CLAMP(label_area.y, 0,
                                 MAX(0, screen->panel_rect.h - label_h));
            sc_screen_draw_text_centered(screen->display.renderer, &label_area,
                                         UI_SECURE_LABEL, 255, 255, 255);
        }
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
    SDL_SetRenderDrawColor(renderer, 28, 28, 28, 255);
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

    if (!screen->window_focused) {
        return;
    }

    if (screen->has_frame) {
        sc_screen_render(screen, update_content);
    } else {
        sc_screen_render_idle(screen);
    }
}

static void
sc_screen_set_input_enabled(struct sc_screen *screen, bool enabled) {
    if (screen->input_enabled == enabled) {
        return;
    }

    screen->input_enabled = enabled;

    bool capture_active = enabled && sc_screen_is_relative_mode(screen)
                       && (!screen->video || screen->has_frame)
                       && screen->window_focused;
    sc_mouse_capture_set_active(&screen->mc, capture_active);
}

static void
sc_screen_animate_screenshot_button_feedback(struct sc_screen *screen) {
    if (!screen->video || !screen->panel_rect.w) {
        return;
    }

    screen->screenshot_button_feedback_active = true;
    screen->screenshot_button_feedback_start_ms = SDL_GetTicks();
    while (true) {
        uint32_t now = SDL_GetTicks();
        uint32_t elapsed = now - screen->screenshot_button_feedback_start_ms;
        if (elapsed >= UI_BUTTON_FEEDBACK_DURATION_MS) {
            break;
        }
        screen->screenshot_button_feedback_progress =
            sc_screen_get_screenshot_button_feedback_progress(screen);
        sc_screen_render_current_state(screen, false);
        SDL_Delay(16);
    }
    screen->screenshot_button_feedback_active = false;
    screen->screenshot_button_feedback_progress = 0.0f;
    sc_screen_render_current_state(screen, false);
}

static bool
sc_screen_capture_screenshot_rgba(struct sc_screen *screen, uint8_t **pixels_out,
                                  size_t *pitch_out, int *width_out,
                                  int *height_out) {
    assert(screen->video);
    assert(pixels_out && pitch_out && width_out && height_out);

    if (!screen->has_frame || !screen->frame) {
        LOGW("No video frame available to capture");
        return false;
    }

    int width = screen->frame->width;
    int height = screen->frame->height;
    if (width <= 0 || height <= 0) {
        LOGW("Invalid screenshot size");
        return false;
    }

    if ((size_t) width > SIZE_MAX / 4u) {
        LOGW("Screenshot size is too large");
        return false;
    }

    size_t pitch = (size_t) width * 4;
    if ((size_t) height > SIZE_MAX / pitch) {
        LOGW("Screenshot buffer is too large");
        return false;
    }

    size_t size = (size_t) height * pitch;
    uint8_t *pixels = malloc(size);
    if (!pixels) {
        LOG_OOM();
        return false;
    }

    struct SwsContext *sws_ctx =
        sws_getContext(width, height, screen->frame->format,
                       width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                       NULL, NULL, NULL);
    if (!sws_ctx) {
        free(pixels);
        LOGW("Could not initialize conversion context for screenshot");
        return false;
    }

    uint8_t *dst_data[4] = {pixels, NULL, NULL, NULL};
    int dst_linesize[4] = {(int) pitch, 0, 0, 0};
    int ret = sws_scale(sws_ctx,
                        (const uint8_t * const *) screen->frame->data,
                        screen->frame->linesize,
                        0, height,
                        dst_data, dst_linesize);
    sws_freeContext(sws_ctx);

    if (ret <= 0) {
        free(pixels);
        LOGW("Could not convert frame for screenshot");
        return false;
    }

    *pixels_out = pixels;
    *pitch_out = pitch;
    *width_out = width;
    *height_out = height;
    return true;
}

static bool
sc_screen_copy_screenshot_to_clipboard(struct sc_screen *screen) {
    assert(screen->video);

    uint8_t *pixels = NULL;
    size_t pitch = 0;
    int width = 0;
    int height = 0;
    bool prepared =
        sc_screen_capture_screenshot_rgba(screen, &pixels, &pitch, &width,
                                          &height);
    if (!prepared) {
        return false;
    }

    bool ok = false;
#ifdef __APPLE__
    ok = sc_darwin_clipboard_set_image_rgba8888(pixels, pitch, width, height);
    if (!ok) {
        LOGW("Could not copy screenshot image to the macOS clipboard");
    }
#else
    LOGW("Screenshot clipboard image is only implemented on macOS");
#endif
    free(pixels);

    if (ok) {
        LOGI("Screenshot copied to clipboard (%dx%d)", width, height);
    }
    return ok;
}

static bool
sc_screen_choose_screenshot_directory(struct sc_screen *screen) {
#ifdef __APPLE__
    char selected[sizeof(screen->screenshot_directory)] = {0};
    bool ok = sc_darwin_choose_directory(selected, sizeof(selected));
    if (!ok) {
        return false;
    }
    snprintf(screen->screenshot_directory, sizeof(screen->screenshot_directory),
             "%s", selected);
    return true;
#else
    (void) screen;
    return false;
#endif
}

static bool
sc_screen_save_screenshot_to_directory(struct sc_screen *screen) {
    assert(screen->video);

#ifndef __APPLE__
    LOGW("Saving screenshots to files is only implemented on macOS");
    return false;
#else
    if (!screen->screenshot_directory[0]
            && !sc_screen_choose_screenshot_directory(screen)) {
        return false;
    }

    uint8_t *pixels = NULL;
    size_t pitch = 0;
    int width = 0;
    int height = 0;
    bool prepared =
        sc_screen_capture_screenshot_rgba(screen, &pixels, &pitch, &width,
                                          &height);
    if (!prepared) {
        return false;
    }

    time_t now = time(NULL);
    struct tm local_tm = {0};
#ifdef _WIN32
    localtime_s(&local_tm, &now);
#else
    localtime_r(&now, &local_tm);
#endif
    uint32_t millis = SDL_GetTicks() % 1000;

    char filename[128];
    snprintf(filename, sizeof(filename),
             "screenshot_%04d%02d%02d_%02d%02d%02d_%03u_%dx%d.png",
             local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
             local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
             (unsigned) millis, width, height);

    char output_path[sizeof(screen->screenshot_directory) + sizeof(filename) + 2];
    int written = snprintf(output_path, sizeof(output_path), "%s/%s",
                           screen->screenshot_directory, filename);
    if (written < 0 || (size_t) written >= sizeof(output_path)) {
        free(pixels);
        LOGW("Screenshot output path is too long");
        return false;
    }

    bool ok = sc_darwin_write_png_rgba8888(output_path, pixels, pitch, width,
                                           height);
    free(pixels);

    if (!ok) {
        LOGW("Could not save screenshot to %s", output_path);
        return false;
    }

    LOGI("Screenshot saved to %s", output_path);
    return true;
#endif
}

static bool
sc_screen_take_screenshot(struct sc_screen *screen, bool force_clipboard) {
    enum sc_screenshot_action action = screen->screenshot_action;
    if (force_clipboard) {
        action = SC_SCREENSHOT_ACTION_COPY_TO_CLIPBOARD;
    }

    bool ok;
    switch (action) {
        case SC_SCREENSHOT_ACTION_COPY_TO_CLIPBOARD:
            ok = sc_screen_copy_screenshot_to_clipboard(screen);
            break;
        case SC_SCREENSHOT_ACTION_SAVE_TO_DIRECTORY:
            ok = sc_screen_save_screenshot_to_directory(screen);
            break;
        default:
            ok = false;
            break;
    }

    if (ok) {
        sc_screen_animate_screenshot_button_feedback(screen);
    }
    return ok;
}

static bool
sc_screen_handle_panel_event(struct sc_screen *screen, const SDL_Event *event) {
    assert(screen->video);

    sc_screen_update_ui_rects(screen);
    if (!screen->panel_rect.w) {
        sc_screen_close_settings_menu(screen);
        return false;
    }

    switch (event->type) {
        case SDL_MOUSEMOTION: {
            int32_t x = event->motion.x;
            int32_t y = event->motion.y;
            sc_screen_hidpi_scale_coords(screen, &x, &y);
            bool in_button = screen->has_frame
                          && point_in_rect(x, y, &screen->screenshot_button_rect);
            bool in_toggle = point_in_rect(x, y, &screen->input_toggle_button_rect);
            bool in_settings = point_in_rect(x, y, &screen->settings_button_rect);
            bool in_panel = point_in_rect(x, y, &screen->panel_rect);
            bool in_menu = screen->settings_menu_open
                        && point_in_rect(x, y, &screen->settings_menu_rect);
            bool in_menu_copy = in_menu
                             && point_in_rect(x, y,
                                              &screen->settings_menu_copy_rect);
            bool in_menu_save = in_menu
                             && point_in_rect(x, y,
                                              &screen->settings_menu_save_rect);
            bool in_menu_dir = in_menu
                            && point_in_rect(x, y,
                                             &screen->settings_menu_directory_rect);

            if (in_button != screen->screenshot_button_hovered
                    || in_toggle != screen->input_toggle_button_hovered
                    || in_settings != screen->settings_button_hovered
                    || in_menu_copy != screen->settings_menu_copy_hovered
                    || in_menu_save != screen->settings_menu_save_hovered
                    || in_menu_dir != screen->settings_menu_directory_hovered) {
                screen->screenshot_button_hovered = in_button;
                screen->input_toggle_button_hovered = in_toggle;
                screen->settings_button_hovered = in_settings;
                screen->settings_menu_copy_hovered = in_menu_copy;
                screen->settings_menu_save_hovered = in_menu_save;
                screen->settings_menu_directory_hovered = in_menu_dir;
                if (!in_button) {
                    screen->screenshot_button_pressed = false;
                }
                if (!in_toggle) {
                    screen->input_toggle_button_pressed = false;
                }
                if (!in_settings) {
                    screen->settings_button_pressed = false;
                }
                sc_screen_render_current_state(screen, false);
            }
            if (screen->settings_menu_open) {
                return true;
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
            bool in_toggle = point_in_rect(x, y, &screen->input_toggle_button_rect);
            bool in_settings = point_in_rect(x, y, &screen->settings_button_rect);
            bool in_menu = screen->settings_menu_open
                        && point_in_rect(x, y, &screen->settings_menu_rect);
            bool in_menu_copy = in_menu
                             && point_in_rect(x, y,
                                              &screen->settings_menu_copy_rect);
            bool in_menu_save = in_menu
                             && point_in_rect(x, y,
                                              &screen->settings_menu_save_rect);
            bool in_menu_dir = in_menu
                            && point_in_rect(x, y,
                                             &screen->settings_menu_directory_rect);

            if (event->button.button == SDL_BUTTON_LEFT) {
                bool down = event->type == SDL_MOUSEBUTTONDOWN;
                if (down && in_button) {
                    screen->screenshot_button_pressed = true;
                    if (screen->settings_menu_open) {
                        sc_screen_close_settings_menu(screen);
                    }
                    sc_screen_render_current_state(screen, false);
                    return true;
                }
                if (down && in_toggle) {
                    screen->input_toggle_button_pressed = true;
                    if (screen->settings_menu_open) {
                        sc_screen_close_settings_menu(screen);
                    }
                    sc_screen_render_current_state(screen, false);
                    return true;
                }
                if (down && in_settings) {
                    screen->settings_button_pressed = true;
                    sc_screen_render_current_state(screen, false);
                    return true;
                }
                if (down && screen->settings_menu_open) {
                    return true;
                }

                if (!down && screen->screenshot_button_pressed) {
                    bool activate = in_button;
                    screen->screenshot_button_pressed = false;
                    sc_screen_render_current_state(screen, false);
                    if (activate) {
                        sc_screen_take_screenshot(screen, false);
                    }
                    return true;
                }
                if (!down && screen->input_toggle_button_pressed) {
                    bool activate = in_toggle;
                    screen->input_toggle_button_pressed = false;
                    if (activate) {
                        sc_screen_set_input_enabled(screen, !screen->input_enabled);
                    }
                    sc_screen_render_current_state(screen, false);
                    return true;
                }
                if (!down && screen->settings_button_pressed) {
                    bool activate = in_settings;
                    screen->settings_button_pressed = false;
                    if (activate) {
#ifdef __APPLE__
                        bool save_selected =
                            screen->screenshot_action
                                == SC_SCREENSHOT_ACTION_SAVE_TO_DIRECTORY
                            && screen->screenshot_directory[0] != '\0';
                        enum sc_darwin_settings_menu_action action =
                            sc_darwin_window_show_settings_menu(
                                screen->window, event->button.x,
                                event->button.y,
                                save_selected,
                                screen->screenshot_directory[0]
                                    ? screen->screenshot_directory
                                    : NULL);
                        switch (action) {
                            case SC_DARWIN_SETTINGS_MENU_ACTION_COPY_TO_CLIPBOARD:
                                screen->screenshot_action =
                                    SC_SCREENSHOT_ACTION_COPY_TO_CLIPBOARD;
                                break;
                            case SC_DARWIN_SETTINGS_MENU_ACTION_SAVE_TO_DIRECTORY:
                                if (screen->screenshot_directory[0]
                                        || sc_screen_choose_screenshot_directory(
                                               screen)) {
                                    screen->screenshot_action =
                                        SC_SCREENSHOT_ACTION_SAVE_TO_DIRECTORY;
                                } else {
                                    screen->screenshot_action =
                                        SC_SCREENSHOT_ACTION_COPY_TO_CLIPBOARD;
                                }
                                break;
                            case SC_DARWIN_SETTINGS_MENU_ACTION_NONE:
                            default:
                                break;
                        }
#else
                        screen->settings_menu_open = !screen->settings_menu_open;
                        if (!screen->settings_menu_open) {
                            sc_screen_close_settings_menu(screen);
                        }
#endif
                    }
                    sc_screen_render_current_state(screen, false);
                    return true;
                }

                if (!down && screen->settings_menu_open) {
                    bool should_render = true;
                    if (in_menu_copy) {
                        screen->screenshot_action =
                            SC_SCREENSHOT_ACTION_COPY_TO_CLIPBOARD;
                        sc_screen_close_settings_menu(screen);
                    } else if (in_menu_save) {
                        screen->screenshot_action =
                            SC_SCREENSHOT_ACTION_SAVE_TO_DIRECTORY;
                        sc_screen_close_settings_menu(screen);
                    } else if (in_menu_dir) {
                        sc_screen_choose_screenshot_directory(screen);
                        sc_screen_close_settings_menu(screen);
                    } else {
                        sc_screen_close_settings_menu(screen);
                    }

                    if (should_render) {
                        sc_screen_render_current_state(screen, false);
                    }
                    return true;
                }
            }

            if (screen->settings_menu_open) {
                return true;
            }
            return in_panel || in_menu;
        }
        case SDL_MOUSEWHEEL: {
            int32_t x;
            int32_t y;
            SDL_GetMouseState(&x, &y);
            sc_screen_hidpi_scale_coords(screen, &x, &y);
            if (screen->settings_menu_open) {
                return true;
            }
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
    screen->input_toggle_button_rect = (SDL_Rect) {0, 0, 0, 0};
    screen->settings_button_rect = (SDL_Rect) {0, 0, 0, 0};
    screen->settings_menu_rect = (SDL_Rect) {0, 0, 0, 0};
    screen->settings_menu_copy_rect = (SDL_Rect) {0, 0, 0, 0};
    screen->settings_menu_save_rect = (SDL_Rect) {0, 0, 0, 0};
    screen->settings_menu_directory_rect = (SDL_Rect) {0, 0, 0, 0};
    screen->screenshot_button_bg = NULL;
    screen->screenshot_button_bg_width = 0;
    screen->screenshot_button_bg_height = 0;
    screen->input_toggle_button_bg = NULL;
    screen->input_toggle_button_bg_width = 0;
    screen->input_toggle_button_bg_height = 0;
    screen->screenshot_icon = NULL;
    screen->screenshot_icon_width = 0;
    screen->screenshot_icon_height = 0;
    screen->screenshot_check_icon = NULL;
    screen->screenshot_check_icon_width = 0;
    screen->screenshot_check_icon_height = 0;
    screen->input_toggle_icon = NULL;
    screen->input_toggle_icon_width = 0;
    screen->input_toggle_icon_height = 0;
    screen->settings_icon = NULL;
    screen->settings_icon_width = 0;
    screen->settings_icon_height = 0;
    screen->screenshot_button_hovered = false;
    screen->screenshot_button_pressed = false;
    screen->input_toggle_button_hovered = false;
    screen->input_toggle_button_pressed = false;
    screen->settings_button_hovered = false;
    screen->settings_button_pressed = false;
    screen->settings_menu_open = false;
    screen->settings_menu_copy_hovered = false;
    screen->settings_menu_save_hovered = false;
    screen->settings_menu_directory_hovered = false;
    screen->input_enabled = false;
    screen->screenshot_action = SC_SCREENSHOT_ACTION_COPY_TO_CLIPBOARD;
    screen->screenshot_directory[0] = '\0';
    screen->screenshot_button_feedback_active = false;
    screen->screenshot_button_feedback_start_ms = 0;
    screen->screenshot_button_feedback_progress = 0.0f;
    screen->window_focused = true;
    screen->secure_content_detected = false;
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
    int width = params->video ? 800 : 256;
    int height = params->video ? 600 : 256;
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

#ifdef __APPLE__
    if (!params->window_borderless) {
        bool native_ok =
            sc_darwin_window_configure_native_chrome(screen->window);
        if (!native_ok) {
            LOGW("Could not configure native macOS window chrome");
        }
    }
#endif

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

    sc_screen_load_screenshot_button_bg(screen);
    sc_screen_load_input_toggle_button_bg(screen);
    sc_screen_load_screenshot_icon(screen);
    sc_screen_load_screenshot_check_icon(screen);
    sc_screen_load_input_toggle_icon(screen);
    sc_screen_load_settings_icon(screen);

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
    } else if (screen->input_enabled && sc_screen_is_relative_mode(screen)) {
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

    struct sc_size window_size;
    if (!screen->req.width && !screen->req.height) {
        // Keep the initial default startup size on first connection.
        window_size = get_window_size(screen);
    } else {
        window_size = get_initial_optimal_size(screen->content_size,
                                               screen->req.width,
                                               screen->req.height);
        uint32_t width = (uint32_t) window_size.width + UI_PANEL_WIDTH;
        window_size.width = MIN(width, 0x7FFF);
    }

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
    if (screen->screenshot_button_bg) {
        SDL_DestroyTexture(screen->screenshot_button_bg);
    }
    if (screen->input_toggle_button_bg) {
        SDL_DestroyTexture(screen->input_toggle_button_bg);
    }
    if (screen->screenshot_icon) {
        SDL_DestroyTexture(screen->screenshot_icon);
    }
    if (screen->screenshot_check_icon) {
        SDL_DestroyTexture(screen->screenshot_check_icon);
    }
    if (screen->input_toggle_icon) {
        SDL_DestroyTexture(screen->input_toggle_icon);
    }
    if (screen->settings_icon) {
        SDL_DestroyTexture(screen->settings_icon);
    }
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

        if (screen->input_enabled && sc_screen_is_relative_mode(screen)) {
            // Capture mouse on start
            sc_mouse_capture_set_active(&screen->mc, true);
        }
    }

    if (screen->window_focused) {
        sc_screen_render(screen, false);
    }
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
        screen->secure_content_detected = false;
        screen->screenshot_button_hovered = false;
        screen->screenshot_button_pressed = false;
        screen->input_toggle_button_hovered = false;
        screen->input_toggle_button_pressed = false;
        screen->settings_button_hovered = false;
        screen->settings_button_pressed = false;
        sc_screen_close_settings_menu(screen);
        screen->screenshot_button_feedback_active = false;
        screen->screenshot_button_feedback_progress = 0.0f;

        if (sc_screen_is_relative_mode(screen)) {
            sc_mouse_capture_set_active(&screen->mc, false);
        }

        if (screen->window_focused) {
            sc_screen_render_idle(screen);
        }
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
        bool active = screen->input_enabled && relative_mode
                   && (!screen->video || screen->has_frame)
                   && screen->window_focused;
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

static bool
sc_screen_is_control_event(const SDL_Event *event) {
    switch (event->type) {
        case SDL_TEXTINPUT:
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEWHEEL:
        case SDL_FINGERMOTION:
        case SDL_FINGERDOWN:
        case SDL_FINGERUP:
        case SDL_CONTROLLERDEVICEADDED:
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_CONTROLLERAXISMOTION:
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_DROPFILE:
            return true;
        default:
            return false;
    }
}

static bool
sc_screen_is_copy_screenshot_shortcut(const SDL_Event *event) {
    if (event->type != SDL_KEYDOWN || event->key.repeat) {
        return false;
    }

    SDL_Keycode keycode = event->key.keysym.sym;
    if (keycode != SDLK_c) {
        return false;
    }

    SDL_Keymod mods = event->key.keysym.mod;
    return (mods & KMOD_GUI) || (mods & KMOD_CTRL);
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
        case SC_EVENT_SCREEN_SECURE_CONTENT: {
            bool detected = event->user.code != 0;
            if (screen->secure_content_detected != detected) {
                screen->secure_content_detected = detected;
                if (screen->video) {
                    sc_screen_render_current_state(screen, false);
                }
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
                case SDL_WINDOWEVENT_FOCUS_GAINED:
                    screen->window_focused = true;
                    if (screen->input_enabled && sc_screen_is_relative_mode(screen)
                            && screen->has_frame) {
                        sc_mouse_capture_set_active(&screen->mc, true);
                    }
                    sc_screen_render_current_state(screen, true);
                    break;
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    screen->window_focused = false;
                    if (sc_screen_is_relative_mode(screen)) {
                        sc_mouse_capture_set_active(&screen->mc, false);
                    }
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

    if (screen->video && !screen->input_enabled
            && sc_screen_is_copy_screenshot_shortcut(event)) {
        sc_screen_take_screenshot(screen, true);
        return true;
    }

    if (screen->video && !screen->input_enabled && sc_screen_is_control_event(event)) {
        return true;
    }

    if (screen->input_enabled
            && sc_screen_is_relative_mode(screen)
            && sc_mouse_capture_handle_event(&screen->mc, event)) {
        // The mouse capture handler consumed the event
        return true;
    }

    if (screen->input_enabled || !sc_screen_is_control_event(event)) {
        sc_input_manager_handle_event(&screen->im, event);
    }
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
