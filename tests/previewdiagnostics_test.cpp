#include "mainwindow.h"
#include "previewprovider.h"
#include "frametablemodel.h"
#include "hevccuprovider.h"
#include "av1blockprovider.h"
#include "platformwindow.h"
#include "avclearning.h"
#include "macroblockview.h"
#include "avcmotionprovider.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QTabWidget>
#include <QElapsedTimer>
#include <QThread>
#include <QMouseEvent>
#include <QScreen>

static void require(bool condition, const char *message)
{
    if (!condition) qFatal("Preview diagnostics: %s", message);
}

namespace bitscope {
struct HevcMotionTest {
    static void run() {
        HevcCuProvider provider;
        auto stream = std::make_shared<StreamInfo>();
        stream->codec = Codec::Hevc; stream->width = stream->height = 64;
        stream->primarySpsId = 0; stream->sps[0].ctbSize = 64;
        provider.setStream(stream);
        provider.m_activeDirectory = provider.m_temp.path();
        const auto write = [&](const char *name, const QByteArray &bytes) {
            QFile f(provider.m_temp.filePath(QString::fromLatin1(name)));
            require(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), "HEVC fixture write");
        };
        write("decoder_cupu.txt", "<7,0> 4\n");
        write("decoder_pred.txt", "<7,0> 1 1\n");
        write("decoder_mv.txt", "<7,0> 1 6 -3 8 2 9 12 -4\n");
        auto result = provider.parseOutput(nullptr);
        require(result.frames.size() == 1 && result.frames[0].size() == 1, "HEVC CU fixture parsed");
        const auto &cu = result.frames[0][0];
        require(cu.motionVectorsReady && cu.motionVectors.size() == 2, "HEVC per-PU directions parsed");
        require(cu.motionVectors[0].height == 16 && cu.motionVectors[1].height == 48
            && cu.motionVectors[1].centerY == 40 && cu.motionVectors[1].list == 1
            && cu.motionVectors[0].referencePoc == 6 && cu.motionVectors[0].dx() == -.75,
            "asymmetric PU geometry, reference POC, list and quarter-pixel precision");
        write("decoder_cupu.txt", "<7,0> 0\n");
        write("decoder_mv.txt", "<7,0> 3 6 0 0 9 1 -1\n");
        result = provider.parseOutput(nullptr);
        require(result.frames[0][0].motionVectors.size() == 2 && result.frames[0][0].motionVectors[1].dy() == -.25,
            "bidirectional HEVC PU preserves zero and fractional vectors");
        write("decoder_mv.txt", "<8,0> 3 6 0 0 9 1 -1\n");
        result = provider.parseOutput(nullptr);
        require(!result.frames[0][0].motionVectorsReady, "wrong POC motion rejected");
        write("decoder_mv.txt", "<7,0> 3 6 0 0 9 1\n");
        result = provider.parseOutput(nullptr);
        require(!result.frames[0][0].motionVectorsReady && result.frames[0][0].motionVectors.isEmpty(), "truncated motion does not leak partial vectors");
        write("decoder_mv.txt", "<7,0> bad 0\n");
        result = provider.parseOutput(nullptr);
        require(!result.frames[0][0].motionVectorsReady, "malformed motion rejected");
        for (int mode = 0; mode < 8; ++mode) {
            const int count = mode == 0 ? 1 : mode == 3 ? 4 : 2;
            write("decoder_cupu.txt", "<7,0> " + QByteArray::number(mode) + "\n");
            write("decoder_mv.txt", "<7,0> " + QByteArray("1 6 0 0 ").repeated(count) + "\n");
            result = provider.parseOutput(nullptr);
            const auto &vectors = result.frames[0][0].motionVectors;
            require(result.frames[0][0].motionVectorsReady && vectors.size() == count, "every HEVC partition parsed");
            int area = 0;
            for (const auto &v : vectors) area += v.width * v.height;
            require(area == 64 * 64, "PU regions cover coded CU");
        }
    }
};
// Offscreen Qt does not provide NSView/HWND handles. Native title-bar behavior
// has its own window-chrome test; this test exercises the real status UI only.
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
void configurePlatformWindow(QWidget *) {}
#endif
#ifdef Q_OS_WIN
bool handlePlatformWindowEvent(QWidget *, void *, qintptr *) { return false; }
#endif

struct PreviewProviderDiagnosticsTest {
    static void run()
    {
        PreviewProvider provider;
        provider.m_activeStart = 10;
        provider.m_activeEnd = 12;
        provider.m_activeRequested = {12};
        provider.m_activeOutputOrder = {10, 11, 12};
        provider.m_activeInput = provider.m_temp.filePath("input with spaces.h265");
        provider.m_activePrefix = provider.m_temp.filePath("preview_");
        provider.m_process.setProgram("/tools with spaces/ffmpeg");
        provider.m_process.setArguments({"-i", provider.m_activeInput});
        const QString debug = QStringLiteral("FFmpeg banner\n")
            + QStringLiteral("macroblock row\n").repeated(2000)
            + QStringLiteral("Error opening output: Permission denied\n0 decoding errors\n"
                             "Input stream #0:0: 25 packets read; 23 frames decoded; 0 decode errors;\n"
                             "muxing overhead: unknown\n");
        const QString reason = provider.failureReason(QStringLiteral("退出码 1"), debug);
        require(reason.section('\n', 0, 0).contains("Permission denied"), "key error precedes debug log");
        require(reason.contains("F10–F12") && reason.contains("/tools with spaces/ffmpeg"), "decode context retained");
        require(reason.size() < 3000 && !reason.contains("FFmpeg banner"), "macroblock logs bounded");

        QString failure;
        int failures = 0, idle = 0;
        QObject::connect(&provider, &PreviewProvider::previewFailed, &provider, [&](int frame, const QString &error) {
            require(frame == 12, "failure frame mapping");
            failure = error;
            ++failures;
        });
        QObject::connect(&provider, &PreviewProvider::queueIdle, &provider, [&] { ++idle; });
        provider.processFinished(0, QProcess::NormalExit);
        require(failures == 1 && failure.contains(QStringLiteral("未生成 F12"))
                && failure.contains("00002.jpg") && failure.contains(QStringLiteral("无 stderr")), "missing JPEG with successful exit");
        require(idle == 1 && provider.m_failed.contains(12), "missing JPEG completes queue");

        provider.m_activePrefix = provider.m_temp.filePath("corrupt_");
        QFile corrupt(provider.m_activePrefix + "00002.jpg");
        require(corrupt.open(QIODevice::WriteOnly), "create corrupt image fixture");
        corrupt.write("not a JPEG");
        corrupt.close();
        provider.processFinished(0, QProcess::NormalExit);
        require(failure.contains(QStringLiteral("图片读取失败")) && failure.contains(QStringLiteral("10 字节")), "corrupt JPEG differs from missing file");
        provider.m_activeOutputOrder.clear();
        provider.processFinished(0, QProcess::NormalExit);
        require(failure.contains(QStringLiteral("不在当前解码段的输出映射")), "suppressed output mapping diagnosed");
        provider.processFinished(9, QProcess::CrashExit);
        require(failure.contains(QStringLiteral("异常终止")) && failure.contains(QStringLiteral("退出码 9")), "process crash diagnosed");
        provider.processError(QProcess::FailedToStart);
        require(failure.contains(QStringLiteral("无法启动")) && idle == 5, "launch failure finishes queue");
        const int previousFailures = failures;
        provider.m_cancelled = true;
        provider.processFinished(9, QProcess::CrashExit);
        require(failures == previousFailures, "user cancellation is not an error");
        provider.m_stream = std::make_shared<StreamInfo>();
        provider.m_stream->codec = Codec::Mpeg4Visual;
        provider.m_processIoError.clear();
        provider.m_activeOutputOrder = {12};
        provider.m_activePrefix = provider.m_temp.filePath("missing_mpeg4_");
        provider.processFinished(0, QProcess::NormalExit);
        require(failure.contains(QStringLiteral("输出帧数不匹配")), "unexpected MPEG-4 drops fail instead of misbinding frames");
    }
};

struct MainWindowStatusTest {
    static void hoverDetails()
    {
        QString motionError;
        auto vectors = AvcMotionProvider::parseOutput("BSMV1 7 32 32 2\n-1 8 8 16 16 -3 8 4\n1 8 8 16 16 0 0 4\n", 7, motionError);
        require(motionError.isEmpty() && vectors.size() == 2 && vectors[0].dx() == -.75
            && vectors[0].dy() == 2 && vectors[1].list == 1, "motion precision and L0/L1 preserved");
        AvcMotionProvider::parseOutput("BSMV1 6 32 32 0\n", 7, motionError);
        require(!motionError.isEmpty(), "wrong reordered frame identity rejected");
        AvcMotionProvider::parseOutput("BSMV1 7 32 32 1\n-1 8 8 16 16 1 2 0\n", 7, motionError);
        require(!motionError.isEmpty(), "invalid vector scale rejected");
        AvcMotionProvider::parseOutput("BSMV1 7 32 32 0\n", 7, motionError);
        require(motionError.isEmpty(), "valid zero-vector-count frame accepted");
        require(avcPredictionName("P").contains("PCM"), "P debug flag is PCM, not a P frame");
        require(avcPredictionName("i").contains(QStringLiteral("未区分")), "generic intra flag does not invent transform size");
        require(avcPredictionName("d").contains("Skip"), "direct skip is explained");
        require(avcPartitionName("X+").startsWith("8×8")
            && avcPartitionName(">|=") == QStringLiteral("8×16"), "debug partitions and interlacement");
        require(avcPartitionName("") == QStringLiteral("未获取"), "missing partition is not 16x16");

        MainWindow window;
        auto stream = std::make_shared<StreamInfo>();
        stream->fileName = QStringLiteral("learning <sample>.h264");
        stream->width = 320;
        stream->height = 240;
        FrameInfo frame;
        frame.index = 0;
        frame.type = "P";
        frame.displayType = "P";
        frame.unitColumns = 20;
        frame.unitRows = 15;
        frame.decodedUnitsReady = true;
        frame.decodedUnits.resize(300);
        for (int i = 0; i < 300; ++i) {
            auto &unit = frame.decodedUnits[i];
            unit.address = i;
            unit.column = i % 20;
            unit.row = i / 20;
            unit.qp = i == 0 ? -1 : 24;
            unit.decoderType = i == 0 ? QString() : (i == 1 ? QStringLiteral("d") : QStringLiteral(">|"));
        }
        stream->frames.push_back(frame);
        window.m_stream = stream;
        window.m_currentFrame = 0;
        window.updateCuTree(0);
        require(window.m_cuTree->topLevelItemCount() == 0, "preview does not materialize macroblock tree");
        window.m_macroView->setStream(stream);
        window.m_macroView->setFrame(0);
        window.show();
        QApplication::processEvents();
        auto *view = window.m_macroView;
        const auto captureBubble = [view](const QString &suffix) {
            const QString path = qEnvironmentVariable("BITSCOPE_LEARNING_SCREENSHOT");
            if (!path.isEmpty()) require(view->m_hoverBubble->grab().save(path + suffix), "codec bubble visual capture");
            require(view->m_hoverBubble->screen()->availableGeometry().contains(view->m_hoverBubble->geometry()), "codec bubble fits screen");
        };
        const auto hover = [view](int x, int y) {
            const QRectF picture = view->pictureRect();
            const QPointF position(picture.left() + (x + .5) * picture.width() / view->m_stream->width,
                                   picture.top() + (y + .5) * picture.height() / view->m_stream->height);
            QMouseEvent move(QEvent::MouseMove, position, view->mapToGlobal(position.toPoint()),
                             Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view, &move);
        };
        hover(1, 1);
        auto *bubble = view->m_hoverBubble;
        require(bubble->isVisible() && bubble->text().contains(QStringLiteral("宏块 #0")), "hover displays current macroblock bubble");
        require(bubble->text().contains(QStringLiteral("实际 QP：未获取")), "missing QP not fabricated");
        require(bubble->testAttribute(Qt::WA_TransparentForMouseEvents)
            && bubble->testAttribute(Qt::WA_TranslucentBackground), "bubble is translucent and does not intercept pointer");
        hover(17, 1);
        require(bubble->text().contains("B Skip / Direct")
            && bubble->text().contains(QStringLiteral("正在按需提取")), "skip is not a fabricated motion vector");
        stream->frames[0].decodedUnits[1].qp = 31;
        view->refreshFrameData();
        require(bubble->text().contains(QStringLiteral("实际 QP：31")), "stationary hover refreshes on decoded data arrival");
        stream->frames[0].decodedUnits[1].qp = 24;
        hover(319, 239);
        require(bubble->text().contains(QStringLiteral("宏块 #299")), "last macroblock hit test");
        require(bubble->screen()->availableGeometry().contains(bubble->geometry()), "bubble clamped inside screen");
        view->setZoom(2);
        require(!bubble->isVisible(), "zoom hides stale bubble");
        view->resetView();
        hover(1, 1);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(view, &leave);
        require(!bubble->isVisible(), "leaving preview hides bubble");
        const auto clickAt = [view](QPointF point) {
            QMouseEvent press(QEvent::MouseButtonPress, point, view->mapToGlobal(point.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, point, view->mapToGlobal(point.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(view, &release);
        };
        const QPointF selection = view->pictureRect().center();
        clickAt(selection);
        require(!view->findChild<QTextBrowser *>(QStringLiteral("motionSelectionCard")), "click does not create a locked detail card");
        hover(17, 1);
        require(bubble->isVisible() && bubble->text().contains(QStringLiteral("宏块 #1")), "hover still follows pointer after click");
        QApplication::sendEvent(view, &leave);
        require(!bubble->isVisible(), "click does not pin hover details");
        view->setZoom(2);
        QMouseEvent press(QEvent::MouseButtonPress, selection, view->mapToGlobal(selection.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view, &press);
        const QPointF dragged = selection + QPointF(50, 30);
        QMouseEvent move(QEvent::MouseMove, dragged, view->mapToGlobal(dragged.toPoint()), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, dragged, view->mapToGlobal(dragged.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view, &release);
        require(!view->m_pan.isNull() && !view->m_pressed && !view->m_panning, "drag still pans and releases normally");
        view->resetView();

        window.updateFrameStatistics(0);
        for (int i = 0; i < window.m_tabs->count(); ++i)
            require(!window.m_tabs->tabText(i).contains(QStringLiteral("学习")), "learning tab removed");
        require(window.m_frameStats->toPlainText().contains("24.00"), "frame QP statistics retained");
        require(stream->entropyCodingName() == QStringLiteral("未获取"), "missing PPS does not invent CAVLC");
        NalUnit n; n.hasSlice = true; n.slice.entropyCoding = 0;
        stream->nals = {n};
        require(stream->entropyCodingName() == "CAVLC", "CAVLC identified");
        n.slice.entropyCoding = 1; stream->nals.append(n);
        require(stream->entropyCodingName().contains(QStringLiteral("混合")), "changed PPS modes retained per slice");
        window.updateMetrics();
        require(window.m_codecMetric->text().contains("CABAC / CAVLC"), "entropy visible in codec card");

        stream->codec = Codec::Mpeg4Visual;
        stream->frames[0].decodedUnits[1].category = QStringLiteral("双向预测");
        hover(17, 1);
        require(bubble->text().contains("MPEG-4") && bubble->text().contains("VOP")
            && bubble->text().contains(QStringLiteral("实际 QP：24")) && !bubble->text().contains("H.264"), "MPEG4 gets its own macroblock bubble");
        require(stream->entropyCodingName() == "VLC", "MPEG4 entropy category");
        captureBubble(".mpeg4.png");
        stream->codec = Codec::Hevc;
        auto &detail = stream->frames[0];
        detail.unitSize = 64; detail.unitColumns = 5; detail.unitRows = 4;
        hover(17, 1);
        require(bubble->text().contains(QStringLiteral("详细块信息加载中")), "HEVC loading root is not a fake leaf CU");
        HevcCodingUnitInfo cu; cu.ctuAddress = 0; cu.x = 16; cu.y = 0; cu.width = cu.height = cu.codedSize = 16;
        cu.depth = 2; cu.qp = 27; cu.partitionName = "2NxN"; cu.prediction = QStringLiteral("帧间预测"); cu.approximateBytes = 19;
        MotionVectorInfo hevcMv; hevcMv.list = 1; hevcMv.predictionUnit = 0; hevcMv.referencePoc = 9;
        hevcMv.centerX = 24; hevcMv.centerY = 4; hevcMv.width = 16; hevcMv.height = 8;
        hevcMv.motionX = -3; hevcMv.motionY = 8; hevcMv.scale = 4;
        cu.motionVectors = {hevcMv}; cu.motionVectorsReady = true;
        detail.hevcCodingUnits = {cu}; detail.hevcCodingUnitsReady = true;
        view->refreshFrameData();
        require(view->m_hoverCu == 0 && bubble->text().contains("H.265 CU")
            && bubble->text().contains("2NxN") && bubble->text().contains(QStringLiteral("实际 QP：27"))
            && bubble->text().contains(QStringLiteral("CABAC 近似值")), "stationary hover resolves newly arrived exact CU");
        require(bubble->text().contains("参考 POC 9") && bubble->text().contains("Δ(-0.75,2.00)"), "HEVC bubble has reference and pixel displacement");
        require(stream->entropyCodingName() == "CABAC", "HEVC entropy category");
        captureBubble(".hevc.png");
        hover(100, 100);
        require(bubble->text().contains(QStringLiteral("没有已导出的叶子块")), "uncovered CU does not keep previous details");
        detail.hevcCodingUnitsReady = false; detail.hevcCodingUnitsError = "decoder <failed>";
        view->refreshFrameData();
        require(bubble->text().contains("decoder &lt;failed&gt;"), "decoder failure escaped in bubble");

        stream->codec = Codec::Av1;
        Av1CodingBlockInfo block; block.superblockAddress = 0; block.x = 16; block.y = 0;
        block.width = block.codedWidth = 16; block.height = block.codedHeight = 8;
        block.qIndex = 123; block.segmentId = 2; block.predictionMode = "NEWMV"; block.uvPredictionMode = "UV DC PRED";
        block.transformSize = "8X8"; block.transformType = "DCT DCT"; block.referenceFrames = "LAST FRAME + ALTREF FRAME";
        detail.av1CodingBlocks = {block}; detail.av1CodingBlocksReady = true;
        hover(17, 1);
        require(bubble->text().contains("AV1 Coding Block") && bubble->text().contains("16×8")
            && bubble->text().contains("QIndex：123") && bubble->text().contains(QStringLiteral("不是零矢量")), "AV1 block shape and missing MV are accurate");
        detail.av1CodingBlocks[0].motionVectorsAvailable = true;
        detail.av1CodingBlocks[0].motionVector0X = -8;
        view->refreshFrameData();
        require(bubble->text().contains("MV0：(-8, 0)") && bubble->text().contains(QStringLiteral("左"))
            && bubble->text().contains("MV1：(0, 0)"), "AV1 actual vectors and zero vector distinguished");
        require(bubble->text().contains("Δ(-1.000, 0.000)") && bubble->text().contains("参考：LAST FRAME"), "AV1 eighth-pixel precision and reference association");
        require(stream->entropyCodingName() == QStringLiteral("多符号算术编码"), "AV1 entropy category");
        captureBubble(".av1.png");
        QApplication::sendEvent(view, &leave);
        require(!bubble->isVisible(), "all-codec tooltip dismisses on leave");
        stream->codec = Codec::Avc;
        stream->frames[0] = frame;
        const QString capture = qEnvironmentVariable("BITSCOPE_LEARNING_SCREENSHOT");
        if (!capture.isEmpty()) require(window.grab().save(capture), "save optional UI verification image");
        // Optional local visual check only; no media/decoder work in normal CI.
        const QString media = qEnvironmentVariable("BITSCOPE_LEARNING_MEDIA");
        if (!media.isEmpty() && !capture.isEmpty()) {
            window.openFile(media);
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 20000) {
                QApplication::processEvents();
                if (!window.m_watcher.isRunning() && window.m_currentFrame >= 0
                    && window.m_stream && !window.m_stream->frames.isEmpty()
                    && window.m_stream->frames.first().decodedUnitsReady) break;
                QThread::msleep(10);
            }
            require(window.m_stream && !window.m_stream->frames.isEmpty()
                && window.m_stream->frames.first().decodedUnitsReady, "real H264 data arrived");
            require(window.grab().save(capture + ".real.png"), "real codec card capture");
            if (window.m_stream->codec == Codec::Avc)
                require(!window.m_tabs->isTabVisible(window.m_tabs->indexOf(window.m_cuTree)), "H264 macroblock tab removed");
            window.m_tabs->setCurrentIndex(0);
            QApplication::processEvents();
            hover(1, 1);
            require(bubble->isVisible(), "real macroblock bubble visible");
            require(bubble->grab().save(capture + ".bubble.png"), "real macroblock bubble capture");
            require(window.grab().save(capture + ".preview.png"), "real preview capture");
            int target = -1;
            for (const auto &f : window.m_stream->frames) if (f.type == "B") { target = f.index; break; }
            if (qEnvironmentVariableIsSet("BITSCOPE_MOTION_FRAME")) target = qEnvironmentVariableIntValue("BITSCOPE_MOTION_FRAME");
            require(target >= 0 && target < window.m_stream->frames.size(), "motion fixture contains requested frame");
            window.showFrame(target);
            window.m_motionProvider->requestFrame(target);
            timer.restart();
            while (timer.elapsed() < 20000) {
                QApplication::processEvents();
                const auto &f = window.m_stream->frames.at(target);
                if (f.motionVectorsReady || !f.motionVectorsError.isEmpty()) break;
                QThread::msleep(10);
            }
            const auto &actual = window.m_stream->frames.at(target);
            if (!actual.motionVectorsReady) qWarning() << actual.motionVectorsError;
            require(actual.motionVectorsReady && !actual.motionVectors.isEmpty(), "real B-frame motion extracted and bound");
            int address = -1;
            bool l0 = false, l1 = false;
            for (const auto &mv : actual.motionVectors) {
                l0 |= mv.list == 0; l1 |= mv.list == 1;
                if (mv.motionX || mv.motionY) address = mv.centerY / 16 * actual.unitColumns + mv.centerX / 16;
            }
            qInfo() << "Motion frame" << target << actual.type << "vectors" << actual.motionVectors.size()
                    << "L0" << l0 << "L1" << l1 << "nonzero address" << address;
            require((l0 || l1) && address >= 0, "motion frame contains valid lists and nonzero vectors");
            hover(address % actual.unitColumns * 16 + 8, address / actual.unitColumns * 16 + 8);
            require(bubble->isVisible() && bubble->text().contains(QStringLiteral("实际 MV")), "actual motion remains available on hover");
            require(bubble->grab().save(capture + ".motion.png"), "real motion bubble capture");
        }
        window.close();
    }
    static void run()
    {
        MainWindow window;
        window.m_currentFrame = 12;
        window.m_lastPath = QStringLiteral("/test/sample.mp4");
        const QString reason = QStringLiteral("未生成图片 · Permission denied\nFFmpeg：/tools/ffmpeg\n")
            + QStringLiteral("诊断日志\n").repeated(120);
        window.m_previews->previewFailed(12, reason);
        require(window.m_status->text().contains("F12") && window.m_status->text().contains("Permission denied"), "selected frame error visible");
        require(!window.m_statusDetails->isHidden(), "details button shown");
        window.m_previews->statusChanged(QStringLiteral("预览加载完成"));
        window.m_hevcCuProvider->statusChanged(QStringLiteral("HEVC CU 已加载"));
        window.m_av1BlockProvider->statusChanged(QStringLiteral("AV1 Blocks 已加载"));
        require(window.m_status->text().contains("Permission denied"), "background success cannot overwrite error");
        window.m_previews->previewFailed(13, QStringLiteral("other frame error"));
        require(!window.m_status->text().contains("other frame error"), "background failure does not replace selected error");
        require(window.m_model->thumbnailError(12) == reason, "details not truncated");

        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            require(dialog != nullptr, "error details dialog opened");
            auto *text = dialog->findChild<QPlainTextEdit *>();
            require(text && text->toPlainText().contains(reason), "complete captured diagnostics in dialog");
            bool copied = false;
            for (auto *button : dialog->findChildren<QPushButton *>()) {
                if (button->text() == QStringLiteral("复制全部")) {
                    button->click();
                    copied = true;
                }
            }
            require(copied && QApplication::clipboard()->text().contains(reason), "copy complete diagnostics");
            dialog->accept();
        });
        window.showPreviewErrorDetails();

        window.m_currentFrame = 14;
        window.refreshStatus();
        require(window.m_statusDetails->isHidden() && !window.m_status->text().contains("Permission denied"), "selection changes clear unrelated error");
        window.m_currentFrame = 12;
        window.refreshStatus();
        require(window.m_status->text().contains("Permission denied"), "reselect restores frame error");
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::black);
        window.m_previews->thumbnailReady(12, image);
        require(window.m_model->thumbnailError(12).isEmpty() && window.m_statusDetails->isHidden(), "successful retry clears error");
        window.m_previews->previewFailed(12, reason);
        window.m_model->setStream({});
        window.clearDetails(QStringLiteral("正在分析新文件"));
        require(window.m_statusDetails->isHidden(), "new file clears old error");
    }
};
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    bitscope::PreviewProviderDiagnosticsTest::run();
    bitscope::MainWindowStatusTest::run();
    bitscope::MainWindowStatusTest::hoverDetails();
    bitscope::HevcMotionTest::run();
    qInfo("PASS: preview failure diagnostics, sticky status, details/copy, selection, retry and reset");
    return 0;
}
