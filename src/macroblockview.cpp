#include "macroblockview.h"
#include "avclearning.h"

#include <QApplication>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QScreen>

#include <algorithm>
#include <cmath>

namespace bitscope {

namespace {
constexpr double kMinimumZoom = 0.5;
constexpr double kMaximumZoom = 8.0;
constexpr double kViewMargin = 12.0;

QString motionDirection(int x, int y)
{
    if (!x && !y) return QStringLiteral("零位移");
    return (x < 0 ? QStringLiteral("左") : x > 0 ? QStringLiteral("右") : QString())
        + (y < 0 ? QStringLiteral("上") : y > 0 ? QStringLiteral("下") : QString());
}

QString motionLine(const MotionVectorInfo &v, const QString &label)
{
    return QStringLiteral("<span style='color:%1'>%2 · 区域 (%3,%4) %5×%6<br>"
        "MV (%7,%8) / %9 → Δ(%10,%11) px · %12</span><br>")
        .arg(v.list == 0 ? QStringLiteral("#3de2e5") : QStringLiteral("#ffbd66"), label)
        .arg(v.centerX - v.width / 2).arg(v.centerY - v.height / 2).arg(v.width).arg(v.height)
        .arg(v.motionX).arg(v.motionY).arg(v.scale).arg(v.dx(), 0, 'f', 2).arg(v.dy(), 0, 'f', 2)
        .arg(motionDirection(v.motionX, v.motionY));
}

class MacroblockBubble final : public QLabel {
public:
    explicit MacroblockBubble(QWidget *parent)
        : QLabel(parent, Qt::ToolTip | Qt::FramelessWindowHint
              | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput) {}
protected:
    void paintEvent(QPaintEvent *event) override
    {
        // Paint the translucent surface explicitly: stylesheet backgrounds on
        // translucent top-level QLabel windows vary across platform plugins.
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(65, 220, 225, 150), 1));
        painter.setBrush(QColor(12, 24, 36, 222));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5, .5, -.5, -.5), 10, 10);
        painter.end();
        QLabel::paintEvent(event);
    }
};
}

MacroblockView::MacroblockView(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_hoverBubble = new MacroblockBubble(this);
    m_hoverBubble->setObjectName(QStringLiteral("macroblockBubble"));
    m_hoverBubble->setAttribute(Qt::WA_TranslucentBackground);
    m_hoverBubble->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_hoverBubble->setAttribute(Qt::WA_ShowWithoutActivating);
    m_hoverBubble->setFocusPolicy(Qt::NoFocus);
    m_hoverBubble->setTextFormat(Qt::RichText);
    m_hoverBubble->setWordWrap(true);
    m_hoverBubble->setStyleSheet(QStringLiteral("QLabel#macroblockBubble {"
        "background:transparent; color:#eaf2f8; border:none; padding:12px; }"));
    m_hoverBubble->hide();
    setFocusPolicy(Qt::ClickFocus);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive) { m_hover = {-1, -1}; hideBubble(); }
    });
}

void MacroblockView::setStream(StreamPtr stream)
{
    hideBubble();
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
    hideBubble();
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
    // Re-evaluate the same cursor location when async block data arrives.
    if (m_hover.x() >= 0 && isVisible() && !m_panning) showBubble();
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
    hideBubble();
    m_hover = {-1, -1};
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
    hideBubble();
    m_hover = {-1, -1};
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
    m_hoverPosition = event->position();
    if (m_pressed && !m_dragged && (event->position() - m_pressPosition).manhattanLength() >= QApplication::startDragDistance()) {
        m_dragged = true;
        m_panning = m_zoom > 1.0 && pictureRect().contains(m_pressPosition);
        if (m_panning) setCursor(Qt::ClosedHandCursor);
    }
    if (m_panning) {
        hideBubble();
        m_pan += event->position() - m_lastPanPosition;
        m_lastPanPosition = event->position();
        clampPan();
        update();
        event->accept();
        return;
    }
    if (!m_stream || m_frameIndex < 0 || m_frameIndex >= m_stream->frames.size()) {
        hideBubble();
        return;
    }

    const auto &frame = m_stream->frames.at(m_frameIndex);
    const QRectF picture = pictureRect();
    if (!picture.contains(event->position()) || frame.unitColumns <= 0 || frame.unitRows <= 0
        || frame.unitSize <= 0 || m_stream->width <= 0 || m_stream->height <= 0) {
        hideBubble();
        Q_EMIT unitHovered({});
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
    showBubble();
    if (m_hoverCu >= 0 && m_stream->codec == Codec::Hevc)
        address = frame.hevcCodingUnits.at(m_hoverCu).ctuAddress;
    else if (m_hoverCu >= 0 && m_stream->codec == Codec::Av1)
        address = frame.av1CodingBlocks.at(m_hoverCu).superblockAddress;
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
        if (block.intraBlockCopy || !reference.contains(QStringLiteral("INTRA FRAME"))) {
            if (!block.motionVectorsAvailable) reference += QStringLiteral(" · MV 未导出");
            else {
                reference += QStringLiteral(" · MV0(%1,%2)").arg(block.motionVector0X).arg(block.motionVector0Y);
                if (block.referenceFrames.contains('+'))
                    reference += QStringLiteral(" · MV1(%1,%2)").arg(block.motionVector1X).arg(block.motionVector1Y);
            }
        }
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
    hideBubble();
    m_hover = {-1, -1};
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        m_pressed = true;
        m_dragged = false;
        m_pressPosition = event->position();
        m_lastPanPosition = event->position();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void MacroblockView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_pressed) {
        m_pressed = false;
        m_panning = false;
        update();
        setCursor(m_zoom > 1.0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
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
    hideBubble();
    m_hover = {-1, -1};
    QWidget::resizeEvent(event);
    clampPan();
}

void MacroblockView::leaveEvent(QEvent *)
{
    hideBubble();
    m_hover = {-1, -1};
    m_hoverCu = -1;
    Q_EMIT unitHovered({});
    update();
}

void MacroblockView::hideEvent(QHideEvent *event)
{
    m_pressed = m_panning = false;
    hideBubble();
    m_hover = {-1, -1};
    QWidget::hideEvent(event);
}

void MacroblockView::hideBubble()
{
    if (m_hoverBubble) m_hoverBubble->hide();
}

QString MacroblockView::blockDetails(int address)
{
    const auto &frame = m_stream->frames.at(m_frameIndex);
    const auto value = [](int n) { return n < 0 ? QStringLiteral("未获取") : QString::number(n); };
    const auto safe = [](const QString &s) { return s.isEmpty() ? QStringLiteral("未获取") : s.toHtmlEscaped(); };
    const auto regionText = [&](int root, const QString &label) {
        for (const auto &r : frame.regions) if (root >= r.start && root < r.end)
            return QStringLiteral("%1 #%2 · %3（范围推导）<br>").arg(label).arg(r.sliceIndex).arg(safe(r.type));
        return label + QStringLiteral("：范围未获取<br>");
    };
    const auto ffmpegMotion = [&](bool intra) {
        if (intra) return QStringLiteral("<b>运动方向：不适用（帧内 / PCM）</b><br>");
        if (!frame.motionVectorsError.isEmpty()) return QStringLiteral("<b>MV 未获取：</b>%1<br>").arg(safe(frame.motionVectorsError));
        if (!frame.motionVectorsReady) {
            Q_EMIT motionRequested(m_frameIndex);
            return QStringLiteral("<b>运动矢量：正在按需提取…</b><br>");
        }
        const auto ids = frame.motionByMacroblock.value(address);
        if (ids.isEmpty()) return QStringLiteral("<b>此宏块未导出运动矢量</b>（不是零矢量）<br>");
        QString text = QStringLiteral("<b>实际 MV · 当前块 → 参考采样位置</b><br>");
        for (int id : ids) {
            const auto &v = frame.motionVectors.at(id);
            text += motionLine(v, m_stream->codec == Codec::Avc ? QStringLiteral("L%1").arg(v.list)
                : v.list == 0 ? QStringLiteral("前向参考") : QStringLiteral("后向参考"));
        }
        return text + QStringLiteral("<span style='color:#a5b7c8'>来源：AVMotionVector；按导出区域展示。<br>参考帧精确索引未导出；不是物体运动轨迹。</span><br>");
    };
    if (m_stream->codec == Codec::Hevc || m_stream->codec == Codec::Av1) {
        const bool hevc = m_stream->codec == Codec::Hevc;
        if (hevc && m_hoverCu >= 0 && m_hoverCu < frame.hevcCodingUnits.size()) {
            const auto &u = frame.hevcCodingUnits.at(m_hoverCu);
            QString html = QStringLiteral("<b style='color:#54dce0'>F%1 · H.265 CU #%2 · CTU #%3</b><br>"
                "像素 (%4, %5) · 深度 %6<br>编码尺寸 %7×%7 · 可见 %8×%9<br>"
                "分区：%10<br>预测：%11<br><b>实际 QP：%12</b><br>编码量：%13<br>")
                .arg(frame.index).arg(m_hoverCu).arg(u.ctuAddress).arg(u.x).arg(u.y).arg(u.depth)
                .arg(u.codedSize).arg(u.width).arg(u.height).arg(safe(u.partitionName), safe(u.prediction), value(u.qp),
                    u.approximateBytes < 0 ? QStringLiteral("未获取") : QStringLiteral("%1 bytes（CABAC 近似值）").arg(u.approximateBytes))
                + regionText(u.ctuAddress, QStringLiteral("Slice"));
            if (u.prediction == QStringLiteral("帧内预测")) html += QStringLiteral("运动矢量：不适用（帧内预测）<br>");
            else if (!u.motionVectorsReady) html += QStringLiteral("MV 未获取：%1<br>").arg(safe(u.motionVectorsError));
            else if (u.motionVectors.isEmpty()) html += QStringLiteral("无有效帧间 MV 记录（不是零矢量）<br>");
            else {
                html += QStringLiteral("<b>实际 MV · 当前 PU → 参考采样位置</b><br>");
                for (const auto &v : u.motionVectors)
                    html += motionLine(v, QStringLiteral("PU #%1 · L%2 · 参考 POC %3").arg(v.predictionUnit).arg(v.list).arg(v.referencePoc));
            }
            return html + QStringLiteral("<span style='color:#a5b7c8'>来源：HM；MV 原始值单位 1/4 像素。<br>参考 POC 为当前解码段的 POC，不是界面帧号。</span>");
        }
        if (!hevc && m_hoverCu >= 0 && m_hoverCu < frame.av1CodingBlocks.size()) {
            const auto &b = frame.av1CodingBlocks.at(m_hoverCu);
            QString html = QStringLiteral("<b style='color:#54dce0'>F%1 · AV1 Coding Block #%2</b><br>"
                "Superblock #%3 · 尺寸层级 %4 · 像素 (%5, %6)<br>编码尺寸 %7×%8 · 可见 %9×%10<br>"
                "Y 预测：%11 · UV：%12<br>变换：%13 / %14<br><b>QIndex：%15</b> · Segment：%16<br>"
                "Skip：%17 · 帧内块复制：%18<br>参考：%19<br>")
                .arg(frame.index).arg(m_hoverCu).arg(b.superblockAddress).arg(b.depth).arg(b.x).arg(b.y)
                .arg(b.codedWidth).arg(b.codedHeight).arg(b.width).arg(b.height)
                .arg(safe(b.predictionMode), safe(b.uvPredictionMode), safe(b.transformSize), safe(b.transformType),
                    value(b.qIndex), value(b.segmentId), b.skip ? QStringLiteral("是") : QStringLiteral("否"),
                    b.intraBlockCopy ? QStringLiteral("是") : QStringLiteral("否"), safe(b.referenceFrames));
            if (!b.intraBlockCopy && b.referenceFrames.contains(QStringLiteral("INTRA FRAME")))
                html += QStringLiteral("运动矢量：不适用（帧内预测）<br>");
            else if (!b.motionVectorsAvailable)
                html += QStringLiteral("运动矢量：未导出（不是零矢量）<br>");
            else {
                const auto refs = b.referenceFrames.split('+', Qt::SkipEmptyParts);
                const auto mv = [&](int index, int x, int y) {
                    return QStringLiteral("MV%1：(%2, %3) / 8 → Δ(%4, %5) px · %6<br>参考：%7<br>")
                        .arg(index).arg(x).arg(y).arg(x / 8., 0, 'f', 3).arg(y / 8., 0, 'f', 3)
                        .arg(motionDirection(x, y), b.intraBlockCopy ? QStringLiteral("当前帧（IBC）")
                            : index < refs.size() ? safe(refs[index].trimmed()) : QStringLiteral("未获取"));
                };
                html += mv(0, b.motionVector0X, b.motionVector0Y);
                if (b.referenceFrames.contains('+')) html += mv(1, b.motionVector1X, b.motionVector1Y);
            }
            return html + regionText(b.superblockAddress, QStringLiteral("Frame OBU"))
                + QStringLiteral("<span style='color:#a5b7c8'>来源：libaom；MV 原始值单位 1/8 像素。<br>全局 / 仿射模式的块 MV 不等于每个像素的位移场。</span>");
        }
        const int x = address % frame.unitColumns * frame.unitSize;
        const int y = address / frame.unitColumns * frame.unitSize;
        const bool ready = hevc ? frame.hevcCodingUnitsReady : frame.av1CodingBlocksReady;
        const QString error = hevc ? frame.hevcCodingUnitsError : frame.av1CodingBlocksError;
        return QStringLiteral("<b style='color:#54dce0'>F%1 · %2 #%3</b><br>像素 (%4, %5)<br>"
            "根单元 %6×%6 · 可见 %7×%8<br>%9<br><span style='color:#a5b7c8'>根网格不代表实际叶子块尺寸。</span>")
            .arg(frame.index).arg(hevc ? QStringLiteral("CTU") : QStringLiteral("Superblock"))
            .arg(address).arg(x).arg(y).arg(frame.unitSize)
            .arg(std::min(frame.unitSize, m_stream->width - x)).arg(std::min(frame.unitSize, m_stream->height - y))
            .arg(!error.isEmpty() ? safe(error) : ready ? QStringLiteral("当前位置没有已导出的叶子块信息") : QStringLiteral("详细块信息加载中，完成后自动更新"));
    }
    const int column = address % frame.unitColumns, row = address / frame.unitColumns;
    const DecodedUnitInfo *unit = frame.decodedUnitsReady && address >= 0 && address < frame.decodedUnits.size()
        ? &frame.decodedUnits.at(address) : nullptr;
    const QString code = unit ? unit->decoderType : QString();
    if (m_stream->codec == Codec::Mpeg4Visual) {
        const int x = column * frame.unitSize, y = row * frame.unitSize;
        return QStringLiteral("<b style='color:#54dce0'>F%1 · MPEG-4 宏块 #%2</b><br>"
            "网格：列 %3 / 行 %4 · 像素 (%5, %6)<br>编码尺寸 %7×%7 · 可见 %8×%9<br>"
            "<b>实际 QP：%10</b><br>类型：%11 · 日志标记：%12<br>")
            .arg(frame.index).arg(address).arg(column).arg(row).arg(x).arg(y).arg(frame.unitSize)
            .arg(std::min(frame.unitSize, m_stream->width - x)).arg(std::min(frame.unitSize, m_stream->height - y))
            .arg(unit ? value(unit->qp) : QStringLiteral("未获取"), unit ? safe(unit->category) : QStringLiteral("未获取"), safe(code))
            + regionText(address, QStringLiteral("VOP"))
            + ffmpegMotion(!code.isEmpty() && QStringLiteral("PiI").contains(code.front()))
            + QStringLiteral("<span style='color:#a5b7c8'>QP / 类型来自 FFmpeg 解码日志。</span>")
            + (!frame.decodedUnitsError.isEmpty() ? QStringLiteral("<br>%1").arg(safe(frame.decodedUnitsError))
               : !frame.decodedUnitsReady ? QStringLiteral("<br>宏块数据加载中，完成后自动更新") : QString());
    }
    const bool intra = !code.isEmpty() && QStringLiteral("PiI").contains(code.front());
    QString prediction = avcPredictionName(code);
    const int x = column * frame.unitSize, y = row * frame.unitSize;
    QString html = QStringLiteral("<b style='color:#54dce0'>F%1 · H.264 宏块 #%2</b><br>"
        "网格：列 %3 / 行 %4 · 像素 (%5, %6)<br>"
        "编码网格 %7×%7 · 可见 %8×%9<br>")
        .arg(frame.index).arg(address).arg(column).arg(row).arg(x).arg(y)
        .arg(frame.unitSize).arg(std::min(frame.unitSize, m_stream->width - x))
        .arg(std::min(frame.unitSize, m_stream->height - y));
    const auto region = std::find_if(frame.regions.cbegin(), frame.regions.cend(), [address](const CodingUnitRegion &r) {
        return address >= r.start && address < r.end;
    });
    html += region == frame.regions.cend() ? QStringLiteral("Slice：范围未获取<br>")
        : QStringLiteral("Slice #%1 · %2（范围推导）<br>").arg(region->sliceIndex).arg(region->type.toHtmlEscaped());
    html += QStringLiteral("<b>实际 QP：%1</b> · 日志标记：%2<br>预测：%3<br>分区：%4<br>")
        .arg(unit && unit->qp >= 0 ? QString::number(unit->qp) : QStringLiteral("未获取"),
             code.isEmpty() ? QStringLiteral("未获取") : code.toHtmlEscaped(),
             prediction.toHtmlEscaped(), avcPartitionName(code).toHtmlEscaped());
    html += ffmpegMotion(intra);
    html += QStringLiteral("<span style='color:#a5b7c8'>来源：FFmpeg 解码日志；标记不等于 mb_type 数值。<br>"
        "参考索引、帧内方向、变换系数 / 残差：未导出。</span>");
    if (code.contains('=')) html += QStringLiteral("<br>场编码标记：当前网格不是完整 MBAFF 地址映射。");
    if (!frame.decodedUnitsReady || !frame.decodedUnitsError.isEmpty())
        html += QStringLiteral("<br><span style='color:#f4a261'>%1</span>").arg(frame.decodedUnitsError.isEmpty()
            ? QStringLiteral("宏块数据加载中，完成后自动更新") : frame.decodedUnitsError.left(160).toHtmlEscaped());

    return html;
}

void MacroblockView::showBubble()
{
    if (!m_stream || !isVisible() || m_panning || m_pressed
        || m_frameIndex < 0 || m_frameIndex >= m_stream->frames.size() || m_hover.x() < 0) {
        hideBubble(); return;
    }
    const auto &frame = m_stream->frames.at(m_frameIndex);
    // Resolve from the exact cursor position also when asynchronous data arrives.
    // Searching just once here keeps the existing hover hit-test cost unchanged.
    m_hoverCu = -1;
    const QRectF picture = pictureRect();
    if (!picture.contains(m_hoverPosition) || frame.unitColumns <= 0 || frame.unitRows <= 0) { hideBubble(); return; }
    const int px = int((m_hoverPosition.x() - picture.left()) * m_stream->width / picture.width());
    const int py = int((m_hoverPosition.y() - picture.top()) * m_stream->height / picture.height());
    const auto findBlock = [px, py](const auto &blocks) {
        for (int i = 0; i < blocks.size(); ++i) {
            const auto &b = blocks.at(i);
            if (px >= b.x && py >= b.y && px < b.x + b.width && py < b.y + b.height) return i;
        }
        return -1;
    };
    if (m_stream->codec == Codec::Hevc && frame.hevcCodingUnitsReady) m_hoverCu = findBlock(frame.hevcCodingUnits);
    else if (m_stream->codec == Codec::Av1 && frame.av1CodingBlocksReady) m_hoverCu = findBlock(frame.av1CodingBlocks);
    const QString html = blockDetails(m_hover.y() * frame.unitColumns + m_hover.x());
    const QPoint cursor = mapToGlobal(m_hoverPosition.toPoint());
    QScreen *screen = QGuiApplication::screenAt(cursor);
    if (!screen) screen = this->screen();
    const QRect available = screen->availableGeometry().adjusted(8, 8, -8, -8);
    const int bubbleWidth = std::min(440, available.width());
    if (m_hoverBubble->text() != html || m_hoverBubble->width() != bubbleWidth) {
        m_hoverBubble->setFixedWidth(bubbleWidth);
        m_hoverBubble->setText(html);
        m_hoverBubble->adjustSize();
    }
    QPoint origin = cursor + QPoint(18, 22);
    if (origin.x() + m_hoverBubble->width() > available.right()) origin.setX(cursor.x() - m_hoverBubble->width() - 18);
    if (origin.y() + m_hoverBubble->height() > available.bottom()) origin.setY(cursor.y() - m_hoverBubble->height() - 18);
    origin.setX(std::clamp(origin.x(), available.left(), std::max(available.left(), available.right() - m_hoverBubble->width() + 1)));
    origin.setY(std::clamp(origin.y(), available.top(), std::max(available.top(), available.bottom() - m_hoverBubble->height() + 1)));
    m_hoverBubble->move(origin);
    m_hoverBubble->show();
}

} // namespace bitscope
