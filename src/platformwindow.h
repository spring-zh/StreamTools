#pragma once

#include <QtGlobal>

class QWidget;

namespace bitscope {

#ifdef Q_OS_MACOS
void configurePlatformWindow(QWidget *window);
#else
inline void configurePlatformWindow(QWidget *) {}
#endif

} // namespace bitscope
