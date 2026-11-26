#include "bitstreamparser.h"
#include "hevccuprovider.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

#include <algorithm>

using namespace bitscope;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        qCritical() << "usage: BitScopeHevcCuSmoke <hevc-file> <HM_180-helper>";
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
    const int targetFrame = std::min(45, int(stream->frames.size()) - 1);
    int exitCode = 4;
    QObject::connect(&provider, &HevcCuProvider::codingUnitsReady,
                     &application, [&](int frameIndex,
                                       const QVector<HevcCodingUnitInfo> &units) {
        if (frameIndex != targetFrame)
            return;
        qint64 coveredPixels = 0;
        bool hasQp = false;
        bool hasSplit = false;
        for (const auto &unit : units) {
            coveredPixels += qint64(unit.width) * unit.height;
            hasQp |= unit.qp >= 0;
            hasSplit |= unit.depth > 0;
        }
        qInfo() << "HEVC frame" << frameIndex << "POC"
                << stream->frames.at(frameIndex).poc << "coding units" << units.size()
                << "QP" << hasQp << "split" << hasSplit << "coverage"
                << coveredPixels << "/" << qint64(stream->width) * stream->height;
        exitCode = !units.isEmpty() && hasQp
                && coveredPixels == qint64(stream->width) * stream->height ? 0 : 5;
        application.quit();
    });
    QObject::connect(&provider, &HevcCuProvider::codingUnitsUnavailable,
                     &application, [&](int frameIndex, const QString &reason) {
        if (frameIndex == targetFrame) {
            qCritical() << reason;
            exitCode = 6;
            application.quit();
        }
    });
    QTimer::singleShot(60000, &application, [&] {
        qCritical() << "HEVC coding unit extraction timed out";
        application.quit();
    });
    provider.requestFrame(targetFrame);
    application.exec();
    return exitCode;
}
