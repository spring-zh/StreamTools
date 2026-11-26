#pragma once

#include "bitstreamtypes.h"

#include <QWidget>

namespace bitscope {

class TimelineWidget final : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(QWidget *parent = nullptr);
    void setStream(StreamPtr stream);
    void setSelectedFrame(int frameIndex);
    void setZoom(double zoom);
    double zoom() const { return m_zoom; }
    QSize minimumSizeHint() const override { return {500,120}; }

Q_SIGNALS:
    void frameActivated(int frameIndex);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    QColor frameColor(const FrameInfo &frame) const;
    QPair<int,int> visibleRange() const;
    StreamPtr m_stream;
    int m_selected = 0;
    double m_zoom = 1.0;
    QRectF m_trackRect;
};

} // namespace bitscope

