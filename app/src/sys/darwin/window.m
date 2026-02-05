#include "sys/darwin/window.h"

#include <SDL2/SDL_syswm.h>

#import <AppKit/AppKit.h>

bool
sc_darwin_window_configure_native_chrome(SDL_Window *window) {
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window, &wm_info)) {
        return false;
    }

    if (wm_info.subsystem != SDL_SYSWM_COCOA || !wm_info.info.cocoa.window) {
        return false;
    }

    @autoreleasepool {
        NSWindow *ns_window = wm_info.info.cocoa.window;
        ns_window.titleVisibility = NSWindowTitleHidden;
        ns_window.titlebarAppearsTransparent = YES;
        ns_window.movableByWindowBackground = NO;
        ns_window.styleMask |= NSWindowStyleMaskFullSizeContentView;
        if ([ns_window respondsToSelector:@selector(setTitlebarSeparatorStyle:)]) {
            ns_window.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
        }
    }

    return true;
}
