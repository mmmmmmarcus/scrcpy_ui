#ifndef SC_ICON_H
#define SC_ICON_H

#include "common.h"

#include <SDL2/SDL_surface.h>

SDL_Surface *
scrcpy_icon_load(void);

SDL_Surface *
scrcpy_icon_load_from_path(const char *path);

void
scrcpy_icon_destroy(SDL_Surface *icon);

#endif
