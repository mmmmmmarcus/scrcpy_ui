#include "sys/darwin/font.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#import <CoreText/CoreText.h>

SDL_Texture *
sc_darwin_font_create_text_texture(SDL_Renderer *renderer,
                                   const char *font_path,
                                   const char *text,
                                   uint8_t r, uint8_t g, uint8_t b,
                                   uint16_t point_size,
                                   uint16_t *out_width,
                                   uint16_t *out_height) {
    if (!renderer || !font_path || !*font_path || !text || !*text
            || !out_width || !out_height) {
        return NULL;
    }

    @autoreleasepool {
        CFStringRef path_string =
            CFStringCreateWithCString(NULL, font_path, kCFStringEncodingUTF8);
        if (!path_string) {
            return NULL;
        }

        CFURLRef font_url = CFURLCreateWithFileSystemPath(
            NULL, path_string, kCFURLPOSIXPathStyle, false);
        CFRelease(path_string);
        if (!font_url) {
            return NULL;
        }

        CGDataProviderRef provider = CGDataProviderCreateWithURL(font_url);
        CFRelease(font_url);
        if (!provider) {
            return NULL;
        }

        CGFontRef cg_font = CGFontCreateWithDataProvider(provider);
        CGDataProviderRelease(provider);
        if (!cg_font) {
            return NULL;
        }

        CTFontRef ct_font =
            CTFontCreateWithGraphicsFont(cg_font, (CGFloat) point_size,
                                         NULL, NULL);
        CGFontRelease(cg_font);
        if (!ct_font) {
            return NULL;
        }

        CFStringRef text_cf =
            CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
        if (!text_cf) {
            CFRelease(ct_font);
            return NULL;
        }

        CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
        CGFloat color_components[4] = {
            r / 255.0f,
            g / 255.0f,
            b / 255.0f,
            1.0f,
        };
        CGColorRef color = CGColorCreate(color_space, color_components);
        CGColorSpaceRelease(color_space);
        if (!color) {
            CFRelease(text_cf);
            CFRelease(ct_font);
            return NULL;
        }

        const void *keys[2] = {kCTFontAttributeName,
                               kCTForegroundColorAttributeName};
        const void *values[2] = {ct_font, color};
        CFDictionaryRef attributes = CFDictionaryCreate(
            NULL, keys, values, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CGColorRelease(color);
        CFRelease(ct_font);
        if (!attributes) {
            CFRelease(text_cf);
            return NULL;
        }

        CFAttributedStringRef attributed =
            CFAttributedStringCreate(NULL, text_cf, attributes);
        CFRelease(attributes);
        CFRelease(text_cf);
        if (!attributed) {
            return NULL;
        }

        CTLineRef line = CTLineCreateWithAttributedString(attributed);
        CFRelease(attributed);
        if (!line) {
            return NULL;
        }

        CGFloat ascent = 0.0;
        CGFloat descent = 0.0;
        CGFloat leading = 0.0;
        double width_double =
            CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
        int width = MAX(1, (int) ceil(width_double));
        int height = MAX(1, (int) ceil(ascent + descent + leading));

        size_t pitch = (size_t) width * 4;
        uint8_t *pixels = calloc((size_t) height, pitch);
        if (!pixels) {
            CFRelease(line);
            return NULL;
        }

        CGColorSpaceRef draw_space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context =
            CGBitmapContextCreate(pixels, (size_t) width, (size_t) height, 8,
                                  pitch, draw_space,
                                  kCGImageAlphaPremultipliedLast
                                  | kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(draw_space);
        if (!context) {
            free(pixels);
            CFRelease(line);
            return NULL;
        }

        CGContextSetShouldAntialias(context, true);
        CGContextSetAllowsAntialiasing(context, true);
        CGContextSetTextPosition(context, 0.0, descent);
        CTLineDraw(line, context);
        CGContextRelease(context);
        CFRelease(line);

        SDL_Texture *texture =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_STATIC, width, height);
        if (!texture) {
            free(pixels);
            return NULL;
        }

        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        if (SDL_UpdateTexture(texture, NULL, pixels, (int) pitch)) {
            SDL_DestroyTexture(texture);
            free(pixels);
            return NULL;
        }
        free(pixels);

        *out_width = (uint16_t) CLAMP(width, 0, 0xFFFF);
        *out_height = (uint16_t) CLAMP(height, 0, 0xFFFF);
        return texture;
    }
}
