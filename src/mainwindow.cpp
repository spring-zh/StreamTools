#include "mainwindow.h"

#include "av1blockprovider.h"
#include "bitstreamparser.h"
#include "frametablemodel.h"
#include "hevccuprovider.h"
#include "macroblockview.h"
#include "previewprovider.h"
#include "timelinewidget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHeaderView>
#include <QHash>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <functional>

namespace bitscope {
namespace {

class StreamFilterProxy final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setNeedle(QString value) { m_needle = std::move(value); refresh(); }
    void setType(QString value) { m_type = std::move(value); refresh(); }
protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override {
        const QModelIndex sourceIndex = sourceModel()->index(row, 0, parent);
        const QString search = sourceIndex.data(FrameTableModel::SearchTextRole).toString();
        const QString type = sourceIndex.data(FrameTableModel::TypeRole).toString();
        const bool typeMatch = m_type.isEmpty()
            || (m_type == QStringLiteral("@random") && type.contains(QStringLiteral("@random")))
            || (m_type == QStringLiteral("@slice") && QStringLiteral("IPBS").contains(type.left(1)))
            || type.contains(m_type, Qt::CaseInsensitive);
        return (m_needle.isEmpty() || search.contains(m_needle, Qt::CaseInsensitive)) && typeMatch;
    }
private:
    void refresh() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        endFilterChange(Direction::Rows);
#else
        invalidateFilter();
#endif
    }
    QString m_needle;
    QString m_type;
};

QString formatBytes(qint64 bytes) {
    if (bytes >= 1024 * 1024) return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 2);
    if (bytes >= 1024) return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(bytes);
}

QString timeText(double seconds) {
    const int totalMs = int(seconds * 1000.0 + .5);
    return QStringLiteral("%1:%2:%3.%4")
        .arg(totalMs / 3600000, 2, 10, QChar('0'))
        .arg((totalMs / 60000) % 60, 2, 10, QChar('0'))
        .arg((totalMs / 1000) % 60, 2, 10, QChar('0'))
        .arg(totalMs % 1000, 3, 10, QChar('0'));
}

QString hexDump(const QByteArray &data, qint64 baseOffset) {
    const int limit = std::min(int(data.size()), 64 * 1024);
    QString out;
    out.reserve(limit * 4);
    for (int row = 0; row < limit; row += 16) {
        out += QStringLiteral("%1  ").arg(baseOffset + row, 8, 16, QChar('0')).toUpper();
        QString ascii;
        for (int col = 0; col < 16; ++col) {
            if (row + col < limit) {
                const uchar byte = uchar(data.at(row + col));
                out += QStringLiteral("%1 ").arg(byte, 2, 16, QChar('0')).toUpper();
                ascii += byte >= 32 && byte <= 126 ? QChar(byte) : QChar('.');
            } else {
                out += QStringLiteral("   ");
                ascii += QChar(' ');
            }
            if (col == 7) out += QChar(' ');
        }
        out += QStringLiteral(" |%1|\n").arg(ascii);
    }
    if (data.size() > limit) out += QStringLiteral("\n… 仅显示前 64 KB，共 %1\n").arg(formatBytes(data.size()));
    return out;
}

QLabel *metricLabel(const QString &caption, QWidget *parent) {
    auto *label = new QLabel(QStringLiteral("<span style='color:#6f8298'>%1</span><br><b>—</b>").arg(caption), parent);
    label->setMinimumWidth(115);
    label->setObjectName(QStringLiteral("metric"));
    return label;
}

QTreeWidgetItem *addField(QTreeWidgetItem *parent, const SyntaxField &fieldValue) {
    auto *item = new QTreeWidgetItem(parent, {fieldValue.name, fieldValue.value, fieldValue.bits, fieldValue.description});
    item->setToolTip(0, fieldValue.description);
    return item;
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    qRegisterMetaType<StreamPtr>();
    setAcceptDrops(true);
    setWindowTitle(QStringLiteral("BitScope · MP4 / H.264 / H.265 / MPEG-4 / AV1 码流分析器"));
    resize(1500, 940);
    buildUi();
    buildMenus();
    applyTheme();
    m_previews = new PreviewProvider(this);
    m_hevcCuProvider = new HevcCuProvider(this);
    m_av1BlockProvider = new Av1BlockProvider(this);

    connect(&m_watcher, &QFutureWatcher<StreamPtr>::finished, this, &MainWindow::parseFinished);
    connect(m_previews, &PreviewProvider::thumbnailReady, this, [this](int frame, const QImage &image) {
        m_model->setThumbnail(frame, image);
        m_macroView->setFrameImage(frame, image);
        if (frame == m_currentFrame)
            requestCurrentBlockData(frame);
    });
    connect(m_previews, &PreviewProvider::decodedUnitsReady, this,
            [this](int frameIndex, const QVector<DecodedUnitInfo> &units) {
        if (!m_stream || frameIndex < 0 || frameIndex >= m_stream->frames.size())
            return;
        auto &frame = m_stream->frames[frameIndex];
        frame.decodedUnits = units;
        frame.decodedUnitsReady = true;
        frame.decodedUnitsError.clear();
        if (frameIndex == m_currentFrame) {
            m_macroView->refreshFrameData();
            updateFrameStatistics(frameIndex);
        }
    });
    connect(m_previews, &PreviewProvider::decodedUnitsUnavailable, this,
            [this](int frameIndex, const QString &reason) {
        if (!m_stream || frameIndex < 0 || frameIndex >= m_stream->frames.size())
            return;
        auto &frame = m_stream->frames[frameIndex];
        frame.decodedUnitsError = reason;
        if (frameIndex == m_currentFrame) {
            m_macroView->refreshFrameData();
            updateFrameStatistics(frameIndex);
            updateCuTree(frameIndex);
        }
    });
    connect(m_model, &FrameTableModel::thumbnailEvicted, m_previews, &PreviewProvider::forgetFrame);
    connect(m_previews, &PreviewProvider::statusChanged, m_status, &QLabel::setText);
    connect(m_previews, &PreviewProvider::previewFailed, this, [this](int frame, const QString &reason) {
        m_macroView->setFrameError(frame, reason.left(500));
        if (m_table->currentIndex().data(FrameTableModel::ItemIndexRole).toInt() == frame) {
            m_status->setText(QStringLiteral("Frame %1 预览失败：%2").arg(frame).arg(reason.left(180)));
            requestCurrentBlockData(frame);
        }
    });
    connect(m_previews, &PreviewProvider::queueIdle,
            this, &MainWindow::requestVisibleBlockData);
    connect(m_hevcCuProvider, &HevcCuProvider::codingUnitsReady, this,
            [this](int frameIndex, const QVector<HevcCodingUnitInfo> &units) {
        if (!m_stream || frameIndex < 0 || frameIndex >= m_stream->frames.size())
            return;
        FrameInfo &frame = m_stream->frames[frameIndex];
        frame.hevcCodingUnits = units;
        frame.hevcCodingUnitsReady = true;
        frame.hevcCodingUnitsError.clear();
        if (frameIndex == m_currentFrame) {
            m_macroView->refreshFrameData();
            updateFrameStatistics(frameIndex);
            updateCuTree(frameIndex);
            if (m_previews->isIdle())
                requestVisibleBlockData();
        }
    });
    connect(m_hevcCuProvider, &HevcCuProvider::codingUnitsUnavailable, this,
            [this](int frameIndex, const QString &reason) {
        if (!m_stream || frameIndex < 0 || frameIndex >= m_stream->frames.size())
            return;
        FrameInfo &frame = m_stream->frames[frameIndex];
        frame.hevcCodingUnitsError = reason;
        if (frameIndex == m_currentFrame) {
            m_macroView->refreshFrameData();
            updateFrameStatistics(frameIndex);
            updateCuTree(frameIndex);
            if (m_previews->isIdle())
                requestVisibleBlockData();
        }
    });
    connect(m_hevcCuProvider, &HevcCuProvider::statusChanged, m_status, &QLabel::setText);
    connect(m_av1BlockProvider, &Av1BlockProvider::codingBlocksReady, this,
            [this](int frameIndex, const QVector<Av1CodingBlockInfo> &blocks) {
        if (!m_stream || frameIndex < 0 || frameIndex >= m_stream->frames.size())
            return;
        FrameInfo &frame = m_stream->frames[frameIndex];
        frame.av1CodingBlocks = blocks;
        frame.av1CodingBlocksReady = true;
        frame.av1CodingBlocksError.clear();
        if (frameIndex == m_currentFrame) {
            m_macroView->refreshFrameData();
            updateFrameStatistics(frameIndex);
            updateCuTree(frameIndex);
            if (m_previews->isIdle())
                requestVisibleBlockData();
        }
    });
    connect(m_av1BlockProvider, &Av1BlockProvider::codingBlocksUnavailable, this,
            [this](int frameIndex, const QString &reason) {
        if (!m_stream || frameIndex < 0 || frameIndex >= m_stream->frames.size())
            return;
        FrameInfo &frame = m_stream->frames[frameIndex];
        frame.av1CodingBlocksError = reason;
        if (frameIndex == m_currentFrame) {
            m_macroView->refreshFrameData();
            updateFrameStatistics(frameIndex);
            updateCuTree(frameIndex);
            if (m_previews->isIdle())
                requestVisibleBlockData();
        }
    });
    connect(m_av1BlockProvider, &Av1BlockProvider::statusChanged, m_status, &QLabel::setText);
    clearDetails(QStringLiteral("拖入或打开 MP4、H.264、H.265、MPEG-4 Part 2 或 AV1 视频开始分析"));
}

MainWindow::~MainWindow() {
    if (m_watcher.isRunning()) m_watcher.waitForFinished();
}

void MainWindow::requestFileView()
{
    m_preferFileView = true;
    if (m_mode->count() > 2)
        m_mode->setCurrentIndex(2);
}

void MainWindow::buildUi() {
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(14, 12, 14, 10);
    root->setSpacing(10);

    auto *top = new QHBoxLayout;
    auto *brand = new QLabel(QStringLiteral("<b style='font-size:19px;color:#f1f6fb'>BitScope</b>  <span style='color:#19c2c9'>Stream Analyzer</span>"), central);
    m_fileLabel = new QLabel(QStringLiteral("尚未打开码流"), central);
    m_fileLabel->setObjectName(QStringLiteral("fileLabel"));
    m_fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *titleBox = new QVBoxLayout;
    titleBox->setSpacing(2);
    titleBox->addWidget(brand);
    titleBox->addWidget(m_fileLabel);
    top->addLayout(titleBox, 1);
    auto *open = new QPushButton(QStringLiteral("打开码流…"), central);
    open->setObjectName(QStringLiteral("primaryButton"));
    connect(open, &QPushButton::clicked, this, &MainWindow::chooseFile);
    top->addWidget(open);
    root->addLayout(top);

    auto *metrics = new QHBoxLayout;
    m_codecMetric = metricLabel(QStringLiteral("编码"), central);
    m_resolutionMetric = metricLabel(QStringLiteral("分辨率"), central);
    m_frameMetric = metricLabel(QStringLiteral("帧 / 码流单元"), central);
    m_rateMetric = metricLabel(QStringLiteral("帧率 / 码率"), central);
    metrics->addWidget(m_codecMetric);
    metrics->addWidget(m_resolutionMetric);
    metrics->addWidget(m_frameMetric);
    metrics->addWidget(m_rateMetric);
    metrics->addStretch(1);
    root->addLayout(metrics);

    auto *toolbar = new QFrame(central);
    toolbar->setObjectName(QStringLiteral("toolbar"));
    auto *tools = new QHBoxLayout(toolbar);
    tools->setContentsMargins(9, 7, 9, 7);
    m_mode = new QComboBox(toolbar);
    m_mode->addItems({QStringLiteral("帧视图"), QStringLiteral("码流单元视图")});
    m_typeFilter = new QComboBox(toolbar);
    m_typeFilter->addItem(QStringLiteral("全部类型"), QString());
    m_typeFilter->addItem(QStringLiteral("IDR / IRAP"), QStringLiteral("@random"));
    m_typeFilter->addItem(QStringLiteral("I 帧"), QStringLiteral("I"));
    m_typeFilter->addItem(QStringLiteral("P 帧"), QStringLiteral("P"));
    m_typeFilter->addItem(QStringLiteral("B 帧"), QStringLiteral("B"));
    m_search = new QLineEdit(toolbar);
    m_search->setPlaceholderText(QStringLiteral("搜索帧号、类型、偏移、码流单元…  ( / )"));
    m_search->setClearButtonEnabled(true);
    tools->addWidget(m_mode);
    tools->addWidget(m_typeFilter);
    tools->addWidget(m_search, 1);
    root->addWidget(toolbar);

    auto *mainSplit = new QSplitter(Qt::Horizontal, central);
    m_model = new FrameTableModel(this);
    auto *proxy = new StreamFilterProxy(this);
    m_proxy = proxy;
    m_proxy->setSourceModel(m_model);
    m_leftStack = new QStackedWidget(mainSplit);
    m_table = new QTableView(m_leftStack);
    m_table->setModel(m_proxy);
    m_table->setAlternatingRowColors(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setSortingEnabled(false);
    m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(0, 56);
    m_table->setColumnWidth(1, 112);
    m_table->setColumnWidth(2, 82);
    m_table->setColumnWidth(3, 112);
    m_table->setColumnWidth(4, 112);
    m_table->setColumnWidth(5, 110);
    m_leftStack->addWidget(m_table);
    m_fileTree = new QTreeWidget(m_leftStack);
    m_fileTree->setHeaderLabels({QStringLiteral("MP4 Box"), QStringLiteral("偏移"), QStringLiteral("大小"), QStringLiteral("说明")});
    m_fileTree->setAlternatingRowColors(true);
    m_fileTree->setRootIsDecorated(true);
    m_fileTree->setUniformRowHeights(true);
    m_fileTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_fileTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_fileTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_fileTree->header()->setStretchLastSection(true);
    m_leftStack->addWidget(m_fileTree);
    mainSplit->addWidget(m_leftStack);

    m_tabs = new QTabWidget(mainSplit);
    auto *picturePage = new QWidget(m_tabs);
    auto *pictureLayout = new QVBoxLayout(picturePage);
    pictureLayout->setContentsMargins(7, 7, 7, 7);
    auto *overlayTools = new QHBoxLayout;
    auto *grid = new QCheckBox(QStringLiteral("宏块 / CTU·CU / Superblock 网格"), picturePage);
    auto *slices = new QCheckBox(QStringLiteral("Slice / VOP / Frame OBU 分区"), picturePage);
    auto *qpOverlay = new QCheckBox(QStringLiteral("QP 热力图"), picturePage);
    qpOverlay->setObjectName(QStringLiteral("qpOverlay"));
    grid->setChecked(true);
    grid->setToolTip(QStringLiteral("H.264 / MPEG-4 Part 2 显示 16×16 宏块；H.265 显示 CTU/CU；AV1 显示 Superblock 与实际 Coding Block"));
    slices->setChecked(true);
    qpOverlay->setChecked(false);
    qpOverlay->setToolTip(QStringLiteral("按实际解码量化值着色：蓝色较低，红色较高；AV1 按 Coding Block QIndex"));
    overlayTools->addWidget(grid);
    overlayTools->addWidget(slices);
    overlayTools->addWidget(qpOverlay);
    overlayTools->addStretch(1);
    auto *previewZoomOut = new QToolButton(picturePage);
    previewZoomOut->setText(QStringLiteral("−"));
    previewZoomOut->setToolTip(QStringLiteral("缩小画面"));
    auto *previewZoomLabel = new QLabel(QStringLiteral("100%"), picturePage);
    previewZoomLabel->setAlignment(Qt::AlignCenter);
    previewZoomLabel->setMinimumWidth(48);
    auto *previewZoomIn = new QToolButton(picturePage);
    previewZoomIn->setText(QStringLiteral("+"));
    previewZoomIn->setToolTip(QStringLiteral("放大画面"));
    auto *previewZoomFit = new QToolButton(picturePage);
    previewZoomFit->setText(QStringLiteral("适合窗口"));
    previewZoomFit->setToolTip(QStringLiteral("恢复为适合窗口大小（双击画面也可恢复）"));
    overlayTools->addWidget(new QLabel(QStringLiteral("画面缩放"), picturePage));
    overlayTools->addWidget(previewZoomOut);
    overlayTools->addWidget(previewZoomLabel);
    overlayTools->addWidget(previewZoomIn);
    overlayTools->addWidget(previewZoomFit);
    pictureLayout->addLayout(overlayTools);
    m_frameStats = new QLabel(QStringLiteral("当前帧统计 · 等待选择视频帧"), picturePage);
    m_frameStats->setObjectName(QStringLiteral("frameStats"));
    m_frameStats->setTextFormat(Qt::RichText);
    m_frameStats->setWordWrap(true);
    pictureLayout->addWidget(m_frameStats);
    m_macroView = new MacroblockView(picturePage);
    pictureLayout->addWidget(m_macroView, 1);
    const QString previewHint = QStringLiteral("滚轮缩放 · 放大后拖动画面 · 移动鼠标查看宏块 / CTU / Superblock 的真实尺寸、地址和所属帧单元");
    m_unitInfo = new QLabel(previewHint, picturePage);
    m_unitInfo->setObjectName(QStringLiteral("hint"));
    m_unitInfo->setWordWrap(true);
    pictureLayout->addWidget(m_unitInfo);
    connect(grid, &QCheckBox::toggled, m_macroView, &MacroblockView::setGridVisible);
    connect(slices, &QCheckBox::toggled, m_macroView, &MacroblockView::setSliceOverlayVisible);
    connect(qpOverlay, &QCheckBox::toggled, m_macroView, &MacroblockView::setQpOverlayVisible);
    connect(previewZoomOut, &QToolButton::clicked, m_macroView, [this] { m_macroView->zoomBy(1.0 / 1.25); });
    connect(previewZoomIn, &QToolButton::clicked, m_macroView, [this] { m_macroView->zoomBy(1.25); });
    connect(previewZoomFit, &QToolButton::clicked, m_macroView, &MacroblockView::resetView);
    connect(m_macroView, &MacroblockView::zoomChanged, previewZoomLabel, [previewZoomLabel](double zoom) {
        previewZoomLabel->setText(QStringLiteral("%1%").arg(qRound(zoom * 100.0)));
    });
    connect(m_macroView, &MacroblockView::unitHovered, this, [this](const QString &text) {
        m_unitInfo->setText(text.isEmpty()
            ? QStringLiteral("滚轮缩放 · 放大后拖动画面 · 移动鼠标查看宏块 / CTU / Superblock 的真实尺寸、地址和所属帧单元")
            : text);
    });
    m_tabs->addTab(picturePage, QStringLiteral("画面 / 宏块"));

    m_cuTree = new QTreeWidget(m_tabs);
    m_cuTree->setHeaderLabels({QStringLiteral("CTU / CU"), QStringLiteral("位置"),
        QStringLiteral("编码尺寸"), QStringLiteral("深度"), QStringLiteral("分区"),
        QStringLiteral("预测"), QStringLiteral("QP"), QStringLiteral("近似字节")});
    m_cuTree->setAlternatingRowColors(true);
    m_cuTree->setUniformRowHeights(true);
    m_cuTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_cuTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_cuTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_cuTree->header()->setStretchLastSection(false);
    connect(m_cuTree, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem *item) { populateCuTreeChildren(item); });
    m_tabs->addTab(m_cuTree, QStringLiteral("CTU / CU"));

    m_slices = new QTreeWidget(m_tabs);
    m_slices->setHeaderLabels({QStringLiteral("Slice"), QStringLiteral("类型"), QStringLiteral("起始地址"), QStringLiteral("覆盖范围"), QStringLiteral("PPS"), QStringLiteral("实际平均 QP")});
    m_slices->setRootIsDecorated(false);
    m_slices->setAlternatingRowColors(true);
    m_tabs->addTab(m_slices, QStringLiteral("Slice"));

    m_syntax = new QTreeWidget(m_tabs);
    m_syntax->setHeaderLabels({QStringLiteral("语法字段"), QStringLiteral("值"), QStringLiteral("编码"), QStringLiteral("说明")});
    m_syntax->setAlternatingRowColors(true);
    m_syntax->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_syntax->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_syntax->header()->setStretchLastSection(true);
    m_tabs->addTab(m_syntax, QStringLiteral("语法树 / SEI"));

    m_references = new QTextBrowser(m_tabs);
    m_references->setOpenExternalLinks(false);
    m_tabs->addTab(m_references, QStringLiteral("参考关系"));

    m_hex = new QPlainTextEdit(m_tabs);
    m_hex->setReadOnly(true);
    QFont mono(QStringLiteral("Menlo"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(10);
    m_hex->setFont(mono);
    m_tabs->addTab(m_hex, QStringLiteral("Hex"));

    m_summary = new QTextBrowser(m_tabs);
    m_tabs->addTab(m_summary, QStringLiteral("摘要 / 诊断"));
    mainSplit->addWidget(m_tabs);
    mainSplit->setStretchFactor(0, 5);
    mainSplit->setStretchFactor(1, 4);
    root->addWidget(mainSplit, 1);

    auto *timelineBox = new QFrame(central);
    timelineBox->setObjectName(QStringLiteral("timelineBox"));
    auto *timelineLayout = new QVBoxLayout(timelineBox);
    timelineLayout->setContentsMargins(0, 0, 0, 0);
    auto *zoomRow = new QHBoxLayout;
    zoomRow->setContentsMargins(12, 5, 12, 0);
    zoomRow->addStretch(1);
    zoomRow->addWidget(new QLabel(QStringLiteral("时间线缩放"), timelineBox));
    auto *zoomOut = new QToolButton(timelineBox);
    zoomOut->setText(QStringLiteral("−"));
    auto *zoomIn = new QToolButton(timelineBox);
    zoomIn->setText(QStringLiteral("+"));
    zoomRow->addWidget(zoomOut);
    zoomRow->addWidget(zoomIn);
    timelineLayout->addLayout(zoomRow);
    m_timeline = new TimelineWidget(timelineBox);
    timelineLayout->addWidget(m_timeline);
    root->addWidget(timelineBox);

    m_status = new QLabel(QStringLiteral("就绪"), this);
    m_progress = new QProgressBar(this);
    m_progress->setMaximumWidth(160);
    m_progress->setTextVisible(false);
    m_progress->hide();
    statusBar()->addWidget(m_status, 1);
    statusBar()->addPermanentWidget(m_progress);

    setCentralWidget(central);

    connect(m_mode, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::setViewMode);
    connect(m_typeFilter, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateFilters(); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { updateFilters(); });
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex &current) { selectionChanged(current); });
    connect(m_fileTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *current) {
        if (current) showFileBox(current);
    });
    connect(m_table->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { QTimer::singleShot(80, this, &MainWindow::updateVisiblePreviews); });
    connect(m_timeline, &TimelineWidget::frameActivated, this, &MainWindow::selectFrame);
    connect(zoomOut, &QToolButton::clicked, this, [this] { m_timeline->setZoom(m_timeline->zoom() / 1.25); });
    connect(zoomIn, &QToolButton::clicked, this, [this] { m_timeline->setZoom(m_timeline->zoom() * 1.25); });
}

void MainWindow::buildMenus() {
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("文件"));
    auto *open = fileMenu->addAction(QStringLiteral("打开码流…"));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::chooseFile);
    auto *quit = fileMenu->addAction(QStringLiteral("退出"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    auto *viewMenu = menuBar()->addMenu(QStringLiteral("视图"));
    auto *frames = viewMenu->addAction(QStringLiteral("帧视图"));
    frames->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
    connect(frames, &QAction::triggered, this, [this] { m_mode->setCurrentIndex(0); });
    auto *nals = viewMenu->addAction(QStringLiteral("码流单元视图"));
    nals->setShortcut(QKeySequence(QStringLiteral("Ctrl+2")));
    connect(nals, &QAction::triggered, this, [this] { m_mode->setCurrentIndex(1); });
    auto *boxes = viewMenu->addAction(QStringLiteral("MP4 文件视图"));
    boxes->setShortcut(QKeySequence(QStringLiteral("Ctrl+3")));
    connect(boxes, &QAction::triggered, this, [this] { if (m_mode->count() > 2) m_mode->setCurrentIndex(2); });
    auto *focusSearch = new QShortcut(QKeySequence(QStringLiteral("/")), this);
    connect(focusSearch, &QShortcut::activated, m_search, qOverload<>(&QWidget::setFocus));
}

void MainWindow::applyTheme() {
    qApp->setStyle(QStringLiteral("Fusion"));
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget { background:#0b1119; color:#dbe5ef; font-size:13px; }
        QMenuBar, QMenu, QStatusBar { background:#0d151f; color:#cdd8e4; }
        QMenu::item:selected { background:#18354a; }
        #fileLabel, #hint { color:#8191a5; }
        #frameStats { background:#101923; color:#b8c7d8; border:1px solid #243343; border-radius:6px; padding:7px 10px; }
        #metric { background:#101923; border:1px solid #1e2d3d; border-radius:7px; padding:8px 12px; }
        #toolbar, #timelineBox { background:#0f1822; border:1px solid #1e2d3d; border-radius:7px; }
        #primaryButton { background:#19aeb5; color:#061014; border:0; border-radius:6px; padding:9px 17px; font-weight:700; }
        #primaryButton:hover { background:#29c4cb; }
        QLineEdit, QComboBox, QPlainTextEdit, QTextBrowser, QTreeWidget, QTableView {
            background:#0d151f; color:#dbe5ef; border:1px solid #243343; border-radius:5px;
            selection-background-color:#15536b; selection-color:white;
        }
        QLineEdit, QComboBox { padding:6px 8px; min-height:18px; }
        QHeaderView::section { background:#111d29; color:#8fa1b5; padding:7px; border:0; border-right:1px solid #243343; border-bottom:1px solid #243343; }
        QTableView { alternate-background-color:#0f1822; }
        QTreeWidget { alternate-background-color:#101a25; }
        QTabWidget::pane { border:1px solid #243343; background:#0d151f; }
        QTabBar::tab { background:#101923; color:#8191a5; padding:8px 12px; border:1px solid #1e2d3d; }
        QTabBar::tab:selected { color:#eaf2f8; border-bottom:2px solid #19c2c9; }
        QCheckBox { spacing:7px; }
        QScrollBar:vertical { background:#0d151f; width:11px; }
        QScrollBar::handle:vertical { background:#2b3d50; border-radius:5px; min-height:24px; }
        QToolButton, QPushButton { background:#172432; border:1px solid #2a3c4f; border-radius:5px; padding:5px 9px; }
        QToolButton:hover, QPushButton:hover { background:#203346; }
        QSplitter::handle { background:#1d2b39; width:2px; }
        QProgressBar { background:#101923; border:1px solid #243343; border-radius:4px; }
        QProgressBar::chunk { background:#19c2c9; }
    )"));
}

void MainWindow::chooseFile() {
    QSettings settings;
    const QString start = settings.value(QStringLiteral("lastDirectory"), QDir::homePath()).toString();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开 MP4 / H.264 / H.265 / MPEG-4 / AV1 视频"), start,
        QStringLiteral("支持的视频 (*.mp4 *.mov *.h264 *.264 *.avc *.h265 *.265 *.hevc *.m4v *.mp4v *.obu *.av1 *.ivf);;MP4 / MOV (*.mp4 *.mov);;视频裸码流 (*.h264 *.264 *.avc *.h265 *.265 *.hevc *.m4v *.mp4v *.obu *.av1 *.ivf);;所有文件 (*.*)"));
    if (!path.isEmpty()) openFile(path);
}

void MainWindow::openFile(const QString &path) {
    if (m_watcher.isRunning()) {
        m_status->setText(QStringLiteral("当前文件仍在解析，请稍候"));
        return;
    }
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        QMessageBox::warning(this, QStringLiteral("无法打开"), QStringLiteral("文件不存在：\n%1").arg(path));
        return;
    }
    m_lastPath = info.absoluteFilePath();
    QSettings().setValue(QStringLiteral("lastDirectory"), info.absolutePath());
    m_fileLabel->setText(QStringLiteral("%1  ·  %2").arg(info.fileName(), formatBytes(info.size())));
    m_fileLabel->setToolTip(m_lastPath);
    m_status->setText(QStringLiteral("正在后台解析码流单元、帧头和参考关系…"));
    m_progress->setRange(0, 0);
    m_progress->show();
    m_model->setStream({});
    if (m_mode->currentIndex() != 0) m_mode->setCurrentIndex(0);
    while (m_mode->count() > 2) m_mode->removeItem(2);
    m_leftStack->setCurrentWidget(m_table);
    m_fileTree->clear();
    m_previews->setStream({});
    m_hevcCuProvider->setStream({});
    m_av1BlockProvider->setStream({});
    m_timeline->setStream({});
    m_macroView->setStream({});
    m_currentFrame = -1;
    clearDetails(QStringLiteral("正在分析码流…"));
    m_watcher.setFuture(QtConcurrent::run([path = m_lastPath] { return BitstreamParser::parseFile(path); }));
}

void MainWindow::parseFinished() {
    m_progress->hide();
    m_stream = m_watcher.result();
    if (!m_stream || !m_stream->error.isEmpty()) {
        const QString error = m_stream ? m_stream->error : QStringLiteral("未知错误");
        m_status->setText(QStringLiteral("解析失败：%1").arg(error));
        QMessageBox::warning(this, QStringLiteral("解析失败"), error);
        return;
    }
    m_model->setStream(m_stream);
    m_previews->setStream(m_stream);
    m_hevcCuProvider->setStream(m_stream);
    m_av1BlockProvider->setStream(m_stream);
    m_timeline->setStream(m_stream);
    m_macroView->setStream(m_stream);
    const bool mpeg4 = m_stream->codec == Codec::Mpeg4Visual;
    const bool av1 = m_stream->codec == Codec::Av1;
    if (auto *qpOverlay = findChild<QCheckBox *>(QStringLiteral("qpOverlay"))) {
        qpOverlay->setEnabled(true);
        qpOverlay->setText(av1 ? QStringLiteral("QIndex 热力图")
                               : QStringLiteral("QP 热力图"));
        qpOverlay->setToolTip(av1
            ? QStringLiteral("按 libaom inspection 导出的真实块级 QIndex 着色：蓝色较低，红色较高")
            : QStringLiteral("按实际解码 QP 着色：蓝色较低，红色较高"));
    }
    m_tabs->setTabText(0, av1 ? QStringLiteral("画面 / Superblock") : QStringLiteral("画面 / 宏块"));
    m_mode->setItemText(1, mpeg4 ? QStringLiteral("起始码单元视图") : (av1 ? QStringLiteral("OBU 视图") : QStringLiteral("NAL 视图")));
    m_tabs->setTabText(m_tabs->indexOf(m_slices), mpeg4 ? QStringLiteral("VOP") : (av1 ? QStringLiteral("Frame Header") : QStringLiteral("Slice")));
    m_tabs->setTabText(m_tabs->indexOf(m_cuTree), av1 ? QStringLiteral("Superblock / Block") : QStringLiteral("CTU / CU"));
    m_cuTree->setHeaderLabels(av1
        ? QStringList{QStringLiteral("Superblock / Coding Block"), QStringLiteral("位置"),
            QStringLiteral("块尺寸"), QStringLiteral("尺寸层级"), QStringLiteral("预测模式"),
            QStringLiteral("色度 / 变换"), QStringLiteral("QIndex"), QStringLiteral("参考 / MV")}
        : QStringList{QStringLiteral("CTU / CU"), QStringLiteral("位置"),
            QStringLiteral("编码尺寸"), QStringLiteral("深度"), QStringLiteral("分区"),
            QStringLiteral("预测"), QStringLiteral("QP"), QStringLiteral("近似字节")});
    m_tabs->setTabText(m_tabs->indexOf(m_syntax), av1 ? QStringLiteral("语法树 / Metadata") : QStringLiteral("语法树 / SEI"));
    m_slices->setHeaderLabels(mpeg4
        ? QStringList{QStringLiteral("VOP"), QStringLiteral("类型"), QStringLiteral("起始宏块"), QStringLiteral("覆盖范围"), QStringLiteral("量化值"), QStringLiteral("实际平均 QP")}
        : (av1
            ? QStringList{QStringLiteral("Frame OBU / Header"), QStringLiteral("类型"), QStringLiteral("起始 Superblock"), QStringLiteral("覆盖范围"), QStringLiteral("Base Q Index"), QStringLiteral("块级 QIndex")}
            : QStringList{QStringLiteral("Slice"), QStringLiteral("类型"), QStringLiteral("起始地址"), QStringLiteral("覆盖范围"), QStringLiteral("PPS"), QStringLiteral("实际平均 QP")}));
    if (m_stream->containerFile) {
        m_mode->addItem(QStringLiteral("文件视图"));
        populateFileView();
    }
    updateMetrics();
    m_status->setText(QStringLiteral("解析完成 · %1 帧 · %2 个%3%4 · 预览将按可见范围加载")
        .arg(m_stream->frames.size()).arg(m_stream->nals.size())
        .arg(mpeg4 ? QStringLiteral("起始码单元") : (av1 ? QStringLiteral(" OBU") : QStringLiteral(" NAL")))
        .arg(m_stream->containerFile ? QStringLiteral(" · %1 个顶层 MP4 Box").arg(m_stream->boxes.size()) : QString()));
    if (m_preferFileView && m_stream->containerFile) m_mode->setCurrentIndex(2);
    else if (!m_stream->frames.isEmpty()) selectFrame(0);
    QTimer::singleShot(120, this, &MainWindow::updateVisiblePreviews);
}

void MainWindow::setViewMode(int index) {
    if (index == 2 && m_stream && m_stream->containerFile) {
        m_leftStack->setCurrentWidget(m_fileTree);
        m_typeFilter->clear();
        m_typeFilter->addItem(QStringLiteral("全部 Box"), QString());
        m_typeFilter->setEnabled(false);
        m_search->setEnabled(false);
        m_search->setPlaceholderText(QStringLiteral("MP4 文件视图按 Box 层级显示"));
        if (!m_fileTree->currentItem() && m_fileTree->topLevelItemCount() > 0)
            m_fileTree->setCurrentItem(m_fileTree->topLevelItem(0));
        return;
    }
    m_leftStack->setCurrentWidget(m_table);
    m_typeFilter->setEnabled(true);
    m_search->setEnabled(true);
    m_search->setPlaceholderText(m_stream && m_stream->codec == Codec::Mpeg4Visual
        ? QStringLiteral("搜索 VOP、起始码、偏移…  ( / )")
        : (m_stream && m_stream->codec == Codec::Av1
            ? QStringLiteral("搜索帧类型、OBU、偏移…  ( / )")
            : QStringLiteral("搜索帧号、类型、偏移、NAL…  ( / )")));
    m_model->setMode(index == 0 ? FrameTableModel::Mode::Frames : FrameTableModel::Mode::Nals);
    m_typeFilter->clear();
    m_typeFilter->addItem(QStringLiteral("全部类型"), QString());
    if (index == 0) {
        m_typeFilter->addItem(m_stream && m_stream->codec == Codec::Mpeg4Visual
            ? QStringLiteral("I-VOP 随机访问") : (m_stream && m_stream->codec == Codec::Av1
                ? QStringLiteral("KEY 随机访问") : QStringLiteral("IDR / IRAP")), QStringLiteral("@random"));
        if (m_stream && m_stream->codec == Codec::Av1) {
            m_typeFilter->addItem(QStringLiteral("KEY"), QStringLiteral("KEY"));
            m_typeFilter->addItem(QStringLiteral("INTER"), QStringLiteral("INTER"));
            m_typeFilter->addItem(QStringLiteral("INTRA_ONLY"), QStringLiteral("INTRA_ONLY"));
            m_typeFilter->addItem(QStringLiteral("SWITCH"), QStringLiteral("SWITCH"));
            m_typeFilter->addItem(QStringLiteral("SHOW_EXISTING"), QStringLiteral("SHOW_EXISTING"));
        } else {
            m_typeFilter->addItem(QStringLiteral("I 帧"), QStringLiteral("I"));
            m_typeFilter->addItem(QStringLiteral("P 帧"), QStringLiteral("P"));
            m_typeFilter->addItem(QStringLiteral("B 帧"), QStringLiteral("B"));
        }
        m_table->setColumnWidth(1, 112);
    } else {
        m_typeFilter->addItem(m_stream && m_stream->codec == Codec::Mpeg4Visual
            ? QStringLiteral("VOP") : (m_stream && m_stream->codec == Codec::Av1
                ? QStringLiteral("Frame OBU / Header") : QStringLiteral("Slice")), QStringLiteral("@slice"));
        m_typeFilter->addItem(QStringLiteral("参数集"), QStringLiteral("PARAM"));
        if (m_stream && m_stream->codec == Codec::Av1) {
            m_typeFilter->addItem(QStringLiteral("Metadata"), QStringLiteral("METADATA"));
            m_typeFilter->addItem(QStringLiteral("Tile Group"), QStringLiteral("TILE"));
        } else m_typeFilter->addItem(QStringLiteral("SEI"), QStringLiteral("SEI"));
        m_typeFilter->addItem(QStringLiteral("其他"), QStringLiteral("OTHER"));
    }
    updateFilters();
    if (m_proxy->rowCount() > 0) m_table->selectRow(0);
    updateVisiblePreviews();
}

void MainWindow::populateFileView()
{
    m_fileTree->clear();
    if (!m_stream || !m_stream->containerFile)
        return;
    std::function<void(const QVector<Mp4BoxInfo> &, QTreeWidgetItem *)> appendBoxes;
    appendBoxes = [this, &appendBoxes](const QVector<Mp4BoxInfo> &boxes, QTreeWidgetItem *parent) {
        for (const auto &box : boxes) {
            auto *item = parent
                ? new QTreeWidgetItem(parent, {box.type,
                      QStringLiteral("0x%1").arg(box.offset, 8, 16, QChar('0')).toUpper(),
                      formatBytes(box.size), box.description})
                : new QTreeWidgetItem(m_fileTree, {box.type,
                      QStringLiteral("0x%1").arg(box.offset, 8, 16, QChar('0')).toUpper(),
                      formatBytes(box.size), box.description});
            item->setData(0, Qt::UserRole, box.offset);
            item->setData(0, Qt::UserRole + 1, box.size);
            QStringList details;
            for (const auto &value : box.fields) details << QStringLiteral("%1 = %2").arg(value.name, value.value);
            if (!details.isEmpty()) item->setToolTip(0, details.join(QLatin1Char('\n')));
            if (!box.children.isEmpty()) appendBoxes(box.children, item);
            if (QSet<QString>{QStringLiteral("moov"), QStringLiteral("trak"), QStringLiteral("mdia"),
                              QStringLiteral("minf"), QStringLiteral("stbl"), QStringLiteral("stsd"),
                              QStringLiteral("avc1"), QStringLiteral("avc3"), QStringLiteral("hvc1"),
                              QStringLiteral("hev1"), QStringLiteral("mp4v"), QStringLiteral("esds"),
                              QStringLiteral("av01"), QStringLiteral("av1C")}.contains(box.type))
                item->setExpanded(true);
        }
    };
    appendBoxes(m_stream->boxes, nullptr);
}

void MainWindow::showFileBox(QTreeWidgetItem *item)
{
    if (!item || !m_stream || !m_stream->containerFile)
        return;
    const qint64 offset = item->data(0, Qt::UserRole).toLongLong();
    const qint64 size = item->data(0, Qt::UserRole + 1).toLongLong();
    const Mp4BoxInfo *selected = nullptr;
    std::function<void(const QVector<Mp4BoxInfo> &)> findBox;
    findBox = [&selected, offset, &findBox](const QVector<Mp4BoxInfo> &boxes) {
        for (const auto &box : boxes) {
            if (box.offset == offset) { selected = &box; return; }
            if (!box.children.isEmpty()) findBox(box.children);
            if (selected) return;
        }
    };
    findBox(m_stream->boxes);
    if (!selected)
        return;
    m_currentFrame = -1;
    m_syntax->clear();
    auto *root = new QTreeWidgetItem(m_syntax, {
        QStringLiteral("Box %1").arg(selected->type), QString::number(selected->size),
        QStringLiteral("ISO BMFF"), selected->description});
    root->setExpanded(true);
    new QTreeWidgetItem(root, {QStringLiteral("offset"),
        QStringLiteral("0x%1").arg(selected->offset, 8, 16, QChar('0')).toUpper(),
        QStringLiteral("bytes"), QStringLiteral("Box 起始位置")});
    new QTreeWidgetItem(root, {QStringLiteral("size"), QString::number(selected->size),
        QStringLiteral("bytes"), formatBytes(selected->size)});
    new QTreeWidgetItem(root, {QStringLiteral("header_size"), QString::number(selected->headerSize),
        QStringLiteral("bytes"), QStringLiteral("Box 头长度")});
    for (const auto &value : selected->fields) addField(root, value);
    const qint64 visibleSize = std::min<qint64>(size, 64 * 1024);
    m_hex->setPlainText(hexDump(m_stream->bytes.mid(int(offset), int(visibleSize)), offset));
    QString fieldsHtml;
    for (const auto &value : selected->fields)
        fieldsHtml += QStringLiteral("<tr><td>%1</td><td><b>%2</b></td><td>%3</td></tr>")
            .arg(value.name.toHtmlEscaped(), value.value.toHtmlEscaped(), value.bits.toHtmlEscaped());
    m_summary->setHtml(QStringLiteral("<h2>%1 Box</h2><p>%2</p>"
        "<p>偏移：0x%3<br>大小：%4<br>头长度：%5 bytes<br>子 Box：%6</p>%7")
        .arg(selected->type.toHtmlEscaped(), selected->description.toHtmlEscaped())
        .arg(selected->offset, 0, 16).arg(formatBytes(selected->size)).arg(selected->headerSize)
        .arg(selected->children.size())
        .arg(fieldsHtml.isEmpty() ? QString() : QStringLiteral("<table cellspacing='6'>%1</table>").arg(fieldsHtml)));
    m_references->setHtml(QStringLiteral("<h3>%1</h3><p>%2</p><p>文件范围：0x%3–0x%4</p>")
        .arg(selected->type.toHtmlEscaped(), selected->description.toHtmlEscaped())
        .arg(selected->offset, 0, 16).arg(selected->offset + selected->size - 1, 0, 16));
    m_frameStats->setText(QStringLiteral("文件视图 · %1 Box · %2").arg(selected->type, formatBytes(selected->size)));
    m_tabs->setCurrentWidget(m_syntax);
}

void MainWindow::updateFilters() {
    auto *proxy = static_cast<StreamFilterProxy *>(m_proxy);
    proxy->setNeedle(m_search->text().trimmed());
    proxy->setType(m_typeFilter->currentData().toString());
    if (m_proxy->rowCount() > 0 && !m_table->currentIndex().isValid()) m_table->selectRow(0);
    updateVisiblePreviews();
}

void MainWindow::selectionChanged(const QModelIndex &current) {
    if (!current.isValid() || !m_stream) return;
    const int item = current.data(FrameTableModel::ItemIndexRole).toInt();
    if (m_model->mode() == FrameTableModel::Mode::Frames) showFrame(item);
    else showNal(item);
}

void MainWindow::selectFrame(int frameIndex) {
    if (!m_stream || frameIndex < 0 || frameIndex >= m_stream->frames.size()) return;
    if (m_mode->currentIndex() != 0 || m_model->mode() != FrameTableModel::Mode::Frames) m_mode->setCurrentIndex(0);
    const QModelIndex source = m_model->index(frameIndex, 0);
    const QModelIndex proxy = m_proxy->mapFromSource(source);
    if (proxy.isValid()) {
        m_table->setCurrentIndex(proxy);
        m_table->scrollTo(proxy, QAbstractItemView::PositionAtCenter);
        m_table->selectRow(proxy.row());
    } else {
        m_typeFilter->setCurrentIndex(0);
        m_search->clear();
        selectFrame(frameIndex);
    }
}

void MainWindow::showFrame(int frameIndex) {
    if (!m_stream || frameIndex < 0 || frameIndex >= m_stream->frames.size()) return;
    const FrameInfo &frame = m_stream->frames.at(frameIndex);
    m_currentFrame = frameIndex;
    m_macroView->setFrame(frameIndex);
    const QImage cached = m_model->thumbnail(frameIndex);
    if (!cached.isNull()) m_macroView->setFrameImage(frameIndex, cached);
    if (cached.isNull()) m_previews->requestFrame(frameIndex);
    else requestCurrentBlockData(frameIndex);
    if (!cached.isNull() && m_previews->isIdle()) requestVisibleBlockData();
    m_timeline->setSelectedFrame(frameIndex);
    updateCuTree(frameIndex);

    m_slices->clear();
    for (int i = 0; i < frame.sliceNalIndices.size(); ++i) {
        const NalUnit &nal = m_stream->nals.at(frame.sliceNalIndices.at(i));
        const SliceInfo &slice = nal.slice;
        const auto region = std::find_if(frame.regions.begin(), frame.regions.end(), [i](const CodingUnitRegion &r) { return r.sliceIndex == i; });
        const QString start = m_stream->codec == Codec::Hevc ? QString::number(slice.address) : QString::number(slice.firstMb);
        const QString range = region == frame.regions.end() ? QStringLiteral("—") : QStringLiteral("%1–%2").arg(region->start).arg(std::max(region->start, region->end - 1));
        const bool qpReady = m_stream->codec == Codec::Hevc
            ? frame.hevcCodingUnitsReady
            : (m_stream->codec == Codec::Av1 ? frame.av1CodingBlocksReady
                                             : frame.decodedUnitsReady);
        const QString qp = qpReady ? QStringLiteral("计算中") : QStringLiteral("—");
        const QString identifier = m_stream->codec == Codec::Mpeg4Visual
            ? QStringLiteral("VOP / 单元 %1").arg(nal.index)
            : (m_stream->codec == Codec::Av1
                ? QStringLiteral("%1 / OBU %2").arg(nal.name).arg(nal.index)
                : QStringLiteral("#%1 / NAL %2").arg(i).arg(nal.index));
        const QString parameter = m_stream->codec == Codec::Mpeg4Visual
            ? (slice.quantizer >= 0 ? QString::number(slice.quantizer) : QStringLiteral("—"))
            : (m_stream->codec == Codec::Av1
                ? (slice.quantizer >= 0 ? QString::number(slice.quantizer) : QStringLiteral("—"))
                : QString::number(slice.ppsId));
        auto *item = new QTreeWidgetItem(m_slices, {identifier, slice.type, start, range, parameter, qp});
        if (i == 0 && frame.truncatedStart) item->setForeground(2, QColor(QStringLiteral("#ff6b6b")));
    }
    for (int column = 0; column < m_slices->columnCount(); ++column) m_slices->resizeColumnToContents(column);

    m_syntax->clear();
    auto *frameRoot = new QTreeWidgetItem(m_syntax, {QStringLiteral("Frame %1").arg(frame.index), frame.displayType, QString(), frame.structure});
    frameRoot->setExpanded(true);
    new QTreeWidgetItem(frameRoot, {QStringLiteral("offset"), QStringLiteral("0x%1").arg(frame.offset, 8, 16, QChar('0')).toUpper(), QStringLiteral("bytes"), QStringLiteral("访问单元起始偏移")});
    new QTreeWidgetItem(frameRoot, {QStringLiteral("size"), QString::number(frame.size), QStringLiteral("bytes"), formatBytes(frame.size)});
    new QTreeWidgetItem(frameRoot, {QStringLiteral("pts"), timeText(frame.pts), QStringLiteral("derived"), QStringLiteral("由容器时间表或码流帧率推算")});
    new QTreeWidgetItem(frameRoot, {m_stream->codec == Codec::Av1 ? QStringLiteral("order_hint") : QStringLiteral("poc"), frame.poc >= 0 ? QString::number(frame.poc) : QStringLiteral("—"), m_stream->codec == Codec::Av1 ? QStringLiteral("frame header") : QStringLiteral("slice"), m_stream->codec == Codec::Av1 ? QStringLiteral("AV1 Order Hint") : QStringLiteral("Picture Order Count")});
    for (int nalIndex : frame.nalIndices) {
        const NalUnit &nal = m_stream->nals.at(nalIndex);
        const QString unitTitle = m_stream->codec == Codec::Mpeg4Visual
            ? QStringLiteral("单元 %1 · %2").arg(nal.index).arg(nal.name)
            : (m_stream->codec == Codec::Av1
                ? QStringLiteral("OBU %1 · %2").arg(nal.index).arg(nal.name)
                : QStringLiteral("NAL %1 · %2").arg(nal.index).arg(nal.name));
        auto *nalRoot = new QTreeWidgetItem(frameRoot, {unitTitle, QString::number(nal.nalType), nal.category, QStringLiteral("0x%1 · %2").arg(nal.offset, 0, 16).arg(formatBytes(nal.size))});
        for (const auto &syntaxField : nal.fields) addField(nalRoot, syntaxField);
        for (const SeiMessage &sei : nal.sei) {
            auto *seiRoot = new QTreeWidgetItem(nalRoot, {QStringLiteral("SEI %1 · %2").arg(sei.type).arg(sei.name), QString::number(sei.payloadSize), QStringLiteral("payload"), QStringLiteral("SEI 详细字段")});
            seiRoot->setForeground(0, QColor(QStringLiteral("#f4a261")));
            for (const auto &syntaxField : sei.fields) addField(seiRoot, syntaxField);
            seiRoot->setExpanded(true);
        }
        if (nal.hasSlice) nalRoot->setExpanded(true);
    }

    QString refText = QStringLiteral("<h3>Frame %1 · %2</h3>").arg(frame.index).arg(frame.displayType.toHtmlEscaped());
    refText += QStringLiteral("<p><b>解码依赖（L0/L1 推断）</b><br>%1</p>")
        .arg(frame.references.isEmpty() ? (frame.index == 0 && !frame.randomAccess ? QStringLiteral("文件外参考帧或参考不可用") : QStringLiteral("无"))
                                      : QStringLiteral("Frame %1").arg(QStringList([&] { QStringList v; for (int n : frame.references) v << QString::number(n); return v; }()).join(QStringLiteral(", Frame "))));
    refText += QStringLiteral("<p><b>被以下帧引用</b><br>%1</p>")
        .arg(frame.referencedBy.isEmpty() ? QStringLiteral("无") : QStringLiteral("Frame %1").arg(QStringList([&] { QStringList v; for (int n : frame.referencedBy) v << QString::number(n); return v; }()).join(QStringLiteral(", Frame "))));
    refText += QStringLiteral("<p style='color:#8191a5'>时间线中，选中帧与参考帧之间会显示弧线。当前参考关系来自 Slice、VOP 或 AV1 reference slot / refresh flags。</p>");
    m_references->setHtml(refText);

    QByteArray bytes;
    for (int nalIndex : frame.nalIndices) {
        if (m_stream->codec != Codec::Av1) bytes.append("\0\0\0\1", 4);
        bytes.append(m_stream->nals.at(nalIndex).payload);
    }
    m_hex->setPlainText(hexDump(bytes, frame.offset));

    int seiCount = 0;
    QStringList seiNames;
    for (int nalIndex : frame.nalIndices) {
        const auto &unit = m_stream->nals.at(nalIndex);
        for (const auto &sei : unit.sei) { ++seiCount; seiNames << sei.name; }
        if (m_stream->codec == Codec::Av1 && unit.nalType == 5) { ++seiCount; seiNames << unit.name; }
    }
    QString diagnostic;
    if (frame.truncatedStart) diagnostic = QStringLiteral("<p style='color:#ff7b72'><b>警告：</b>首个 Slice 不是从地址 0 开始，帧前部数据缺失，解码画面可能不完整。</p>");
    else if (!frame.randomAccess && frame.references.isEmpty() && frame.displayType.contains(QChar('P'))) diagnostic = QStringLiteral("<p style='color:#f4a261'><b>提示：</b>这是预测帧，但文件内没有可用前向参考。若从这里开始送入解码器，画面会在随机访问点前不完整。</p>");
    else diagnostic = m_stream->codec == Codec::Mpeg4Visual
        ? QStringLiteral("<p style='color:#42d392'>当前 VOP 头与宏块覆盖范围正常。</p>")
        : (m_stream->codec == Codec::Av1
            ? QStringLiteral("<p style='color:#42d392'>当前 AV1 Frame Header / OBU 结构可解析。</p>")
            : QStringLiteral("<p style='color:#42d392'>当前帧结构未发现明显的起始 Slice 缺失。</p>"));
    QString unitDescription;
    if (m_stream->codec == Codec::Avc || m_stream->codec == Codec::Mpeg4Visual) {
        unitDescription = QStringLiteral("宏块网格：%1 × %2，标准单元 16×16，边缘按可见像素裁剪")
            .arg(frame.unitColumns).arg(frame.unitRows);
    } else if (m_stream->codec == Codec::Av1) {
        const QString blockState = frame.av1CodingBlocksReady
            ? QStringLiteral("已由 libaom inspection 导出 <b>%1</b> 个真实 Coding Block，包含非方形块、预测模式、变换、QIndex、参考帧和运动矢量")
                  .arg(frame.av1CodingBlocks.size())
            : (frame.av1CodingBlocksError.isEmpty()
                ? QStringLiteral("正在按需解析内部 Coding Block…")
                : frame.av1CodingBlocksError.toHtmlEscaped());
        unitDescription = QStringLiteral("Superblock 根网格：%1 × %2，根单元 %3×%3；内部块最小可到 4×4<br>"
                                         "<span style='color:#8191a5'>%4</span>")
            .arg(frame.unitColumns).arg(frame.unitRows).arg(frame.unitSize).arg(blockState);
    } else {
        const ParameterSetInfo sps = m_stream->sps.value(m_stream->primarySpsId);
        const QString cuState = frame.hevcCodingUnitsReady
            ? QStringLiteral("已解析 <b>%1</b> 个叶子 CU，画面中亮色线为实际 CU 边界")
                  .arg(frame.hevcCodingUnits.size())
            : (frame.hevcCodingUnitsError.isEmpty()
                ? QStringLiteral("正在按需解析 CTU 内部 CU 四叉树…")
                : frame.hevcCodingUnitsError.toHtmlEscaped());
        unitDescription = QStringLiteral("CTU 根网格：%1 × %2，最大 CTU %3×%3；SPS 允许的 CU 边长 %4–%3<br>"
                                         "<span style='color:#8191a5'>%5</span>")
            .arg(frame.unitColumns).arg(frame.unitRows).arg(frame.unitSize)
            .arg(sps.minCodingBlockSize).arg(cuState);
    }
    const QString pictureUnitName = m_stream->codec == Codec::Mpeg4Visual ? QStringLiteral("VOP") : (m_stream->codec == Codec::Av1 ? QStringLiteral("Frame Header / OBU") : QStringLiteral("Slice"));
    const QString streamUnitName = m_stream->codec == Codec::Mpeg4Visual ? QStringLiteral("起始码单元") : (m_stream->codec == Codec::Av1 ? QStringLiteral("OBU") : QStringLiteral("NAL"));
    const QString metadataName = m_stream->codec == Codec::Av1 ? QStringLiteral("Metadata OBU") : QStringLiteral("SEI");
    m_summary->setHtml(QStringLiteral("<h2>Frame %1</h2><p><b>%2</b> · %3 · PTS %4 · %5</p>%6<p>%7：%8<br>%9：%10<br>SEI：%11 %12<br>%13</p>")
        .arg(frame.index).arg(frame.displayType.toHtmlEscaped(), frame.structure.toHtmlEscaped(), timeText(frame.pts), formatBytes(frame.size), diagnostic)
        .arg(pictureUnitName).arg(frame.sliceNalIndices.size()).arg(streamUnitName).arg(frame.nalIndices.size()).arg(seiCount)
        .arg(seiNames.isEmpty() ? QString() : QStringLiteral("(%1)").arg(seiNames.join(QStringLiteral(", ")).toHtmlEscaped()))
        .arg(unitDescription).replace(QStringLiteral("SEI："), metadataName + QStringLiteral("：")));
    updateFrameStatistics(frameIndex);
}

void MainWindow::showNal(int nalIndex) {
    if (!m_stream || nalIndex < 0 || nalIndex >= m_stream->nals.size()) return;
    const NalUnit &nal = m_stream->nals.at(nalIndex);
    int owner = -1;
    for (const FrameInfo &frame : m_stream->frames) if (frame.nalIndices.contains(nalIndex)) { owner = frame.index; break; }
    if (owner >= 0) {
        m_currentFrame = owner;
        m_macroView->setFrame(owner);
        const QImage cached = m_model->thumbnail(owner);
        if (!cached.isNull()) m_macroView->setFrameImage(owner, cached);
        if (cached.isNull()) m_previews->requestFrame(owner);
        m_timeline->setSelectedFrame(owner);
        updateFrameStatistics(owner);
    }
    m_slices->clear();
    if (nal.hasSlice) {
        const SliceInfo &slice = nal.slice;
        new QTreeWidgetItem(m_slices, {m_stream->codec == Codec::Mpeg4Visual ? QStringLiteral("VOP %1").arg(nal.index) : (m_stream->codec == Codec::Av1 ? QStringLiteral("OBU %1").arg(nal.index) : QStringLiteral("NAL %1").arg(nal.index)), slice.type,
            QString::number(m_stream->codec == Codec::Hevc ? slice.address : slice.firstMb), m_stream->codec == Codec::Mpeg4Visual ? QStringLiteral("当前 VOP") : (m_stream->codec == Codec::Av1 ? QStringLiteral("当前 Frame OBU") : QStringLiteral("当前 Slice")), (m_stream->codec == Codec::Mpeg4Visual || m_stream->codec == Codec::Av1) ? (slice.quantizer >= 0 ? QString::number(slice.quantizer) : QStringLiteral("—")) : QString::number(slice.ppsId), QStringLiteral("—")});
    }
    m_syntax->clear();
    const QString unitName = m_stream->codec == Codec::Mpeg4Visual ? QStringLiteral("起始码单元") : (m_stream->codec == Codec::Av1 ? QStringLiteral("OBU") : QStringLiteral("NAL"));
    const QString typeValue = m_stream->codec == Codec::Mpeg4Visual
        ? QStringLiteral("0x%1").arg(nal.nalType, 2, 16, QChar('0')).toUpper() : QString::number(nal.nalType);
    auto *root = new QTreeWidgetItem(m_syntax, {QStringLiteral("%1 %2 · %3").arg(unitName).arg(nal.index).arg(nal.name), typeValue, nal.category, QStringLiteral("%1 bytes").arg(nal.size)});
    root->setExpanded(true);
    new QTreeWidgetItem(root, {QStringLiteral("offset"), QStringLiteral("0x%1").arg(nal.offset, 8, 16, QChar('0')).toUpper(), QStringLiteral("bytes"), QStringLiteral("文件偏移")});
    if (m_stream->codec == Codec::Avc)
        new QTreeWidgetItem(root, {QStringLiteral("nal_ref_idc"), QString::number(nal.refIdc), QStringLiteral("header"), QStringLiteral("H.264 参考属性")});
    if (m_stream->codec == Codec::Hevc)
        new QTreeWidgetItem(root, {QStringLiteral("temporal_id"), nal.temporalId >= 0 ? QString::number(nal.temporalId) : QStringLiteral("—"), QStringLiteral("header"), QStringLiteral("H.265 时间层")});
    if (m_stream->codec == Codec::Av1)
        new QTreeWidgetItem(root, {QStringLiteral("temporal_id"), nal.temporalId >= 0 ? QString::number(nal.temporalId) : QStringLiteral("0"), QStringLiteral("OBU extension"), QStringLiteral("AV1 时间层")});
    for (const auto &syntaxField : nal.fields) addField(root, syntaxField);
    for (const SeiMessage &sei : nal.sei) {
        auto *seiRoot = new QTreeWidgetItem(root, {QStringLiteral("SEI %1 · %2").arg(sei.type).arg(sei.name), QString::number(sei.payloadSize), QStringLiteral("payload"), QStringLiteral("SEI payload")});
        seiRoot->setForeground(0, QColor(QStringLiteral("#f4a261")));
        for (const auto &syntaxField : sei.fields) addField(seiRoot, syntaxField);
        seiRoot->setExpanded(true);
    }
    m_hex->setPlainText(hexDump(nal.payload, nal.offset + nal.startCodeSize));
    m_references->setHtml(QStringLiteral("<h3>%1 %2</h3><p>所属访问单元：%3</p><p>类型：%4 (%5)<br>分类：%6<br>偏移：0x%7<br>大小：%8</p>")
        .arg(unitName)
        .arg(nal.index).arg(owner >= 0 ? QStringLiteral("Frame %1").arg(owner) : QStringLiteral("尚未归入视频帧"))
        .arg(nal.name.toHtmlEscaped()).arg(typeValue).arg(nal.category).arg(nal.offset, 0, 16).arg(formatBytes(nal.size)));
    QString seiText;
    for (const SeiMessage &sei : nal.sei) seiText += QStringLiteral("<li>%1 (%2) · %3 bytes%4</li>").arg(sei.name.toHtmlEscaped()).arg(sei.type).arg(sei.payloadSize).arg(sei.recoveryFrameCount >= 0 ? QStringLiteral(" · recovery_frame_cnt=%1").arg(sei.recoveryFrameCount) : QString());
    m_summary->setHtml(QStringLiteral("<h2>%1 %2</h2><p><b>%3</b> · 类型 %4 · %5</p><p>文件偏移 0x%6，大小 %7</p>%8")
        .arg(unitName).arg(nal.index).arg(nal.name.toHtmlEscaped()).arg(typeValue).arg(nal.category).arg(nal.offset, 0, 16).arg(formatBytes(nal.size))
        .arg(seiText.isEmpty() ? (m_stream->codec == Codec::Av1 ? QStringLiteral("<p>OBU 语法字段见语法树。</p>") : QStringLiteral("<p>无 SEI 消息</p>")) : QStringLiteral("<h3>SEI</h3><ul>%1</ul>").arg(seiText)));
}

void MainWindow::updateVisiblePreviews() {
    if (!m_stream || m_mode->currentIndex() != 0 || m_model->mode() != FrameTableModel::Mode::Frames || m_proxy->rowCount() == 0) return;
    int first = m_table->rowAt(0);
    int last = m_table->rowAt(std::max(0, m_table->viewport()->height() - 1));
    if (first < 0) first = 0;
    if (last < 0) last = std::min(m_proxy->rowCount() - 1, first + 10);
    for (int row = first; row <= last; ++row) {
        const QModelIndex index = m_proxy->index(row, 0);
        if (index.isValid()) {
            const int frame = index.data(FrameTableModel::ItemIndexRole).toInt();
            if (m_model->thumbnail(frame).isNull()) m_previews->requestRange(frame, frame);
        }
    }
    if (m_previews->isIdle())
        requestVisibleBlockData();
}

void MainWindow::requestVisibleBlockData()
{
    if (!m_stream || (m_stream->codec != Codec::Hevc && m_stream->codec != Codec::Av1)
        || m_currentFrame < 0 || m_mode->currentIndex() != 0
        || m_model->mode() != FrameTableModel::Mode::Frames || m_proxy->rowCount() == 0)
        return;
    const FrameInfo &current = m_stream->frames.at(m_currentFrame);
    const bool currentComplete = m_stream->codec == Codec::Av1
        ? current.av1CodingBlocksReady || !current.av1CodingBlocksError.isEmpty()
        : current.hevcCodingUnitsReady || !current.hevcCodingUnitsError.isEmpty();
    if (!currentComplete)
        return;

    int first = m_table->rowAt(0);
    int last = m_table->rowAt(std::max(0, m_table->viewport()->height() - 1));
    if (first < 0)
        first = 0;
    if (last < 0)
        last = std::min(m_proxy->rowCount() - 1, first + 10);
    QVector<int> visibleFrames;
    visibleFrames.reserve(last - first + 2);
    for (int row = first; row <= last; ++row) {
        const QModelIndex index = m_proxy->index(row, 0);
        if (!index.isValid())
            continue;
        const int frame = index.data(FrameTableModel::ItemIndexRole).toInt();
        if (!visibleFrames.contains(frame))
            visibleFrames.push_back(frame);
    }

    for (int frame : visibleFrames) {
        if (frame == m_currentFrame)
            continue;
        if (m_stream->codec == Codec::Av1)
            m_av1BlockProvider->requestRange(frame, frame);
        else
            m_hevcCuProvider->requestRange(frame, frame);
    }
}

void MainWindow::requestCurrentBlockData(int frameIndex)
{
    if (!m_stream || frameIndex != m_currentFrame || frameIndex < 0
        || frameIndex >= m_stream->frames.size())
        return;
    const FrameInfo &frame = m_stream->frames.at(frameIndex);
    if (m_stream->codec == Codec::Av1) {
        if (!frame.av1CodingBlocksReady && frame.av1CodingBlocksError.isEmpty())
            m_av1BlockProvider->requestFrame(frameIndex);
    } else if (m_stream->codec == Codec::Hevc) {
        if (!frame.hevcCodingUnitsReady && frame.hevcCodingUnitsError.isEmpty())
            m_hevcCuProvider->requestFrame(frameIndex);
    }
}

void MainWindow::updateFrameStatistics(int frameIndex)
{
    if (!m_stream || !m_frameStats || frameIndex < 0 || frameIndex >= m_stream->frames.size())
        return;
    const FrameInfo &frame = m_stream->frames.at(frameIndex);
    const QString unitName = m_stream->codec == Codec::Hevc
        ? QStringLiteral("CTU 根单元") : (m_stream->codec == Codec::Av1
            ? QStringLiteral("Superblock 根单元") : QStringLiteral("宏块"));
    const int totalUnits = frame.unitColumns * frame.unitRows;
    if (m_stream->codec == Codec::Hevc) {
        if (!frame.hevcCodingUnitsReady) {
            const QString state = frame.hevcCodingUnitsError.isEmpty()
                ? QStringLiteral("正在通过 HEVC 参考解码器解析实际 CU 四叉树…")
                : frame.hevcCodingUnitsError.toHtmlEscaped();
            m_frameStats->setText(
                QStringLiteral("<b>当前帧 F%1</b>　CTU %2（%3 × %4）　最大尺寸 %5×%5　Slice %6<br>"
                               "<span style='color:#8191a5'>%7</span>")
                    .arg(frame.index).arg(totalUnits).arg(frame.unitColumns)
                    .arg(frame.unitRows).arg(frame.unitSize)
                    .arg(frame.sliceNalIndices.size()).arg(state));
            return;
        }

        QMap<QString, int> sizeCounts;
        QMap<int, int> depthCounts;
        QMap<QString, int> predictionCounts;
        QVector<int> qps;
        double qpSum = 0.0;
        int qpMinimum = 999;
        int qpMaximum = -999;
        qint64 approximateBytes = 0;
        for (const auto &unit : frame.hevcCodingUnits) {
            ++sizeCounts[QStringLiteral("%1×%1").arg(unit.codedSize)];
            ++depthCounts[unit.depth];
            ++predictionCounts[unit.prediction];
            if (unit.qp >= 0) {
                qps.push_back(unit.qp);
                qpSum += unit.qp;
                qpMinimum = std::min(qpMinimum, unit.qp);
                qpMaximum = std::max(qpMaximum, unit.qp);
            }
            if (unit.approximateBytes >= 0)
                approximateBytes += unit.approximateBytes;
        }
        QStringList sizes;
        for (auto it = sizeCounts.cbegin(); it != sizeCounts.cend(); ++it)
            sizes << QStringLiteral("%1: %2").arg(it.key()).arg(it.value());
        QStringList depths;
        for (auto it = depthCounts.cbegin(); it != depthCounts.cend(); ++it)
            depths << QStringLiteral("D%1: %2").arg(it.key()).arg(it.value());
        QStringList predictions;
        for (auto it = predictionCounts.cbegin(); it != predictionCounts.cend(); ++it)
            predictions << QStringLiteral("%1 %2").arg(it.key()).arg(it.value());
        const QString qpText = qps.isEmpty()
            ? QStringLiteral("—")
            : QStringLiteral("平均 %1，最小/最大 %2/%3")
                  .arg(qpSum / qps.size(), 0, 'f', 2).arg(qpMinimum).arg(qpMaximum);
        m_frameStats->setText(
            QStringLiteral("<b>当前帧 F%1 · HEVC 实际 CU 拆分</b>　CTU <b>%2</b>　"
                           "叶子 CU <b style='color:#19c2c9'>%3</b>　QP %4　CABAC 近似 %5 bytes<br>"
                           "<span style='color:#8191a5'>尺寸：%6　·　深度：%7　·　%8</span>")
                .arg(frame.index).arg(totalUnits).arg(frame.hevcCodingUnits.size())
                .arg(qpText).arg(approximateBytes)
                .arg(sizes.join(QStringLiteral("　")))
                .arg(depths.join(QStringLiteral("　")))
                .arg(predictions.join(QStringLiteral("　"))));

        for (int sliceIndex = 0; sliceIndex < m_slices->topLevelItemCount(); ++sliceIndex) {
            const auto region = std::find_if(frame.regions.begin(), frame.regions.end(),
                [sliceIndex](const CodingUnitRegion &candidate) {
                    return candidate.sliceIndex == sliceIndex;
                });
            if (region == frame.regions.end())
                continue;
            double sum = 0.0;
            int count = 0;
            int minimum = 999;
            int maximum = -999;
            for (const auto &unit : frame.hevcCodingUnits) {
                if (unit.ctuAddress < region->start || unit.ctuAddress >= region->end || unit.qp < 0)
                    continue;
                sum += unit.qp;
                ++count;
                minimum = std::min(minimum, unit.qp);
                maximum = std::max(maximum, unit.qp);
            }
            m_slices->topLevelItem(sliceIndex)->setText(5, count > 0
                ? QStringLiteral("%1（%2–%3）").arg(sum / count, 0, 'f', 2).arg(minimum).arg(maximum)
                : QStringLiteral("—"));
        }
        m_slices->resizeColumnToContents(5);
        return;
    }
    if (m_stream->codec == Codec::Av1) {
        int refreshFlags = 0;
        bool showExisting = false;
        for (int unitIndex : frame.sliceNalIndices) {
            const auto &header = m_stream->nals.at(unitIndex).slice;
            refreshFlags |= header.refreshFrameFlags;
            showExisting |= header.showExistingFrame;
        }
        if (!frame.av1CodingBlocksReady) {
            const QString state = frame.av1CodingBlocksError.isEmpty()
                ? QStringLiteral("正在通过 libaom inspection 解析真实 Coding Block…")
                : frame.av1CodingBlocksError.toHtmlEscaped();
            m_frameStats->setText(
                QStringLiteral("<b>当前帧 F%1 · AV1</b>　Superblock <b>%2</b>（%3 × %4）　"
                               "根尺寸 %5×%5　Frame Header %6　Order Hint %7<br>"
                               "<span style='color:#8191a5'>refresh_frame_flags=0x%8%9　·　%10</span>")
                    .arg(frame.index).arg(totalUnits).arg(frame.unitColumns).arg(frame.unitRows)
                    .arg(frame.unitSize).arg(frame.sliceNalIndices.size())
                    .arg(frame.poc >= 0 ? QString::number(frame.poc) : QStringLiteral("—"))
                    .arg(refreshFlags, 2, 16, QChar('0'))
                    .arg(showExisting ? QStringLiteral("　·　show_existing_frame") : QString())
                    .arg(state));
            for (int i = 0; i < m_slices->topLevelItemCount(); ++i)
                m_slices->topLevelItem(i)->setText(5, QStringLiteral("加载中"));
            return;
        }

        QMap<QString, int> sizeCounts;
        QMap<QString, int> modeCounts;
        double qSum = 0.0;
        int qCount = 0;
        int qMinimum = 999;
        int qMaximum = -999;
        int skipped = 0;
        for (const auto &block : frame.av1CodingBlocks) {
            ++sizeCounts[block.blockSize];
            ++modeCounts[block.predictionMode];
            skipped += block.skip ? 1 : 0;
            if (block.qIndex >= 0) {
                qSum += block.qIndex;
                ++qCount;
                qMinimum = std::min(qMinimum, block.qIndex);
                qMaximum = std::max(qMaximum, block.qIndex);
            }
        }
        QStringList sizes;
        for (auto it = sizeCounts.cbegin(); it != sizeCounts.cend(); ++it)
            sizes << QStringLiteral("%1: %2").arg(it.key()).arg(it.value());
        QStringList modes;
        for (auto it = modeCounts.cbegin(); it != modeCounts.cend(); ++it)
            modes << QStringLiteral("%1 %2").arg(it.key()).arg(it.value());
        const QString qText = qCount == 0 ? QStringLiteral("—")
            : QStringLiteral("平均 %1，最小/最大 %2/%3")
                  .arg(qSum / qCount, 0, 'f', 2).arg(qMinimum).arg(qMaximum);
        m_frameStats->setText(
            QStringLiteral("<b>当前帧 F%1 · AV1 实际 Coding Block</b>　Superblock <b>%2</b>　"
                           "叶子块 <b style='color:#19c2c9'>%3</b>　QIndex %4　Skip %5<br>"
                           "<span style='color:#8191a5'>尺寸：%6<br>预测：%7</span>")
                .arg(frame.index).arg(totalUnits).arg(frame.av1CodingBlocks.size())
                .arg(qText).arg(skipped)
                .arg(sizes.join(QStringLiteral("　")))
                .arg(modes.join(QStringLiteral("　"))));
        for (int i = 0; i < m_slices->topLevelItemCount(); ++i)
            m_slices->topLevelItem(i)->setText(5, qCount > 0
                ? QStringLiteral("%1（%2–%3）").arg(qSum / qCount, 0, 'f', 2)
                      .arg(qMinimum).arg(qMaximum)
                : QStringLiteral("—"));
        return;
    }
    if (!frame.decodedUnitsReady) {
        const QString state = frame.decodedUnitsError.isEmpty()
            ? QStringLiteral("正在随画面预览加载实际 QP 和类型…")
            : frame.decodedUnitsError.toHtmlEscaped();
        const QString sizeText = QStringLiteral("16×16");
        const QString pictureUnit = m_stream->codec == Codec::Mpeg4Visual
            ? QStringLiteral("VOP") : QStringLiteral("Slice");
        m_frameStats->setText(
            QStringLiteral("<b>当前帧 F%1</b>　%2 %3（%4 × %5）　尺寸 %6　%7 %8<br>"
                           "<span style='color:#8191a5'>%9</span>")
                .arg(frame.index).arg(unitName).arg(totalUnits)
                .arg(frame.unitColumns).arg(frame.unitRows)
                .arg(sizeText).arg(pictureUnit).arg(frame.sliceNalIndices.size()).arg(state));
        for (int i = 0; i < m_slices->topLevelItemCount(); ++i)
            m_slices->topLevelItem(i)->setText(5, QStringLiteral("—"));
        return;
    }

    QVector<int> qps;
    qps.reserve(frame.decodedUnits.size());
    QMap<QString, int> categories;
    double sum = 0.0;
    double squareSum = 0.0;
    int minimum = 52;
    int maximum = -1;
    for (const auto &unit : frame.decodedUnits) {
        if (unit.qp < 0)
            continue;
        qps.push_back(unit.qp);
        sum += unit.qp;
        squareSum += unit.qp * unit.qp;
        minimum = std::min(minimum, unit.qp);
        maximum = std::max(maximum, unit.qp);
        ++categories[unit.category];
    }
    if (qps.isEmpty()) {
        m_frameStats->setText(QStringLiteral("<b>当前帧 F%1</b>　未取得有效的实际 QP 数据").arg(frame.index));
        return;
    }
    std::sort(qps.begin(), qps.end());
    const double average = sum / qps.size();
    const double variance = std::max(0.0, squareSum / qps.size() - average * average);
    const double deviation = std::sqrt(variance);
    const double median = qps.size() % 2
        ? qps.at(qps.size() / 2)
        : (qps.at(qps.size() / 2 - 1) + qps.at(qps.size() / 2)) / 2.0;
    QStringList typeParts;
    const QStringList order{QStringLiteral("帧内预测"), QStringLiteral("前向预测"),
                            QStringLiteral("反向预测"), QStringLiteral("双向/直接"),
                            QStringLiteral("跳过"), QStringLiteral("其他")};
    for (const QString &category : order) {
        const int count = categories.value(category);
        if (count > 0)
            typeParts << QStringLiteral("%1 %2%")
                .arg(category).arg(count * 100.0 / qps.size(), 0, 'f', 1);
    }
    m_frameStats->setText(
        QStringLiteral("<b>当前帧 F%1 · 实际解码统计</b>　%2 <b>%3/%4</b>　"
                       "QP 平均 <b style='color:#19c2c9'>%5</b>　中位数 %6　"
                       "最小/最大 %7/%8　标准差 %9<br>"
                       "<span style='color:#8191a5'>%10　·　%11 %12　·　覆盖率 %13%</span>")
            .arg(frame.index).arg(unitName).arg(qps.size()).arg(totalUnits)
            .arg(average, 0, 'f', 2).arg(median, 0, 'f', 1)
            .arg(minimum).arg(maximum).arg(deviation, 0, 'f', 2)
            .arg(typeParts.join(QStringLiteral("　")))
            .arg(m_stream->codec == Codec::Mpeg4Visual ? QStringLiteral("VOP") : QStringLiteral("Slice"))
            .arg(frame.sliceNalIndices.size())
            .arg(totalUnits > 0 ? qps.size() * 100.0 / totalUnits : 0.0, 0, 'f', 1));

    for (int sliceIndex = 0; sliceIndex < m_slices->topLevelItemCount(); ++sliceIndex) {
        const auto region = std::find_if(frame.regions.begin(), frame.regions.end(),
            [sliceIndex](const CodingUnitRegion &candidate) {
                return candidate.sliceIndex == sliceIndex;
            });
        if (region == frame.regions.end()) {
            m_slices->topLevelItem(sliceIndex)->setText(5, QStringLiteral("—"));
            continue;
        }
        double sliceSum = 0.0;
        int sliceCount = 0;
        int sliceMin = 52;
        int sliceMax = -1;
        for (int address = region->start;
             address < region->end && address < frame.decodedUnits.size(); ++address) {
            const int qp = frame.decodedUnits.at(address).qp;
            if (qp < 0)
                continue;
            sliceSum += qp;
            ++sliceCount;
            sliceMin = std::min(sliceMin, qp);
            sliceMax = std::max(sliceMax, qp);
        }
        const QString text = sliceCount > 0
            ? QStringLiteral("%1（%2–%3）").arg(sliceSum / sliceCount, 0, 'f', 2).arg(sliceMin).arg(sliceMax)
            : QStringLiteral("—");
        m_slices->topLevelItem(sliceIndex)->setText(5, text);
    }
    m_slices->resizeColumnToContents(5);
}

void MainWindow::updateCuTree(int frameIndex)
{
    if (!m_cuTree)
        return;
    m_cuTree->setUpdatesEnabled(false);
    m_cuTree->clear();
    if (!m_stream || frameIndex < 0 || frameIndex >= m_stream->frames.size()) {
        m_cuTree->setUpdatesEnabled(true);
        return;
    }
    const FrameInfo &frame = m_stream->frames.at(frameIndex);
    if (m_stream->codec == Codec::Av1) {
        if (!frame.av1CodingBlocksReady) {
            new QTreeWidgetItem(m_cuTree, {frame.av1CodingBlocksError.isEmpty()
                ? QStringLiteral("正在按需解析 Frame %1 的 AV1 Coding Block…").arg(frame.index)
                : frame.av1CodingBlocksError});
            m_cuTree->setUpdatesEnabled(true);
            return;
        }
        QMap<int, int> blockCounts;
        for (const auto &block : frame.av1CodingBlocks)
            ++blockCounts[block.superblockAddress];
        for (auto count = blockCounts.cbegin(); count != blockCounts.cend(); ++count) {
            const int address = count.key();
            const int column = address % frame.unitColumns;
            const int row = address / frame.unitColumns;
            const int x = column * frame.unitSize;
            const int y = row * frame.unitSize;
            const int width = std::min(frame.unitSize, m_stream->width - x);
            const int height = std::min(frame.unitSize, m_stream->height - y);
            auto *root = new QTreeWidgetItem(m_cuTree, {
                QStringLiteral("Superblock %1（%2 个 Coding Block）").arg(address).arg(count.value()),
                QStringLiteral("(%1, %2)").arg(x).arg(y),
                width == frame.unitSize && height == frame.unitSize
                    ? QStringLiteral("%1×%1").arg(frame.unitSize)
                    : QStringLiteral("%1×%1（可见 %2×%3）")
                          .arg(frame.unitSize).arg(width).arg(height),
                QStringLiteral("0"), QStringLiteral("AV1 Superblock 根")
            });
            root->setData(0, Qt::UserRole, address);
            root->setData(0, Qt::UserRole + 1, frameIndex);
            root->setForeground(0, QColor(QStringLiteral("#19c2c9")));
            auto *placeholder = new QTreeWidgetItem(root, {QStringLiteral("展开加载 Coding Block 详情…")});
            placeholder->setData(0, Qt::UserRole, true);
            placeholder->setForeground(0, QColor(QStringLiteral("#8191a5")));
        }
        for (int column = 0; column < m_cuTree->columnCount(); ++column)
            m_cuTree->resizeColumnToContents(column);
        m_cuTree->setUpdatesEnabled(true);
        return;
    }
    if (m_stream->codec != Codec::Hevc) {
        new QTreeWidgetItem(m_cuTree, {QStringLiteral("%1 使用 16×16 宏块，请在画面页查看")
            .arg(m_stream->codec == Codec::Mpeg4Visual ? QStringLiteral("MPEG-4 Part 2") : QStringLiteral("H.264"))});
        m_cuTree->setUpdatesEnabled(true);
        return;
    }
    if (!frame.hevcCodingUnitsReady) {
        new QTreeWidgetItem(m_cuTree, {frame.hevcCodingUnitsError.isEmpty()
            ? QStringLiteral("正在按需解析 Frame %1 的 CTU/CU 四叉树…").arg(frame.index)
            : frame.hevcCodingUnitsError});
        m_cuTree->setUpdatesEnabled(true);
        return;
    }

    QMap<int, int> leafCounts;
    for (const auto &unit : frame.hevcCodingUnits)
        ++leafCounts[unit.ctuAddress];
    for (auto it = leafCounts.cbegin(); it != leafCounts.cend(); ++it) {
        const int address = it.key();
        const int column = address % frame.unitColumns;
        const int row = address / frame.unitColumns;
        const int x = column * frame.unitSize;
        const int y = row * frame.unitSize;
        auto *root = new QTreeWidgetItem(m_cuTree, {
            QStringLiteral("CTU %1（%2 个叶子 CU）").arg(address).arg(it.value()),
            QStringLiteral("(%1, %2)").arg(x).arg(y),
            QStringLiteral("%1×%1").arg(frame.unitSize),
            QStringLiteral("0"), QStringLiteral("四叉树根")
        });
        root->setData(0, Qt::UserRole, address);
        root->setData(0, Qt::UserRole + 1, frameIndex);
        root->setForeground(0, QColor(QStringLiteral("#19c2c9")));
        auto *placeholder = new QTreeWidgetItem(root, {QStringLiteral("展开加载 CU 详情…")});
        placeholder->setData(0, Qt::UserRole, true);
        placeholder->setForeground(0, QColor(QStringLiteral("#8191a5")));
    }
    for (int column = 0; column < m_cuTree->columnCount(); ++column)
        m_cuTree->resizeColumnToContents(column);
    m_cuTree->setUpdatesEnabled(true);
}

void MainWindow::populateCuTreeChildren(QTreeWidgetItem *root)
{
    if (!root || !m_stream
        || (m_stream->codec != Codec::Hevc && m_stream->codec != Codec::Av1)
        || root->treeWidget() != m_cuTree)
        return;
    const int frameIndex = root->data(0, Qt::UserRole + 1).toInt();
    const int ctuAddress = root->data(0, Qt::UserRole).toInt();
    if (frameIndex != m_currentFrame || frameIndex < 0 || frameIndex >= m_stream->frames.size())
        return;
    if (root->childCount() != 1 || !root->child(0)->data(0, Qt::UserRole).toBool())
        return;

    delete root->takeChild(0);
    const FrameInfo &frame = m_stream->frames.at(frameIndex);
    if (m_stream->codec == Codec::Av1) {
        int blockIndex = 0;
        m_cuTree->setUpdatesEnabled(false);
        for (const auto &block : frame.av1CodingBlocks) {
            if (block.superblockAddress != ctuAddress)
                continue;
            const QString visible = block.width == block.codedWidth
                    && block.height == block.codedHeight
                ? QStringLiteral("%1×%2").arg(block.codedWidth).arg(block.codedHeight)
                : QStringLiteral("%1×%2（可见 %3×%4）")
                      .arg(block.codedWidth).arg(block.codedHeight)
                      .arg(block.width).arg(block.height);
            QString flags;
            if (block.skip)
                flags += QStringLiteral("Skip · ");
            if (block.intraBlockCopy)
                flags += QStringLiteral("IntraBC · ");
            const QString transform = QStringLiteral("%1 · TX %2/%3%4")
                .arg(block.uvPredictionMode, block.transformSize, block.transformType,
                     flags.isEmpty() ? QString() : QStringLiteral(" · ") + flags.chopped(3));
            QString reference = block.referenceFrames;
            if (!reference.isEmpty() && !reference.contains(QStringLiteral("INTRA FRAME"))) {
                reference += QStringLiteral(" · MV0(%1,%2)")
                    .arg(block.motionVector0X).arg(block.motionVector0Y);
                if (block.motionVector1X != 0 || block.motionVector1Y != 0)
                    reference += QStringLiteral(" MV1(%1,%2)")
                        .arg(block.motionVector1X).arg(block.motionVector1Y);
            }
            auto *item = new QTreeWidgetItem(root, {
                QStringLiteral("Coding Block %1").arg(blockIndex++),
                QStringLiteral("(%1, %2)").arg(block.x).arg(block.y),
                visible, QString::number(block.depth), block.predictionMode,
                transform,
                block.qIndex >= 0 ? QString::number(block.qIndex) : QStringLiteral("—"),
                reference.isEmpty() ? QStringLiteral("—") : reference
            });
            item->setForeground(4, block.predictionMode.contains(QStringLiteral("PRED"))
                ? QColor(QStringLiteral("#f4a261")) : QColor(QStringLiteral("#58a6ff")));
        }
        m_cuTree->setUpdatesEnabled(true);
        return;
    }
    const int rootX = (ctuAddress % frame.unitColumns) * frame.unitSize;
    const int rootY = (ctuAddress / frame.unitColumns) * frame.unitSize;
    QHash<QString, QTreeWidgetItem *> branches;
    int leafIndex = 0;
    m_cuTree->setUpdatesEnabled(false);
    for (const auto &unit : frame.hevcCodingUnits) {
        if (unit.ctuAddress != ctuAddress)
            continue;
        QTreeWidgetItem *parent = root;
        QString path;
        for (int level = 1; level < unit.depth; ++level) {
            const int size = std::max(1, frame.unitSize >> level);
            const int x = rootX + ((unit.x - rootX) / size) * size;
            const int y = rootY + ((unit.y - rootY) / size) * size;
            const int quadrantX = ((unit.x - rootX) / size) & 1;
            const int quadrantY = ((unit.y - rootY) / size) & 1;
            path += QStringLiteral("/%1:%2:%3").arg(level).arg(x).arg(y);
            QTreeWidgetItem *branch = branches.value(path, nullptr);
            if (!branch) {
                static const QStringList quadrants{
                    QStringLiteral("左上"), QStringLiteral("右上"),
                    QStringLiteral("左下"), QStringLiteral("右下")
                };
                branch = new QTreeWidgetItem(parent, {
                    QStringLiteral("D%1 分裂块 · %2")
                        .arg(level).arg(quadrants.at(quadrantY * 2 + quadrantX)),
                    QStringLiteral("(%1, %2)").arg(x).arg(y),
                    QStringLiteral("%1×%1").arg(size),
                    QString::number(level), QStringLiteral("继续四叉分裂")
                });
                branch->setForeground(0, QColor(QStringLiteral("#a78bfa")));
                branches.insert(path, branch);
            }
            parent = branch;
        }
        const QString visible = unit.width == unit.codedSize && unit.height == unit.codedSize
            ? QStringLiteral("%1×%1").arg(unit.codedSize)
            : QStringLiteral("%1×%1（可见 %2×%3）")
                  .arg(unit.codedSize).arg(unit.width).arg(unit.height);
        auto *item = new QTreeWidgetItem(parent, {
            QStringLiteral("叶子 CU %1").arg(leafIndex++),
            QStringLiteral("(%1, %2)").arg(unit.x).arg(unit.y),
            visible, QString::number(unit.depth), unit.partitionName,
            unit.prediction,
            unit.qp >= 0 ? QString::number(unit.qp) : QStringLiteral("—"),
            unit.approximateBytes >= 0 ? QString::number(unit.approximateBytes) : QStringLiteral("—")
        });
        item->setForeground(5, unit.prediction == QStringLiteral("帧内预测")
            ? QColor(QStringLiteral("#f4a261")) : QColor(QStringLiteral("#58a6ff")));
    }
    m_cuTree->setUpdatesEnabled(true);
}

void MainWindow::updateMetrics() {
    if (!m_stream) return;
    const QString chroma = m_stream->chroma == 1 ? QStringLiteral("4:2:0") : m_stream->chroma == 2 ? QStringLiteral("4:2:2") : m_stream->chroma == 3 ? QStringLiteral("4:4:4") : QStringLiteral("Mono");
    const QString profileText = (m_stream->codec == Codec::Mpeg4Visual || m_stream->codec == Codec::Av1)
        ? m_stream->profileName.toHtmlEscaped()
        : QStringLiteral("Profile %1 / L%2").arg(m_stream->profile).arg(m_stream->level);
    m_codecMetric->setText(QStringLiteral("<span style='color:#6f8298'>编码</span><br><b>%1</b> · %2").arg(m_stream->codecName(), profileText));
    m_resolutionMetric->setText(QStringLiteral("<span style='color:#6f8298'>分辨率</span><br><b>%1 × %2</b> · %3-bit %4").arg(m_stream->width).arg(m_stream->height).arg(m_stream->bitDepth).arg(chroma));
    m_frameMetric->setText(QStringLiteral("<span style='color:#6f8298'>帧 / 码流单元</span><br><b>%1</b> 帧 · %2 %3")
        .arg(m_stream->frames.size()).arg(m_stream->nals.size())
        .arg(m_stream->codec == Codec::Mpeg4Visual ? QStringLiteral("起始码单元") : (m_stream->codec == Codec::Av1 ? QStringLiteral("OBU") : QStringLiteral("NAL"))));
    m_rateMetric->setText(QStringLiteral("<span style='color:#6f8298'>帧率 / 码率</span><br><b>%1 fps</b> · %2 Mbps").arg(m_stream->fps, 0, 'f', 3).arg(m_stream->bitrate / 1000000.0, 0, 'f', 2));
    m_summary->setHtml(QStringLiteral("<h2>%1</h2><p>%2 · %3 × %4 · %5 fps · 时长 %6</p><p>格式：%7<br>帧：%8<br>码流单元：%9<br>平均码率：%10 Mbps</p>")
        .arg(m_stream->fileName.toHtmlEscaped(), m_stream->codecName()).arg(m_stream->width).arg(m_stream->height).arg(m_stream->fps, 0, 'f', 3)
        .arg(timeText(m_stream->duration), m_stream->format).arg(m_stream->frames.size()).arg(m_stream->nals.size()).arg(m_stream->bitrate / 1000000.0, 0, 'f', 2));
}

void MainWindow::clearDetails(const QString &message) {
    m_currentFrame = -1;
    if (m_frameStats)
        m_frameStats->setText(QStringLiteral("当前帧统计 · %1").arg(message.toHtmlEscaped()));
    m_slices->clear();
    if (m_cuTree)
        m_cuTree->clear();
    m_syntax->clear();
    m_hex->clear();
    m_references->setHtml(QStringLiteral("<p style='color:#8191a5'>%1</p>").arg(message.toHtmlEscaped()));
    m_summary->setHtml(QStringLiteral("<h2>%1</h2><p>支持 MP4 / MOV，H.264 / H.265、MPEG-4 Part 2，以及 AV1 (`av01` / `.obu` / `.av1` / `.ivf`) 码流。</p>").arg(message.toHtmlEscaped()));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty() && event->mimeData()->urls().first().isLocalFile()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event) {
    if (event->mimeData()->hasUrls() && !event->mimeData()->urls().isEmpty()) {
        openFile(event->mimeData()->urls().first().toLocalFile());
        event->acceptProposedAction();
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_watcher.isRunning()) m_status->setText(QStringLiteral("正在完成后台解析…"));
    QMainWindow::closeEvent(event);
}

} // namespace bitscope
