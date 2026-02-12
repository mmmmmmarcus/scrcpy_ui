#ifndef SC_DARWIN_FONT_H
#define SC_DARWIN_FONT_H

#include "common.h"

#include <stdint.h>

typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

SDL_Texture *
sc_darwin_font_create_text_texture(SDL_Renderer *renderer,
                                   const char *font_path,
                                   const char *text,
                                   uint8_t r, uint8_t g, uint8_t b,
                                   uint16_t point_size,
                                   uint16_t *out_width,
                                   uint16_t *out_height);

#endif
