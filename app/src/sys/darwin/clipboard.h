#ifndef SC_DARWIN_CLIPBOARD_H
#define SC_DARWIN_CLIPBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool
sc_darwin_clipboard_set_image_rgba8888(const uint8_t *data, size_t pitch,
                                       uint16_t width, uint16_t height);

bool
sc_darwin_choose_directory(char *path, size_t path_size);

bool
sc_darwin_write_png_rgba8888(const char *path, const uint8_t *data,
                             size_t pitch, uint16_t width, uint16_t height);

#endif
