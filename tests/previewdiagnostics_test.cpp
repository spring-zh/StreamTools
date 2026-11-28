#include "mainwindow.h"
#include "previewprovider.h"
#include "frametablemodel.h"
#include "hevccuprovider.h"
#include "av1blockprovider.h"
#include "platformwindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>

static void require(bool condition, const char *message)
{
    if (!condition) qFatal("Preview diagnostics: %s", message);
}

namespace bitscope {
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
    qInfo("PASS: preview failure diagnostics, sticky status, details/copy, selection, retry and reset");
    return 0;
}
