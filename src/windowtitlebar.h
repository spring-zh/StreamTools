#pragma once

#include <QToolBar>

class QMenu;

namespace bitscope {

class WindowTitleBar final : public QToolBar {
public:
    explicit WindowTitleBar(QWidget *parent = nullptr);
    QMenu *addTitleMenu(const QString &title);
    void addWindowControls();
    static bool isDragPosition(const QWidget *window, const QPoint &position);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
};

} // namespace bitscope
