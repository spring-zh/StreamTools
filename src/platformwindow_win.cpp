#include "platformwindow.h"
#include "windowtitlebar.h"

#include <QVariant>
#include <QWidget>
#include <QtMath>
#include <qt_windows.h>
#include <windowsx.h>

namespace bitscope {

void configurePlatformWindow(QWidget *window)
{
    if (!window) return;
    window->setWindowFlag(Qt::FramelessWindowHint);
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    // Retain native sizing, taskbar and Aero Snap behavior, but remove the
    // non-client title via WM_NCCALCSIZE instead of drawing a second title row.
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME | WS_CAPTION
                     | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

bool handlePlatformWindowEvent(QWidget *window, void *message, qintptr *result)
{
    const auto *msg = static_cast<MSG *>(message);
    if (msg->message == WM_NCCALCSIZE && msg->wParam) {
        auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
        if (IsZoomed(msg->hwnd)) {
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                params->rgrc[0] = monitor.rcWork; // Do not cover the taskbar when maximized.
        }
        *result = 0;
        return true;
    }
    if (msg->message != WM_NCHITTEST) return false;

    // LPARAM uses physical screen pixels (and signed coordinates on monitors
    // left/above the primary display). Convert only after native edge testing.
    POINT point{GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
    RECT rect{};
    if (!GetWindowRect(msg->hwnd, &rect)) return false;
    if (!IsZoomed(msg->hwnd) && !window->isFullScreen()) {
        const int border = qMax(1, qRound(6 * window->devicePixelRatioF()));
        const bool left = point.x >= rect.left && point.x < rect.left + border;
        const bool right = point.x < rect.right && point.x >= rect.right - border;
        const bool top = point.y >= rect.top && point.y < rect.top + border;
        const bool bottom = point.y < rect.bottom && point.y >= rect.bottom - border;
        if (left || right || top || bottom) {
            *result = top ? (left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP)
                : bottom ? (left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM)
                : left ? HTLEFT : HTRIGHT;
            return true;
        }
    }
    ScreenToClient(msg->hwnd, &point);
    const qreal scale = window->devicePixelRatioF();
    const QPoint local(qFloor(point.x / scale), qFloor(point.y / scale));
    *result = WindowTitleBar::isDragPosition(window, local) ? HTCAPTION : HTCLIENT;
    return true;
}

} // namespace bitscope
