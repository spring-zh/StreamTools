#include "bitstreamparser.h"
#include "previewprovider.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QImage>
#include <QSet>
#include <QTimer>
#include <algorithm>

using namespace bitscope;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc < 3 || argc > 5 || (argc == 5 && QByteArray(argv[4]) != "--verify-images")) {
        qCritical() << "usage: BitScopePreviewSmoke <video-file> <first-frame> [last-frame] [--verify-images]";
        return 2;
    }
    bool firstOk = false;
    bool lastOk = argc == 3;
    const int firstFrame = QString::fromLocal8Bit(argv[2]).toInt(&firstOk);
    const int lastFrame = argc >= 4
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
    QTemporaryDir referenceDir;
    QVector<int> displayOrder;
    if (argc == 5) {
        // Test fixture only: compare each preview to ordinary display-order
        // decoding, so coded-order MPEG-4 output cannot silently swap B frames.
        if (!stream->containerFile || stream->frames.size() > 64 || !referenceDir.isValid()) {
            qCritical() << "reference comparison requires a small container fixture (<=64 frames)";
            return 9;
        }
        for (int i = 0; i < stream->frames.size(); ++i)
            displayOrder.push_back(i);
        std::stable_sort(displayOrder.begin(), displayOrder.end(), [&](int a, int b) {
            return stream->frames[a].pts < stream->frames[b].pts;
        });
        QProcess reference;
        reference.start(provider.ffmpegPath(), {"-hide_banner", "-nostats", "-loglevel", "error",
            "-threads", "1", "-i", QString::fromLocal8Bit(argv[1]), "-an",
            "-vf", "scale=320:-2", "-fps_mode", "passthrough", "-q:v", "5",
            "-start_number", "0", "-frames:v", QString::number(stream->frames.size()),
            "-y", QDir(referenceDir.path()).filePath("%05d.jpg")});
        if (!reference.waitForFinished(60000) || reference.exitStatus() != QProcess::NormalExit
            || reference.exitCode() != 0) {
            qCritical() << "reference decode failed" << reference.readAllStandardError();
            return 9;
        }
    }
    QSet<int> pending;
    for (int frame = firstFrame; frame <= lastFrame; ++frame)
        pending.insert(frame);
    const bool requireMacroblocks = stream->codec == Codec::Avc
        || stream->codec == Codec::Mpeg4Visual;
    QSet<int> pendingUnits = requireMacroblocks ? pending : QSet<int>{};
    int exitCode = 5;
    bool failed = false;
    const auto finishIfReady = [&] {
        if (!failed && pending.isEmpty() && pendingUnits.isEmpty()) {
            exitCode = 0;
            application.quit();
        }
    };
    QObject::connect(&provider, &PreviewProvider::decodedUnitsReady,
                     &application, [&](int frameIndex, const QVector<DecodedUnitInfo> &units) {
        if (failed || !pendingUnits.contains(frameIndex))
            return;
        const int columns = (stream->width + 15) / 16;
        const int rows = (stream->height + 15) / 16;
        bool valid = units.size() == columns * rows;
        qint64 qpSum = 0;
        for (int i = 0; i < units.size(); ++i) {
            const auto &unit = units[i];
            valid &= unit.address == i && unit.column == i % columns
                && unit.row == i / columns && unit.qp >= 0 && unit.qp <= 51
                && !unit.decoderType.isEmpty() && !unit.category.isEmpty();
            qpSum += unit.qp;
        }
        if (!valid || units.isEmpty()) {
            qCritical() << "incomplete macroblocks/QP for frame" << frameIndex;
            failed = true;
            exitCode = 8;
            application.quit();
            return;
        }
        qInfo() << "macroblocks frame" << frameIndex << "count" << units.size()
                << "average QP" << double(qpSum) / units.size();
        pendingUnits.remove(frameIndex);
        finishIfReady();
    });
    QObject::connect(&provider, &PreviewProvider::decodedUnitsUnavailable,
                     &application, [&](int frameIndex, const QString &reason) {
        if (pendingUnits.contains(frameIndex)) {
            qCritical() << "macroblocks unavailable for frame" << frameIndex << reason;
            failed = true;
            exitCode = 8;
            application.quit();
        }
    });
    QObject::connect(&provider, &PreviewProvider::thumbnailReady,
                     &application, [&](int frameIndex, const QImage &image) {
        if (failed || !pending.contains(frameIndex))
            return;
        qInfo() << "preview frame" << frameIndex << "PTS"
                << stream->frames.at(frameIndex).pts << "size" << image.size();
        if (image.isNull()) {
            failed = true;
            exitCode = 6;
            application.quit();
            return;
        }
        if (!displayOrder.isEmpty()) {
            const int position = displayOrder.indexOf(frameIndex);
            const QImage expected(QDir(referenceDir.path()).filePath(
                QStringLiteral("%1.jpg").arg(position, 5, 10, QLatin1Char('0'))));
            if (expected.isNull() || image.convertToFormat(QImage::Format_RGB32)
                != expected.convertToFormat(QImage::Format_RGB32)) {
                qCritical() << "preview does not match reference frame" << frameIndex;
                failed = true;
                exitCode = 10;
                application.quit();
                return;
            }
        }
        pending.remove(frameIndex);
        finishIfReady();
    });
    QObject::connect(&provider, &PreviewProvider::previewFailed,
                     &application, [&](int frameIndex, const QString &reason) {
        if (pending.contains(frameIndex)) {
            failed = true;
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
