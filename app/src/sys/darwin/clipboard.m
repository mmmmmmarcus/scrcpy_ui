#include "sys/darwin/clipboard.h"

#import <AppKit/AppKit.h>

#include <string.h>

bool
sc_darwin_clipboard_set_image_rgba8888(const uint8_t *data, size_t pitch,
                                       uint16_t width, uint16_t height) {
    @autoreleasepool {
        NSBitmapImageRep *rep =
            [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                    pixelsWide:width
                                                    pixelsHigh:height
                                                 bitsPerSample:8
                                               samplesPerPixel:4
                                                      hasAlpha:YES
                                                      isPlanar:NO
                                                colorSpaceName:NSDeviceRGBColorSpace
                                                   bytesPerRow:width * 4
                                                  bitsPerPixel:32];
        if (!rep) {
            return false;
        }

        unsigned char *dest = [rep bitmapData];
        if (!dest) {
            [rep release];
            return false;
        }

        size_t dest_pitch = (size_t) width * 4;
        for (uint16_t y = 0; y < height; ++y) {
            memcpy(dest + (size_t) y * dest_pitch, data + (size_t) y * pitch,
                   dest_pitch);
        }

        NSImage *image =
            [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
        [image addRepresentation:rep];

        NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
        [pasteboard clearContents];
        BOOL ok = [pasteboard
            writeObjects:[NSArray arrayWithObject:image]];

        [image release];
        [rep release];
        return ok == YES;
    }
}
