#include "platformwindow.h"

#include <QWidget>

#import <Cocoa/Cocoa.h>

namespace bitscope {

void configurePlatformWindow(QWidget *window)
{
    if (!window)
        return;
    window->setWindowFlag(Qt::FramelessWindowHint, true);
    auto *view = reinterpret_cast<NSView *>(window->winId());
    NSWindow *nativeWindow = view.window;
    if (!nativeWindow)
        return;

    nativeWindow.styleMask |= NSWindowStyleMaskResizable
        | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskClosable;
    nativeWindow.movableByWindowBackground = NO;
    nativeWindow.hasShadow = YES;
    nativeWindow.backgroundColor = [NSColor colorWithRed:11.0 / 255.0
                                                   green:17.0 / 255.0
                                                    blue:25.0 / 255.0
                                                   alpha:1.0];
}

} // namespace bitscope
