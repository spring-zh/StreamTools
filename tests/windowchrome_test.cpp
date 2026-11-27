#include "platformwindow.h"
#include "windowtitlebar.h"

#include <QApplication>
#include <QAction>
#include <QEventLoop>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QToolButton>
#include <QTimer>
#include <QWindow>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <windowsx.h>
#endif

namespace {
void require(bool condition, const char *message) {
    if (!condition) qFatal("FAIL: %s", message);
}

class TestWindow final : public QMainWindow {
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override {
        return bitscope::handlePlatformWindowEvent(this, message, result)
            || QMainWindow::nativeEvent(type, message, result);
    }
#endif
};

#ifdef Q_OS_WIN
LRESULT hitTest(QWidget &window, const QPoint &local) {
    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    const qreal scale = window.devicePixelRatioF();
    POINT p{qRound(local.x() * scale), qRound(local.y() * scale)};
    ClientToScreen(hwnd, &p);
    return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y));
}
#endif
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setStyle(QStringLiteral("Fusion"));
    TestWindow window;
    window.setStyleSheet(QStringLiteral(
        "QWidget {background:#0b1119;color:#dbe5ef;font-size:13px;}"
        "QToolBar {border:0;padding:0 10px;spacing:7px;}"
        "#titleMenuButton {background:transparent;border:0;padding:6px 12px;}"
        "#titleMenuButton::menu-indicator {image:none;width:0;}"));
    window.resize(900, 400);
    auto *bar = new bitscope::WindowTitleBar(&window);
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setFixedHeight(44);
    window.addToolBar(bar);
    auto *brand = new QLabel(QStringLiteral("BitScope Stream Analyzer"), bar);
    brand->setAttribute(Qt::WA_TransparentForMouseEvents);
    bar->addWidget(brand);
    auto *fileMenu = bar->addTitleMenu(QStringLiteral("文件"));
    auto *viewMenu = bar->addTitleMenu(QStringLiteral("视图"));
    auto *fileAction = fileMenu->addAction(QStringLiteral("打开码流…"));
    auto *viewAction = viewMenu->addAction(QStringLiteral("帧视图"));
    viewAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
    window.addAction(viewAction);
    int menuActions = 0;
    int shortcutActions = 0;
    QObject::connect(fileAction, &QAction::triggered, &window, [&] { ++menuActions; });
    QObject::connect(viewAction, &QAction::triggered, &window, [&] { ++shortcutActions; });
    // Nested, non-transparent children used to create dead drag regions.
    auto *wrapper = new QWidget(bar);
    auto *wrapperLayout = new QHBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    auto *nestedLabel = new QLabel(QStringLiteral("空白"), wrapper);
    wrapperLayout->addWidget(nestedLabel);
    auto *nestedButton = new QPushButton(QStringLiteral("按钮"), wrapper);
    wrapperLayout->addWidget(nestedButton);
    bar->addWidget(wrapper);
    auto *filename = new QLabel(QStringLiteral("test.mp4"), bar);
    filename->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    filename->setMinimumWidth(1);
    filename->setAttribute(Qt::WA_TransparentForMouseEvents);
    bar->addWidget(filename);
    auto *open = new QPushButton(QStringLiteral("打开码流…"), bar);
    bar->addWidget(open);
    bar->addWindowControls();
    auto *central = new QWidget(&window);
    window.setCentralWidget(central);
#ifdef Q_OS_WIN
    bitscope::configurePlatformWindow(&window);
#else
    window.setWindowFlag(Qt::FramelessWindowHint);
#endif
    window.show();
    QTimer::singleShot(100, &window, [&] {
        auto *minimize = bar->findChild<QToolButton *>(QStringLiteral("captionMinimize"));
        auto *maximize = bar->findChild<QToolButton *>(QStringLiteral("captionMaximize"));
        auto *close = bar->findChild<QToolButton *>(QStringLiteral("captionClose"));
        require(minimize && maximize && close, "all window controls exist");
        require(bar->geometry().top() == 0, "no separate menu or title row");
        filename->setText(QString(500, QChar('W')));
        QApplication::processEvents();
        require(close->isVisible() && bar->rect().contains(close->geometry()), "long filename keeps controls visible");
        const auto menuButtons = bar->findChildren<QToolButton *>(QStringLiteral("titleMenuButton"));
        require(menuButtons.size() == 2, "file and view buttons exist");
        for (auto *button : menuButtons) {
            require(button->isVisible() && bar->rect().contains(button->geometry()), "menu is not hidden in toolbar overflow");
            require(button->width() >= button->fontMetrics().horizontalAdvance(button->text()) + 20
                    && button->width() < 120, "menu text visible without taking over drag space");
        }
        require(menuButtons[0]->geometry().left() > brand->geometry().right()
                && menuButtons[1]->geometry().right() < filename->geometry().left(), "menus between brand and filename");
        const auto canDrag = [&](QWidget *widget) {
            return bitscope::WindowTitleBar::isDragPosition(&window, widget->mapTo(&window, widget->rect().center()));
        };
        require(canDrag(brand) && canDrag(filename) && canDrag(nestedLabel), "title, filename and nested label drag");
        require(!canDrag(nestedButton) && !canDrag(open) && !canDrag(close)
                && !canDrag(menuButtons[0]) && !canDrag(menuButtons[1]) && !canDrag(central), "controls and content do not drag");
        // Exercise a real press, not just QAction::trigger(), to catch menus
        // accidentally treated as title dragging. The timer closes the popup.
        auto *fileButton = menuButtons[0];
        QEventLoop menuLoop;
        QTimer::singleShot(50, &window, [&] {
            require(fileMenu->isVisible(), "file button opens popup");
            fileAction->trigger();
            fileMenu->close();
            menuLoop.quit();
        });
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(fileButton->rect().center()),
                          QPointF(fileButton->mapToGlobal(fileButton->rect().center())),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(fileButton, &press);
        if (menuActions == 0) menuLoop.exec();
        require(menuActions == 1, "file menu action executes");
        window.activateWindow();
        window.setFocus();
        QApplication::processEvents();
        QKeyEvent shortcut(QEvent::KeyPress, Qt::Key_1, Qt::ControlModifier);
        QApplication::sendEvent(&window, &shortcut);
        require(shortcutActions == 1, "view shortcut works with closed menu");
#ifdef Q_OS_WIN
        const auto hwnd = reinterpret_cast<HWND>(window.winId());
        require((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME) != 0, "native resize/snap style");
        RECT outer{}, client{};
        GetWindowRect(hwnd, &outer);
        GetClientRect(hwnd, &client);
        require(outer.bottom - outer.top == client.bottom, "native caption is removed");
        require(hitTest(window, brand->mapTo(&window, brand->rect().center())) == HTCAPTION, "brand is draggable");
        require(hitTest(window, filename->mapTo(&window, filename->rect().center())) == HTCAPTION, "filename is draggable");
        require(hitTest(window, nestedLabel->mapTo(&window, nestedLabel->rect().center())) == HTCAPTION, "nested label native drag");
        for (QWidget *widget : {static_cast<QWidget *>(open), static_cast<QWidget *>(menuButtons[0]),
                               static_cast<QWidget *>(menuButtons[1]), static_cast<QWidget *>(nestedButton), static_cast<QWidget *>(close)})
            require(hitTest(window, widget->mapTo(&window, widget->rect().center())) == HTCLIENT, "interactive control does not drag");
        require(hitTest(window, {1, 1}) == HTTOPLEFT, "top-left resize");
        require(hitTest(window, {window.width() - 2, window.height() - 2}) == HTBOTTOMRIGHT, "bottom-right resize");
        require(hitTest(window, central->mapTo(&window, central->rect().center())) == HTCLIENT, "content does not drag");
#endif
        maximize->click();
        QApplication::processEvents();
        require(window.isMaximized(), "maximize button");
#ifdef Q_OS_WIN
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        GetClientRect(hwnd, &client);
        POINT origin{0, 0};
        ClientToScreen(hwnd, &origin);
        require(origin.x == monitor.rcWork.left && origin.y == monitor.rcWork.top
                && origin.x + client.right == monitor.rcWork.right
                && origin.y + client.bottom == monitor.rcWork.bottom, "maximize respects monitor work area");
#endif
        maximize->click();
        QApplication::processEvents();
        require(!window.isMaximized(), "restore button");
        minimize->click();
        QApplication::processEvents();
        require(window.isMinimized(), "minimize button");
        window.showNormal();
        close->click();
        require(!window.isVisible(), "close button");
        qInfo("PASS: title menus, popup/shortcut, drag regions, long filename and window controls (native hit tests on Windows).");
        app.exit(0);
    });
    return app.exec();
}
