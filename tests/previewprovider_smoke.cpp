#include "bitstreamparser.h"
#include "previewprovider.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSet>
#include <QTimer>

using namespace bitscope;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc < 3 || argc > 4) {
        qCritical() << "usage: BitScopePreviewSmoke <video-file> <first-frame> [last-frame]";
        return 2;
    }
    bool firstOk = false;
    bool lastOk = argc == 3;
    const int firstFrame = QString::fromLocal8Bit(argv[2]).toInt(&firstOk);
    const int lastFrame = argc == 4
        ? QString::fromLocal8Bit(argv[3]).toInt(&lastOk) : firstFrame;
    const StreamPtr stream = BitstreamParser::parseFile(QString::fromLocal8Bit(argv[1]));
    if (!stream || !stream->error.isEmpty() || stream->frames.isEmpty()) {
        qCritical() << "input parse failed" << (stream ? stream->error : QString());
        return 3;
    }
    if (!firstOk || !lastOk || firstFrame < 0 || lastFrame < firstFrame
        || lastFrame >= stream->frames.size()) {
        qCritical() << "invalid target range" << firstFrame << lastFrame;
        return 4;
    }

    PreviewProvider provider;
    provider.setStream(stream);
    QSet<int> pending;
    for (int frame = firstFrame; frame <= lastFrame; ++frame)
        pending.insert(frame);
    int exitCode = 5;
    QObject::connect(&provider, &PreviewProvider::thumbnailReady,
                     &application, [&](int frameIndex, const QImage &image) {
        if (!pending.contains(frameIndex))
            return;
        qInfo() << "preview frame" << frameIndex << "PTS"
                << stream->frames.at(frameIndex).pts << "size" << image.size();
        if (image.isNull()) {
            exitCode = 6;
            application.quit();
            return;
        }
        pending.remove(frameIndex);
        if (pending.isEmpty()) {
            exitCode = 0;
            application.quit();
        }
    });
    QObject::connect(&provider, &PreviewProvider::previewFailed,
                     &application, [&](int frameIndex, const QString &reason) {
        if (pending.contains(frameIndex)) {
            qCritical() << reason;
            exitCode = 7;
            application.quit();
        }
    });
    QTimer::singleShot(60000, &application, [&] {
        qCritical() << "preview extraction timed out";
        application.quit();
    });
    if (firstFrame == lastFrame)
        provider.requestFrame(firstFrame);
    else
        provider.requestRange(firstFrame, lastFrame);
    application.exec();
    return exitCode;
}
