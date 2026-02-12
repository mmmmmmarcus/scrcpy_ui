#include "sys/darwin/clipboard.h"

#import <AppKit/AppKit.h>

#include <string.h>

static NSBitmapImageRep *
sc_darwin_create_image_rep_rgba8888(const uint8_t *data, size_t pitch,
                                    uint16_t width, uint16_t height) {
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
        return nil;
    }

    unsigned char *dest = [rep bitmapData];
    if (!dest) {
        [rep release];
        return nil;
    }

    size_t dest_pitch = (size_t) width * 4;
    for (uint16_t y = 0; y < height; ++y) {
        memcpy(dest + (size_t) y * dest_pitch, data + (size_t) y * pitch,
               dest_pitch);
    }

    return rep;
}

bool
sc_darwin_clipboard_set_image_rgba8888(const uint8_t *data, size_t pitch,
                                       uint16_t width, uint16_t height) {
    @autoreleasepool {
        NSBitmapImageRep *rep =
            sc_darwin_create_image_rep_rgba8888(data, pitch, width, height);
        if (!rep) {
            return false;
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

bool
sc_darwin_choose_directory(char *path, size_t path_size) {
    if (!path || path_size < 2) {
        return false;
    }

    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.canCreateDirectories = YES;
        panel.prompt = @"Select";
        panel.title = @"Choose screenshot folder";

        NSInteger result = [panel runModal];
        if (result != NSModalResponseOK) {
            return false;
        }

        NSURL *url = panel.URL;
        if (!url) {
            return false;
        }

        const char *selected = [[url path] fileSystemRepresentation];
        if (!selected || !*selected) {
            return false;
        }

        size_t len = strlen(selected);
        if (len + 1 > path_size) {
            return false;
        }

        memcpy(path, selected, len + 1);
        return true;
    }
}

bool
sc_darwin_write_png_rgba8888(const char *path, const uint8_t *data,
                             size_t pitch, uint16_t width, uint16_t height) {
    if (!path || !*path) {
        return false;
    }

    @autoreleasepool {
        NSBitmapImageRep *rep =
            sc_darwin_create_image_rep_rgba8888(data, pitch, width, height);
        if (!rep) {
            return false;
        }

        NSData *png_data =
            [rep representationUsingType:NSBitmapImageFileTypePNG
                              properties:@{}];
        [rep release];
        if (!png_data) {
            return false;
        }

        NSString *ns_path = [NSString stringWithUTF8String:path];
        if (!ns_path) {
            return false;
        }

        return [png_data writeToFile:ns_path atomically:YES] == YES;
    }
}
