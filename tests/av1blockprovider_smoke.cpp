#include "av1blockprovider.h"
#include "bitstreamparser.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

#include <algorithm>

using namespace bitscope;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        qCritical() << "usage: BitScopeAv1BlockSmoke <av1-file> <inspection-helper>";
        return 2;
    }
    qputenv("BITSCOPE_AV1_INSPECT", argv[2]);
    const StreamPtr stream = BitstreamParser::parseFile(QString::fromLocal8Bit(argv[1]));
    if (!stream || !stream->error.isEmpty() || stream->codec != Codec::Av1
        || stream->frames.isEmpty()) {
        qCritical() << "AV1 input parse failed" << (stream ? stream->error : QString());
        return 3;
    }

    Av1BlockProvider provider;
    provider.setStream(stream);
    int targetFrame = std::min(5, int(stream->frames.size()) - 1);
    for (int index = 1; index < stream->frames.size(); ++index) {
        if (stream->frames.at(index - 1).displayType == QStringLiteral("SHOW_EXISTING")
            && stream->frames.at(index).displayType != QStringLiteral("SHOW_EXISTING")) {
            targetFrame = index;
            break;
        }
    }
    int exitCode = 4;
    QObject::connect(&provider, &Av1BlockProvider::codingBlocksReady,
                     &application, [&](int frameIndex, const QVector<Av1CodingBlockInfo> &blocks) {
        if (frameIndex != targetFrame)
            return;
        bool hasNonSquareBlock = false;
        bool hasDetailedFields = false;
        qint64 coveredPixels = 0;
        for (const auto &block : blocks) {
            hasNonSquareBlock |= block.codedWidth != block.codedHeight;
            hasDetailedFields |= block.qIndex >= 0 && !block.predictionMode.isEmpty()
                && !block.transformSize.isEmpty();
            coveredPixels += qint64(block.width) * block.height;
        }
        qInfo() << "AV1 frame" << frameIndex << "coding blocks" << blocks.size()
                << "non-square" << hasNonSquareBlock << "details" << hasDetailedFields
                << "coverage" << coveredPixels << "/" << stream->width * stream->height;
        exitCode = !blocks.isEmpty() && hasDetailedFields
                && coveredPixels == qint64(stream->width) * stream->height ? 0 : 5;
        application.quit();
    });
    QObject::connect(&provider, &Av1BlockProvider::codingBlocksUnavailable,
                     &application, [&](int frameIndex, const QString &reason) {
        if (frameIndex == targetFrame) {
            qCritical() << reason;
            exitCode = 6;
            application.quit();
        }
    });
    QTimer::singleShot(30000, &application, [&] {
        qCritical() << "AV1 coding block extraction timed out";
        application.quit();
    });
    provider.requestFrame(targetFrame);
    application.exec();
    return exitCode;
}
