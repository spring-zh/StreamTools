#include "platformwindow.h"

#include <QWidget>

#import <Cocoa/Cocoa.h>

namespace bitscope {

void configurePlatformWindow(QWidget *window)
{
    if (!window)
        return;
    auto *view = reinterpret_cast<NSView *>(window->winId());
    NSWindow *nativeWindow = view.window;
    if (!nativeWindow)
        return;

    nativeWindow.titleVisibility = NSWindowTitleHidden;
    nativeWindow.titlebarAppearsTransparent = YES;
    nativeWindow.styleMask |= NSWindowStyleMaskFullSizeContentView;
    nativeWindow.movableByWindowBackground = YES;
    nativeWindow.backgroundColor = [NSColor colorWithRed:11.0 / 255.0
                                                   green:17.0 / 255.0
                                                    blue:25.0 / 255.0
                                                   alpha:1.0];
    if (@available(macOS 11.0, *))
        nativeWindow.titlebarSeparatorStyle = NSTitlebarSeparatorStyleNone;
}

} // namespace bitscope
