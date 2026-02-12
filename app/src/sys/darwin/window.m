#include "sys/darwin/window.h"

#include <SDL2/SDL_syswm.h>

#import <AppKit/AppKit.h>

@interface ScSettingsMenuActionTarget : NSObject
@property(nonatomic, assign) NSInteger selectedTag;
- (void)onMenuItem:(id)sender;
@end

@implementation ScSettingsMenuActionTarget
- (instancetype)init {
    self = [super init];
    if (self) {
        _selectedTag = SC_DARWIN_SETTINGS_MENU_ACTION_NONE;
    }
    return self;
}

- (void)onMenuItem:(id)sender {
    if ([sender isKindOfClass:[NSMenuItem class]]) {
        self.selectedTag = [(NSMenuItem *) sender tag];
    }
}
@end

static NSWindow *
sc_darwin_window_get_native_window(SDL_Window *window) {
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window, &wm_info)) {
        return nil;
    }

    if (wm_info.subsystem != SDL_SYSWM_COCOA || !wm_info.info.cocoa.window) {
        return nil;
    }

    return wm_info.info.cocoa.window;
}

bool
sc_darwin_window_configure_native_chrome(SDL_Window *window) {
    NSWindow *ns_window = sc_darwin_window_get_native_window(window);
    if (!ns_window) {
        return false;
    }

    @autoreleasepool {
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

enum sc_darwin_settings_menu_action
sc_darwin_window_show_settings_menu(SDL_Window *window, int32_t x, int32_t y,
                                    bool save_selected,
                                    const char *save_directory) {
    NSWindow *ns_window = sc_darwin_window_get_native_window(window);
    if (!ns_window) {
        return SC_DARWIN_SETTINGS_MENU_ACTION_NONE;
    }

    @autoreleasepool {
        NSView *content_view = [ns_window contentView];
        if (!content_view) {
            return SC_DARWIN_SETTINGS_MENU_ACTION_NONE;
        }

        ScSettingsMenuActionTarget *target =
            [[ScSettingsMenuActionTarget alloc] init];
        NSMenu *menu = [[NSMenu alloc] initWithTitle:@"Settings"];

        NSMenuItem *section_item =
            [[NSMenuItem alloc] initWithTitle:@"screenshot detination"
                                       action:nil
                                keyEquivalent:@""];
        section_item.enabled = NO;
        [menu addItem:section_item];
        [section_item release];
        [menu addItem:[NSMenuItem separatorItem]];

        NSMenuItem *copy_item =
            [[NSMenuItem alloc] initWithTitle:@"Copy to Clipboard"
                                       action:@selector(onMenuItem:)
                                keyEquivalent:@""];
        copy_item.target = target;
        copy_item.tag = SC_DARWIN_SETTINGS_MENU_ACTION_COPY_TO_CLIPBOARD;
        copy_item.state = save_selected ? NSControlStateValueOff
                                        : NSControlStateValueOn;
        [menu addItem:copy_item];
        [copy_item release];

        NSString *save_title = @"Save Image To…";
        if (save_directory && *save_directory) {
            NSString *full_path = [NSString stringWithUTF8String:save_directory];
            if (full_path) {
                NSString *folder_name = [full_path lastPathComponent];
                if (folder_name.length) {
                    save_title = [NSString stringWithFormat:@"Save Image To \"%@\"",
                                                            folder_name];
                }
            }
        }

        NSMenuItem *save_item =
            [[NSMenuItem alloc] initWithTitle:save_title
                                       action:@selector(onMenuItem:)
                                keyEquivalent:@""];
        save_item.target = target;
        save_item.tag = SC_DARWIN_SETTINGS_MENU_ACTION_SAVE_TO_DIRECTORY;
        save_item.state = save_selected ? NSControlStateValueOn
                                        : NSControlStateValueOff;
        [menu addItem:save_item];
        [save_item release];

        NSRect bounds = [content_view bounds];
        NSPoint location = NSMakePoint((CGFloat) x,
                                       bounds.size.height - (CGFloat) y);
        [menu popUpMenuPositioningItem:nil atLocation:location
                                inView:content_view];

        NSInteger selected_tag = target.selectedTag;
        [menu release];
        [target release];

        switch (selected_tag) {
            case SC_DARWIN_SETTINGS_MENU_ACTION_COPY_TO_CLIPBOARD:
                return SC_DARWIN_SETTINGS_MENU_ACTION_COPY_TO_CLIPBOARD;
            case SC_DARWIN_SETTINGS_MENU_ACTION_SAVE_TO_DIRECTORY:
                return SC_DARWIN_SETTINGS_MENU_ACTION_SAVE_TO_DIRECTORY;
            default:
                return SC_DARWIN_SETTINGS_MENU_ACTION_NONE;
        }
    }
}
