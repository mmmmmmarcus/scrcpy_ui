#ifndef SC_DARWIN_WINDOW_H
#define SC_DARWIN_WINDOW_H

#include <stdbool.h>

typedef struct SDL_Window SDL_Window;

bool
sc_darwin_window_configure_native_chrome(SDL_Window *window);

#endif
