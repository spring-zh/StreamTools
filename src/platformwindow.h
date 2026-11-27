#pragma once

#include <QtGlobal>

class QWidget;

namespace bitscope {

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
void configurePlatformWindow(QWidget *window);
#else
inline void configurePlatformWindow(QWidget *) {}
#endif

#ifdef Q_OS_WIN
bool handlePlatformWindowEvent(QWidget *window, void *message, qintptr *result);
#endif

} // namespace bitscope
