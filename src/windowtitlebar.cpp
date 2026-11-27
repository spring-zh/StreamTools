#include "windowtitlebar.h"

#include <QEvent>
#include <QAbstractButton>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QWindow>

namespace bitscope {
namespace {

// Draw in logical pixels rather than relying on font glyphs or light-theme icons.
class CaptionButton final : public QToolButton {
public:
    enum Role { Minimize, Maximize, Close };
    CaptionButton(Role role, QWidget *parent) : QToolButton(parent), m_role(role) {
        setFixedSize(38, 30);
        setCursor(Qt::ArrowCursor);
        const QString label = role == Minimize ? QStringLiteral("最小化")
            : role == Close ? QStringLiteral("关闭") : QStringLiteral("最大化 / 还原");
        setToolTip(label);
        setAccessibleName(label);
        setObjectName(role == Minimize ? QStringLiteral("captionMinimize")
            : role == Close ? QStringLiteral("captionClose") : QStringLiteral("captionMaximize"));
        window()->installEventFilter(this);
        connect(this, &QToolButton::clicked, this, [this] {
            if (m_role == Close) window()->close();
            else if (m_role == Minimize) window()->showMinimized();
            else if (window()->isMaximized()) window()->showNormal();
            else window()->showMaximized();
        });
    }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::WindowStateChange) update();
        return QToolButton::eventFilter(watched, event);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (underMouse() || isDown() || hasFocus()) {
            p.setPen(Qt::NoPen);
            p.setBrush(m_role == Close ? QColor(isDown() ? "#b83240" : "#d7414f")
                                      : QColor(isDown() ? "#2b4054" : "#203346"));
            p.drawRoundedRect(rect(), 5, 5);
        }
        p.setPen(QPen(QColor("#dbe5ef"), 1.2));
        p.setBrush(Qt::NoBrush);
        const QRectF glyph(width() / 2.0 - 5, height() / 2.0 - 5, 10, 10);
        if (m_role == Minimize) p.drawLine(glyph.bottomLeft(), glyph.bottomRight());
        else if (m_role == Close) {
            p.drawLine(glyph.topLeft(), glyph.bottomRight());
            p.drawLine(glyph.topRight(), glyph.bottomLeft());
        } else if (window()->isMaximized()) {
            p.drawPolyline(QPolygonF{glyph.topLeft() + QPointF(3, 2), glyph.topLeft() + QPointF(3, 0),
                                     glyph.topRight(), glyph.bottomRight() - QPointF(0, 3),
                                     glyph.bottomRight() - QPointF(2, 3)});
            p.drawRect(glyph.adjusted(0, 3, -3, 0));
        } else p.drawRect(glyph);
    }
private:
    Role m_role;
};
}

WindowTitleBar::WindowTitleBar(QWidget *parent) : QToolBar(parent)
{
    setProperty("bitscopeTitleBar", true);
}

QMenu *WindowTitleBar::addTitleMenu(const QString &title)
{
    // A QMenuBar embedded as a toolbar widget can expand/collapse its actions
    // into an overflow control on Windows. Explicit text buttons have stable
    // size hints and leave the unused title area available for native dragging.
    auto *button = new QToolButton(this);
    button->setObjectName(QStringLiteral("titleMenuButton"));
    button->setText(title);
    button->setAccessibleName(title);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setPopupMode(QToolButton::InstantPopup);
    button->setAutoRaise(true);
    button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    button->setCursor(Qt::ArrowCursor);
    auto *menu = new QMenu(title, button);
    button->setMenu(menu);
    addWidget(button);
    return menu;
}

bool WindowTitleBar::isDragPosition(const QWidget *window, const QPoint &position)
{
    if (!window || window->isFullScreen() || !window->rect().contains(position)) return false;
    // Do not rely on the deepest hit widget being the toolbar itself: layouts
    // can introduce child containers. Only interactive controls block dragging.
    for (const QWidget *hit = window->childAt(position); hit && hit != window; hit = hit->parentWidget()) {
        if (qobject_cast<const QAbstractButton *>(hit) || qobject_cast<const QMenuBar *>(hit)
            || hit->property("bitscopeTitleInteractive").toBool())
            return false;
        if (hit->property("bitscopeTitleBar").toBool()) return true;
    }
    return false;
}

void WindowTitleBar::addWindowControls()
{
    for (const auto role : {CaptionButton::Minimize, CaptionButton::Maximize, CaptionButton::Close})
        addWidget(new CaptionButton(role, this));
}

void WindowTitleBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton
        && isDragPosition(window(), mapTo(window(), event->position().toPoint())) && window()->windowHandle()
        && window()->windowHandle()->startSystemMove()) {
        event->accept();
        return;
    }
    QToolBar::mousePressEvent(event);
}

void WindowTitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton
        && isDragPosition(window(), mapTo(window(), event->position().toPoint()))) {
        window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
        event->accept();
        return;
    }
    QToolBar::mouseDoubleClickEvent(event);
}

} // namespace bitscope
