#ifndef SC_DARWIN_WINDOW_H
#define SC_DARWIN_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SDL_Window SDL_Window;

enum sc_darwin_settings_menu_action {
    SC_DARWIN_SETTINGS_MENU_ACTION_NONE = 0,
    SC_DARWIN_SETTINGS_MENU_ACTION_COPY_TO_CLIPBOARD = 1,
    SC_DARWIN_SETTINGS_MENU_ACTION_SAVE_TO_DIRECTORY = 2,
};

bool
sc_darwin_window_configure_native_chrome(SDL_Window *window);

enum sc_darwin_settings_menu_action
sc_darwin_window_show_settings_menu(SDL_Window *window, int32_t x, int32_t y,
                                    bool save_selected,
                                    const char *save_directory);

#endif
