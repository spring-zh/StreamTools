#include "bitstreamparser.h"
#include "hevccuprovider.h"

#include <QCoreApplication>
#include <QDebug>
#include <QSet>
#include <QTimer>

#include <algorithm>

using namespace bitscope;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc < 3 || argc > 5) {
        qCritical() << "usage: BitScopeHevcCuSmoke <hevc-file> <HM_180-helper> [first-frame] [last-frame]";
        return 2;
    }
    qputenv("BITSCOPE_HEVC_ANALYZER", argv[2]);
    const StreamPtr stream = BitstreamParser::parseFile(QString::fromLocal8Bit(argv[1]));
    if (!stream || !stream->error.isEmpty() || stream->codec != Codec::Hevc
        || stream->frames.isEmpty()) {
        qCritical() << "HEVC input parse failed" << (stream ? stream->error : QString());
        return 3;
    }

    HevcCuProvider provider;
    provider.setStream(stream);
    bool firstOk = argc == 3;
    bool lastOk = argc < 5;
    const int firstFrame = argc >= 4
        ? QString::fromLocal8Bit(argv[3]).toInt(&firstOk) : 45;
    const int lastFrame = argc == 5
        ? QString::fromLocal8Bit(argv[4]).toInt(&lastOk) : firstFrame;
    if (!firstOk || !lastOk || firstFrame < 0 || lastFrame < firstFrame
        || lastFrame >= stream->frames.size()) {
        qCritical() << "invalid target range" << firstFrame << lastFrame;
        return 7;
    }
    QSet<int> pending;
    for (int frame = firstFrame; frame <= lastFrame; ++frame)
        pending.insert(frame);
    int exitCode = 4;
    QObject::connect(&provider, &HevcCuProvider::codingUnitsReady,
                     &application, [&](int frameIndex,
                                       const QVector<HevcCodingUnitInfo> &units) {
        if (!pending.contains(frameIndex))
            return;
        qint64 coveredPixels = 0;
        bool hasQp = false;
        bool hasSplit = false;
        int motionCount = 0, motionMissing = 0;
        for (const auto &unit : units) {
            coveredPixels += qint64(unit.width) * unit.height;
            hasQp |= unit.qp >= 0;
            hasSplit |= unit.depth > 0;
            motionCount += unit.motionVectors.size();
            motionMissing += !unit.motionVectorsReady;
        }
        qInfo() << "HEVC frame" << frameIndex << "POC"
                << stream->frames.at(frameIndex).poc << "coding units" << units.size()
                << "QP" << hasQp << "split" << hasSplit << "coverage"
                << coveredPixels << "/" << qint64(stream->width) * stream->height;
        qInfo() << "Motion vectors" << motionCount << "CUs missing motion records" << motionMissing;
        if (qEnvironmentVariableIsSet("BITSCOPE_REQUIRE_MOTION") && (motionCount == 0 || motionMissing != 0)) {
            exitCode = 8; application.quit(); return;
        }
        if (units.isEmpty() || !hasQp
            || coveredPixels != qint64(stream->width) * stream->height) {
            exitCode = 5;
            application.quit();
            return;
        }
        pending.remove(frameIndex);
        if (pending.isEmpty()) {
            exitCode = 0;
            application.quit();
        }
    });
    QObject::connect(&provider, &HevcCuProvider::codingUnitsUnavailable,
                     &application, [&](int frameIndex, const QString &reason) {
        if (pending.contains(frameIndex)) {
            qCritical() << reason;
            exitCode = 6;
            application.quit();
        }
    });
    QTimer::singleShot(60000, &application, [&] {
        qCritical() << "HEVC coding unit extraction timed out";
        application.quit();
    });
    if (firstFrame == lastFrame)
        provider.requestFrame(firstFrame);
    else
        provider.requestRange(firstFrame, lastFrame);
    application.exec();
    return exitCode;
}
