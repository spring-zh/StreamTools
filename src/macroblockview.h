#pragma once

#include "bitstreamtypes.h"

#include <QWidget>

class QMouseEvent;
class QResizeEvent;
class QWheelEvent;

namespace bitscope {

class MacroblockView final : public QWidget {
    Q_OBJECT
public:
    explicit MacroblockView(QWidget *parent = nullptr);
    void setStream(StreamPtr stream);
    void setFrame(int frameIndex);
    void setFrameImage(int frameIndex, const QImage &image);
    void setFrameError(int frameIndex, const QString &error);
    void setGridVisible(bool visible);
    void setSliceOverlayVisible(bool visible);
    void setQpOverlayVisible(bool visible);
    void refreshFrameData();
    double zoom() const { return m_zoom; }
    void setZoom(double zoom);
    void zoomBy(double factor);
    void resetView();
    QSize minimumSizeHint() const override { return {360,240}; }

Q_SIGNALS:
    void unitHovered(const QString &description);
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRectF basePictureRect() const;
    QRectF pictureRect() const;
    QRectF sourceRect(int x, int y, int width, int height,
                      const QRectF &picture) const;
    QRectF unitRect(const FrameInfo &frame, int column, int row,
                    const QRectF &picture) const;
    void setZoomAt(double zoom, const QPointF &anchor);
    void clampPan();
    QColor regionColor(const QString &type) const;
    StreamPtr m_stream;
    int m_frameIndex = -1;
    QImage m_image;
    QString m_error;
    bool m_showGrid = true;
    bool m_showSlices = true;
    bool m_showQp = false;
    QPoint m_hover = {-1,-1};
    int m_hoverCu = -1;
    double m_zoom = 1.0;
    QPointF m_pan;
    QPointF m_lastPanPosition;
    bool m_panning = false;
};

} // namespace bitscope
