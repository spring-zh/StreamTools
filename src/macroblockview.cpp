#include "macroblockview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace bitscope {

namespace {
constexpr double kMinimumZoom = 0.5;
constexpr double kMaximumZoom = 8.0;
constexpr double kViewMargin = 12.0;
}

MacroblockView::MacroblockView(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void MacroblockView::setStream(StreamPtr stream)
{
    m_stream = std::move(stream);
    m_frameIndex = -1;
    m_image = {};
    m_error.clear();
    m_hoverCu = -1;
    resetView();
}

void MacroblockView::setFrame(int frameIndex)
{
    if (m_frameIndex == frameIndex)
        return;
    m_frameIndex = frameIndex;
    m_image = {};
    m_error.clear();
    m_hover = {-1, -1};
    m_hoverCu = -1;
    update();
}

void MacroblockView::setFrameImage(int frameIndex, const QImage &image)
{
    if (frameIndex != m_frameIndex)
        return;
    m_image = image;
    m_error.clear();
    update();
}

void MacroblockView::setFrameError(int frameIndex, const QString &error)
{
    if (frameIndex != m_frameIndex)
        return;
    m_image = {};
    m_error = error;
    update();
}

void MacroblockView::setGridVisible(bool visible)
{
    m_showGrid = visible;
    update();
}

void MacroblockView::setSliceOverlayVisible(bool visible)
{
    m_showSlices = visible;
    update();
}

void MacroblockView::setQpOverlayVisible(bool visible)
{
    m_showQp = visible;
    update();
}

void MacroblockView::refreshFrameData()
{
    update();
}

void MacroblockView::setZoom(double zoom)
{
    setZoomAt(zoom, QRectF(rect()).center());
}

void MacroblockView::zoomBy(double factor)
{
    setZoom(m_zoom * factor);
}

void MacroblockView::resetView()
{
    const bool changed = !qFuzzyCompare(m_zoom, 1.0) || !m_pan.isNull();
    m_zoom = 1.0;
    m_pan = {};
    m_panning = false;
    unsetCursor();
    if (changed)
        Q_EMIT zoomChanged(m_zoom);
    update();
}

QRectF MacroblockView::basePictureRect() const
{
    const QRectF available = QRectF(rect()).adjusted(kViewMargin, kViewMargin,
                                                     -kViewMargin, -kViewMargin);
    if (!m_stream || m_stream->width <= 0 || m_stream->height <= 0)
        return available;

    const double aspect = double(m_stream->width) / m_stream->height;
    double width = available.width();
    double height = width / aspect;
    if (height > available.height()) {
        height = available.height();
        width = height * aspect;
    }
    return {available.center().x() - width / 2.0,
            available.center().y() - height / 2.0,
            width, height};
}

QRectF MacroblockView::pictureRect() const
{
    const QRectF base = basePictureRect();
    const QSizeF scaled = base.size() * m_zoom;
    const QPointF center = base.center() + m_pan;
    return {center.x() - scaled.width() / 2.0,
            center.y() - scaled.height() / 2.0,
            scaled.width(), scaled.height()};
}

QRectF MacroblockView::unitRect(const FrameInfo &frame, int column, int row,
                                const QRectF &picture) const
{
    if (!m_stream || m_stream->width <= 0 || m_stream->height <= 0
        || frame.unitSize <= 0)
        return {};
    const int sourceX = column * frame.unitSize;
    const int sourceY = row * frame.unitSize;
    const int sourceRight = std::min(sourceX + frame.unitSize, m_stream->width);
    const int sourceBottom = std::min(sourceY + frame.unitSize, m_stream->height);
    return sourceRect(sourceX, sourceY, sourceRight - sourceX,
                      sourceBottom - sourceY, picture);
}

QRectF MacroblockView::sourceRect(int x, int y, int width, int height,
                                  const QRectF &picture) const
{
    if (!m_stream || m_stream->width <= 0 || m_stream->height <= 0)
        return {};
    return {
        picture.left() + picture.width() * x / m_stream->width,
        picture.top() + picture.height() * y / m_stream->height,
        picture.width() * std::max(0, width) / m_stream->width,
        picture.height() * std::max(0, height) / m_stream->height
    };
}

void MacroblockView::setZoomAt(double zoom, const QPointF &anchor)
{
    const double nextZoom = std::clamp(zoom, kMinimumZoom, kMaximumZoom);
    if (qFuzzyCompare(nextZoom, m_zoom))
        return;

    const QRectF oldPicture = pictureRect();
    const double relativeX = oldPicture.width() > 0
        ? (anchor.x() - oldPicture.left()) / oldPicture.width() : 0.5;
    const double relativeY = oldPicture.height() > 0
        ? (anchor.y() - oldPicture.top()) / oldPicture.height() : 0.5;

    m_zoom = nextZoom;
    const QRectF newPicture = pictureRect();
    const QPointF mappedPoint(newPicture.left() + relativeX * newPicture.width(),
                              newPicture.top() + relativeY * newPicture.height());
    m_pan += anchor - mappedPoint;
    clampPan();
    setCursor(m_zoom > 1.0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
    Q_EMIT zoomChanged(m_zoom);
    update();
}

void MacroblockView::clampPan()
{
    const QRectF available = QRectF(rect()).adjusted(kViewMargin, kViewMargin,
                                                     -kViewMargin, -kViewMargin);
    const QSizeF scaled = basePictureRect().size() * m_zoom;
    const double maximumX = std::max(0.0, (scaled.width() - available.width()) / 2.0);
    const double maximumY = std::max(0.0, (scaled.height() - available.height()) / 2.0);
    m_pan.setX(std::clamp(m_pan.x(), -maximumX, maximumX));
    m_pan.setY(std::clamp(m_pan.y(), -maximumY, maximumY));
}

QColor MacroblockView::regionColor(const QString &type) const
{
    if (type == "I" || type == "SI")
        return QColor(244, 162, 97, 78);
    if (type == "P" || type == "SP")
        return QColor(88, 166, 255, 72);
    if (type == "B")
        return QColor(155, 135, 245, 74);
    return QColor(25, 194, 201, 70);
}

void MacroblockView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.fillRect(rect(), QColor("#080d13"));
    if (!m_stream || m_frameIndex < 0 || m_frameIndex >= m_stream->frames.size()) {
        painter.setPen(QColor("#68788c"));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("选择视频帧以查看画面和宏块"));
        return;
    }

    const auto &frame = m_stream->frames.at(m_frameIndex);
    const QRectF picture = pictureRect();
    const QRectF viewport = QRectF(rect()).adjusted(kViewMargin, kViewMargin,
                                                   -kViewMargin, -kViewMargin);
    painter.save();
    painter.setClipRect(viewport);
    painter.setPen(QPen(QColor("#2a394b"), 1));
    painter.setBrush(QColor("#101923"));
    painter.drawRoundedRect(picture, 5, 5);
    if (!m_image.isNull()) {
        painter.drawImage(picture, m_image);
    } else if (!m_error.isEmpty()) {
        painter.setPen(QColor("#ff7b72"));
        painter.drawText(picture.adjusted(30, 30, -30, -30),
                         Qt::AlignCenter | Qt::TextWordWrap,
                         QStringLiteral("Frame %1 预览不可用\n\n%2")
                             .arg(frame.index).arg(m_error));
    } else {
        painter.setPen(QColor("#8190a3"));
        painter.drawText(picture, Qt::AlignCenter,
                         QStringLiteral("正在按需生成 Frame %1 预览…").arg(frame.index));
    }

    if (frame.unitColumns <= 0 || frame.unitRows <= 0) {
        painter.restore();
        return;
    }

    if (m_showSlices) {
        for (const auto &region : frame.regions) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(regionColor(region.type));
            for (int address = region.start; address < region.end; ++address) {
                const int row = address / frame.unitColumns;
                const int column = address % frame.unitColumns;
                painter.drawRect(unitRect(frame, column, row, picture));
            }
        }
        if (!frame.regions.isEmpty() && frame.regions.first().start > 0) {
            painter.setBrush(QColor(255, 107, 107, 80));
            const int missing = frame.regions.first().start;
            for (int address = 0; address < missing; ++address) {
                const int row = address / frame.unitColumns;
                const int column = address % frame.unitColumns;
                painter.drawRect(unitRect(frame, column, row, picture));
            }
        }
    }

    if (m_showQp && m_stream->codec == Codec::Av1 && frame.av1CodingBlocksReady) {
        painter.setPen(Qt::NoPen);
        for (const auto &block : frame.av1CodingBlocks) {
            if (block.qIndex < 0)
                continue;
            const int hue = std::clamp(220 - qRound(block.qIndex / 255.0 * 220.0), 0, 220);
            painter.setBrush(QColor::fromHsv(hue, 205, 235, 92));
            painter.drawRect(sourceRect(block.x, block.y, block.width, block.height, picture));
        }
    } else if (m_showQp && m_stream->codec == Codec::Hevc && frame.hevcCodingUnitsReady) {
        painter.setPen(Qt::NoPen);
        for (const auto &unit : frame.hevcCodingUnits) {
            if (unit.qp < 0)
                continue;
            const int hue = std::clamp(220 - qRound(unit.qp / 51.0 * 220.0), 0, 220);
            painter.setBrush(QColor::fromHsv(hue, 205, 235, 92));
            painter.drawRect(sourceRect(unit.x, unit.y, unit.width, unit.height, picture));
        }
    } else if (m_showQp && frame.decodedUnitsReady) {
        painter.setPen(Qt::NoPen);
        const double maximumQp = m_stream->codec == Codec::Mpeg4Visual ? 31.0
            : (m_stream->codec == Codec::Av1 ? 255.0 : 51.0);
        for (const auto &unit : frame.decodedUnits) {
            if (unit.qp < 0 || unit.column < 0 || unit.row < 0)
                continue;
            const int hue = std::clamp(220 - qRound(unit.qp / maximumQp * 220.0), 0, 220);
            QColor color = QColor::fromHsv(hue, 205, 235, 92);
            painter.setBrush(color);
            painter.drawRect(unitRect(frame, unit.column, unit.row, picture));
        }
    }

    if (m_showGrid) {
        const double fullCellWidth = picture.width() * frame.unitSize / m_stream->width;
        const double fullCellHeight = picture.height() * frame.unitSize / m_stream->height;
        const int step = std::max(1, int(std::ceil(2.4 / std::min(fullCellWidth, fullCellHeight))));
        painter.setPen(QPen(QColor(160, 178, 197, 45), .5));
        for (int column = 0; column <= frame.unitColumns; column += step) {
            const int sourceX = std::min(column * frame.unitSize, m_stream->width);
            const double x = picture.left() + picture.width() * sourceX / m_stream->width;
            painter.drawLine(QPointF(x, picture.top()), QPointF(x, picture.bottom()));
        }
        if (frame.unitColumns % step != 0)
            painter.drawLine(picture.topRight(), picture.bottomRight());
        for (int row = 0; row <= frame.unitRows; row += step) {
            const int sourceY = std::min(row * frame.unitSize, m_stream->height);
            const double y = picture.top() + picture.height() * sourceY / m_stream->height;
            painter.drawLine(QPointF(picture.left(), y), QPointF(picture.right(), y));
        }
        if (frame.unitRows % step != 0)
            painter.drawLine(picture.bottomLeft(), picture.bottomRight());

        if (m_stream->codec == Codec::Hevc && frame.hevcCodingUnitsReady) {
            for (const auto &unit : frame.hevcCodingUnits) {
                QColor lineColor = unit.prediction == QStringLiteral("帧内预测")
                    ? QColor(255, 185, 90, 205) : QColor(65, 220, 225, 190);
                lineColor.setAlpha(std::clamp(130 + unit.depth * 25, 130, 230));
                painter.setPen(QPen(lineColor, unit.depth >= 3 ? 0.8 : 1.15));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(sourceRect(unit.x, unit.y, unit.width, unit.height, picture));
            }
        } else if (m_stream->codec == Codec::Av1 && frame.av1CodingBlocksReady) {
            for (const auto &block : frame.av1CodingBlocks) {
                const bool intra = block.referenceFrames.contains(QStringLiteral("INTRA FRAME"));
                QColor lineColor = intra ? QColor(255, 185, 90, 215)
                                         : QColor(65, 220, 225, 200);
                lineColor.setAlpha(std::clamp(145 + block.depth * 18, 145, 235));
                painter.setPen(QPen(lineColor, block.depth >= 3 ? 0.75 : 1.1));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(sourceRect(block.x, block.y, block.width, block.height, picture));
            }
        }
    }

    painter.setPen(QPen(QColor(255, 255, 255, 125), 1));
    for (const auto &region : frame.regions) {
        const int row = region.start / frame.unitColumns;
        const int column = region.start % frame.unitColumns;
        const QRectF startRect = unitRect(frame, column, row, picture);
        painter.drawLine(startRect.topLeft(), QPointF(picture.right(), startRect.top()));
    }
    if (m_stream->codec == Codec::Av1 && m_hoverCu >= 0
        && m_hoverCu < frame.av1CodingBlocks.size()) {
        const auto &block = frame.av1CodingBlocks.at(m_hoverCu);
        painter.setPen(QPen(QColor("#ffffff"), 2));
        painter.setBrush(QColor(25, 194, 201, 30));
        painter.drawRect(sourceRect(block.x, block.y, block.width, block.height, picture));
    } else if (m_hoverCu >= 0 && m_hoverCu < frame.hevcCodingUnits.size()) {
        const auto &unit = frame.hevcCodingUnits.at(m_hoverCu);
        painter.setPen(QPen(QColor("#ffffff"), 2));
        painter.setBrush(QColor(25, 194, 201, 30));
        painter.drawRect(sourceRect(unit.x, unit.y, unit.width, unit.height, picture));
    } else if (m_hover.x() >= 0) {
        painter.setPen(QPen(QColor("#19c2c9"), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(unitRect(frame, m_hover.x(), m_hover.y(), picture));
    }
    painter.restore();
}

void MacroblockView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_panning) {
        m_pan += event->position() - m_lastPanPosition;
        m_lastPanPosition = event->position();
        clampPan();
        update();
        event->accept();
        return;
    }
    if (!m_stream || m_frameIndex < 0)
        return;

    const auto &frame = m_stream->frames.at(m_frameIndex);
    const QRectF picture = pictureRect();
    if (!picture.contains(event->position()) || frame.unitColumns <= 0) {
        m_hover = {-1, -1};
        m_hoverCu = -1;
        update();
        return;
    }

    const int pixelX = std::clamp(
        int((event->position().x() - picture.left()) / picture.width() * m_stream->width),
        0, m_stream->width - 1);
    const int pixelY = std::clamp(
        int((event->position().y() - picture.top()) / picture.height() * m_stream->height),
        0, m_stream->height - 1);
    const int column = std::clamp(pixelX / frame.unitSize, 0, frame.unitColumns - 1);
    const int row = std::clamp(pixelY / frame.unitSize, 0, frame.unitRows - 1);
    int address = row * frame.unitColumns + column;
    m_hover = {column, row};
    m_hoverCu = -1;
    if (m_stream->codec == Codec::Hevc && frame.hevcCodingUnitsReady) {
        for (int index = 0; index < frame.hevcCodingUnits.size(); ++index) {
            const auto &unit = frame.hevcCodingUnits.at(index);
            if (pixelX >= unit.x && pixelX < unit.x + unit.width
                && pixelY >= unit.y && pixelY < unit.y + unit.height) {
                m_hoverCu = index;
                address = unit.ctuAddress;
                break;
            }
        }
    } else if (m_stream->codec == Codec::Av1 && frame.av1CodingBlocksReady) {
        for (int index = 0; index < frame.av1CodingBlocks.size(); ++index) {
            const auto &block = frame.av1CodingBlocks.at(index);
            if (pixelX >= block.x && pixelX < block.x + block.width
                && pixelY >= block.y && pixelY < block.y + block.height) {
                m_hoverCu = index;
                address = block.superblockAddress;
                break;
            }
        }
    }
    const auto region = std::find_if(frame.regions.begin(), frame.regions.end(),
        [address](const CodingUnitRegion &candidate) {
            return address >= candidate.start && address < candidate.end;
        });
    const bool hevc = m_stream->codec == Codec::Hevc;
    const bool av1 = m_stream->codec == Codec::Av1;
    const QString unit = hevc ? QStringLiteral("CTU 根单元")
        : (av1 ? QStringLiteral("Superblock 根单元") : QStringLiteral("宏块"));
    const QString pictureUnit = m_stream->codec == Codec::Mpeg4Visual
        ? QStringLiteral("VOP") : (av1 ? QStringLiteral("Frame OBU") : QStringLiteral("Slice"));
    const QString slice = region == frame.regions.end()
        ? QStringLiteral("当前帧 %1 未覆盖").arg(pictureUnit)
        : QStringLiteral("%1 #%2 · %3").arg(pictureUnit)
              .arg(region->sliceIndex).arg(region->type);
    if (av1 && m_hoverCu >= 0 && m_hoverCu < frame.av1CodingBlocks.size()) {
        const auto &block = frame.av1CodingBlocks.at(m_hoverCu);
        const QString visibleSize = block.width == block.codedWidth
                && block.height == block.codedHeight
            ? QString()
            : QStringLiteral(" · 可见裁剪 %1×%2").arg(block.width).arg(block.height);
        QString reference = block.referenceFrames.isEmpty()
            ? QStringLiteral("—") : block.referenceFrames;
        if (!reference.contains(QStringLiteral("INTRA FRAME")))
            reference += QStringLiteral(" · MV0(%1,%2) · MV1(%3,%4)")
                .arg(block.motionVector0X).arg(block.motionVector0Y)
                .arg(block.motionVector1X).arg(block.motionVector1Y);
        Q_EMIT unitHovered(
            QStringLiteral("AV1 Coding Block #%1 · Superblock %2 · 尺寸层级 %3 · 编码尺寸 %4×%5%6 · "
                           "源像素 (%7, %8)–(%9, %10) · Y %11 · UV %12 · TX %13/%14 · "
                           "QIndex %15 · Segment %16 · %17%18 · %19")
                .arg(m_hoverCu).arg(block.superblockAddress).arg(block.depth)
                .arg(block.codedWidth).arg(block.codedHeight).arg(visibleSize)
                .arg(block.x).arg(block.y)
                .arg(block.x + block.width - 1).arg(block.y + block.height - 1)
                .arg(block.predictionMode, block.uvPredictionMode,
                     block.transformSize, block.transformType)
                .arg(block.qIndex).arg(block.segmentId)
                .arg(reference)
                .arg(block.skip ? QStringLiteral(" · Skip") : QString())
                .arg(slice));
        update();
        return;
    }
    if (m_hoverCu >= 0) {
        const auto &unitInfo = frame.hevcCodingUnits.at(m_hoverCu);
        const QString qp = unitInfo.qp >= 0
            ? QString::number(unitInfo.qp) : QStringLiteral("—");
        const QString bytes = unitInfo.approximateBytes >= 0
            ? QStringLiteral("%1 bytes（CABAC 近似）").arg(unitInfo.approximateBytes)
            : QStringLiteral("—");
        const QString visibleSize = unitInfo.width == unitInfo.codedSize
                && unitInfo.height == unitInfo.codedSize
            ? QString()
            : QStringLiteral(" · 可见裁剪 %1×%2")
                  .arg(unitInfo.width).arg(unitInfo.height);
        Q_EMIT unitHovered(
            QStringLiteral("CU #%1 · CTU %2 · 深度 %3 · 编码尺寸 %4×%4%5 · 源像素 (%6, %7)–(%8, %9) · "
                           "分区 %10 · %11 · QP %12 · 编码量 %13 · %14")
                .arg(m_hoverCu).arg(unitInfo.ctuAddress).arg(unitInfo.depth)
                .arg(unitInfo.codedSize).arg(visibleSize)
                .arg(unitInfo.x).arg(unitInfo.y)
                .arg(unitInfo.x + unitInfo.width - 1)
                .arg(unitInfo.y + unitInfo.height - 1)
                .arg(unitInfo.partitionName, unitInfo.prediction, qp, bytes, slice));
        update();
        return;
    }
    QString decoded;
    if (address >= 0 && address < frame.decodedUnits.size()
        && frame.decodedUnits.at(address).qp >= 0) {
        const auto &unitInfo = frame.decodedUnits.at(address);
        decoded = QStringLiteral("实际 QP %1 · %2 · 类型码 %3")
            .arg(unitInfo.qp).arg(unitInfo.category, unitInfo.decoderType);
    } else if (av1) {
        decoded = frame.av1CodingBlocksError.isEmpty()
            ? QStringLiteral("正在加载 AV1 实际 Coding Block")
            : frame.av1CodingBlocksError;
    } else if (!frame.decodedUnitsError.isEmpty()) {
        decoded = frame.decodedUnitsError;
    } else {
        decoded = QStringLiteral("正在加载实际 QP/宏块类型");
    }
    const int sourceX = column * frame.unitSize;
    const int sourceY = row * frame.unitSize;
    const int sourceRight = std::min(sourceX + frame.unitSize, m_stream->width);
    const int sourceBottom = std::min(sourceY + frame.unitSize, m_stream->height);
    const int actualWidth = sourceRight - sourceX;
    const int actualHeight = sourceBottom - sourceY;
    const QString blockError = av1 ? frame.av1CodingBlocksError
                                   : frame.hevcCodingUnitsError;
    const QString partition = (hevc || av1)
        ? (blockError.isEmpty()
            ? (av1
                ? QStringLiteral("· 正在加载该 Superblock 内部 Coding Block")
                : QStringLiteral("· 正在加载该 CTU 内部 CU 四叉树"))
            : QStringLiteral("· %1").arg(blockError))
        : QString();
    Q_EMIT unitHovered(
        QStringLiteral("%1 %2 · 列 %3 行 %4 · 当前尺寸 %5×%6 · 源像素 (%7, %8)–(%9, %10) · %11 · %12 %13")
            .arg(unit).arg(address).arg(column).arg(row)
            .arg(actualWidth).arg(actualHeight)
            .arg(sourceX).arg(sourceY).arg(sourceRight - 1).arg(sourceBottom - 1)
            .arg(decoded, slice, partition));
    update();
}

void MacroblockView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_zoom > 1.0
        && pictureRect().contains(event->position())) {
        m_panning = true;
        m_lastPanPosition = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void MacroblockView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_panning) {
        m_panning = false;
        setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void MacroblockView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        resetView();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void MacroblockView::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const double factor = std::pow(1.0015, event->angleDelta().y());
    setZoomAt(m_zoom * factor, event->position());
    event->accept();
}

void MacroblockView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    clampPan();
}

void MacroblockView::leaveEvent(QEvent *)
{
    m_hover = {-1, -1};
    m_hoverCu = -1;
    Q_EMIT unitHovered({});
    update();
}

} // namespace bitscope
