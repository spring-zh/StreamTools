#include "previewprovider.h"
#include "mpeg4access.h"

#include <QCoreApplication>
#include <QDebug>

namespace bitscope {
struct PreviewProviderLogTest {
    static QVector<QVector<DecodedUnitInfo>> parse(const QByteArray &log, Codec codec,
                                                  int width = 32, int height = 32,
                                                  int count = 1)
    {
        PreviewProvider provider;
        auto stream = std::make_shared<StreamInfo>();
        stream->codec = codec;
        stream->width = width;
        stream->height = height;
        provider.setStream(stream);
        return provider.parseAvcDecodedUnits(log, count);
    }
};
}
using namespace bitscope;

static void check(bool condition, const char *message)
{
    if (!condition)
        qFatal("Macroblock regression: %s", message);
}

// FFmpeg qp+mb_type: two columns of QP followed by three type/partition flags.
static QByteArray cell(int qp, const char *type)
{
    return QByteArray::number(qp).rightJustified(2, ' ')
        + QByteArray(type).leftJustified(3, ' ');
}

static QByteArray frame(const QByteArray &prefix, int qp = 12,
                        const QByteArray &eol = "\n")
{
    return prefix + "New frame, type: P" + eol
        + prefix + "   0 " + cell(qp, "I") + cell(7, ">") + eol
        + prefix + "  16 " + cell(23, "S") + cell(31, "X+") + eol;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    auto openGop = std::make_shared<StreamInfo>();
    openGop->codec = Codec::Mpeg4Visual;
    // Coded order: I P B B I B B P B B I B B P B B.
    // The B pictures after each later I use a reference from the preceding GOP.
    const QStringList types{"I", "P", "B", "B", "I", "B", "B", "P", "B", "B", "I", "B", "B", "P", "B", "B"};
    for (const auto &type : types) {
        FrameInfo frame;
        frame.index = openGop->frames.size();
        frame.type = type;
        frame.randomAccess = type == "I";
        frame.referencePicture = type != "B";
        openGop->frames.push_back(frame);
    }
    check(nearestMpeg4DecodeStart(openGop, 4) == 4, "I keeps nearest GOP fast path");
    check(nearestMpeg4DecodeStart(openGop, 5) == 0, "leading B requests preceding reference GOP");
    check(nearestMpeg4DecodeStart(openGop, 11) == 4, "leading B does not fall back to file start");
    check(nearestMpeg4DecodeStart(openGop, 14) == 10, "ordinary B keeps nearest GOP fast path");
    check(mpeg4PreviewOutputOrder(openGop, 4, 9) == QVector<int>({4, 7, 8, 9}), "omit initial leading B from image and MB mapping");
    check(mpeg4PreviewOutputOrder(openGop, 4, 12) == QVector<int>({4, 7, 8, 9, 10, 11, 12}), "retain leading B once references exist");
    check(mpeg4PreviewOutputOrder(openGop, 0, 3) == QVector<int>({0, 1, 2, 3}), "first GOP unchanged");
    for (Codec codec : {Codec::Avc, Codec::Mpeg4Visual}) {
        const QByteArray name = codec == Codec::Avc ? "h264" : "mpeg4";
        for (const QByteArray &address : {QByteArray("0000021AB0CDEF00"),
                                         QByteArray("0x7fab1234"), QByteArray("0XABCDEF")}) {
            const QByteArray prefix = "[" + name + " @ " + address + "] ";
            for (const QByteArray &eol : {QByteArray("\n"), QByteArray("\r\n")}) {
                const auto parsed = PreviewProviderLogTest::parse(frame(prefix, 12, eol), codec);
                check(parsed.size() == 1 && parsed[0].size() == 4, "platform address / newline format");
                check(parsed[0][0].qp == 12 && parsed[0][1].qp == 7
                      && parsed[0][2].qp == 23 && parsed[0][3].qp == 31, "QP values");
                check(parsed[0][3].address == 3 && parsed[0][3].column == 1
                      && parsed[0][3].row == 1 && parsed[0][3].decoderType == "X+", "cell coordinates and type");
            }
        }
        const QByteArray prefix = "[" + name + " @ 0000021AB0CDEF00] ";
        const QByteArray dense = prefix + "New frame, type: B\n" + prefix + "   0 "
            + cell(12, "X+=") + cell(23, ">|=") + "\n"
            + prefix + "  16 " + cell(7, "A") + cell(5, "P") + "\n";
        const auto parsed = PreviewProviderLogTest::parse(dense, codec);
        check(parsed.size() == 1 && parsed[0].size() == 4, "adjacent fixed-width macroblocks");
        check(parsed[0][1].qp == 23 && parsed[0][1].decoderType == ">|=", "interlaced partition flags");
        check(parsed[0][2].category == QStringLiteral("帧内预测")
              && parsed[0][3].category == QStringLiteral("帧内预测"), "AC prediction / PCM categories");
        const auto gap = PreviewProviderLogTest::parse(
            frame(prefix, 11) + prefix + "New frame, type: P\n" + frame(prefix, 24), codec, 32, 32, 3);
        check(gap.size() == 3 && gap[0][0].qp == 11 && gap[1].isEmpty()
              && gap[2][0].qp == 24, "missing data must not shift later frames");
        const auto probe = PreviewProviderLogTest::parse(
            frame("[" + name + " @ 0x1234] ", 10) + frame(prefix, 21) + frame(prefix, 22), codec, 32, 32, 2);
        check(probe.size() == 2 && probe[0][0].qp == 21 && probe[1][0].qp == 22, "probe versus decode context");
        QByteArray mixed = frame(prefix, 20);
        mixed.replace(prefix + "New frame", "  in[0] 'default': New frame");
        const auto interleaved = PreviewProviderLogTest::parse(frame(prefix, 11) + mixed, codec, 32, 32, 2);
        check(interleaved.size() == 2 && interleaved[0][0].qp == 11
              && interleaved[1][0].qp == 20, "filter messages must not swallow frame boundaries");
        QByteArray missingPrefix = frame(prefix, 19);
        missingPrefix.replace(prefix + "  16 ", "filter output:   16 ");
        const auto orphanRow = PreviewProviderLogTest::parse(missingPrefix, codec);
        check(orphanRow.size() == 1 && orphanRow[0][2].qp == 23
              && orphanRow[0][3].qp == 31, "row without context prefix");
        const auto empty = PreviewProviderLogTest::parse(prefix + "New frame, type: P\n", codec, 16, 16);
        check(empty.size() == 1 && empty[0].isEmpty(), "empty one-macroblock frame is not valid");
        QByteArray large = prefix + "New frame, type: P\r\n";
        for (int y = 0; y < 135; ++y) {
            large += prefix + QByteArray::number(y * 16).rightJustified(5, ' ') + ' ';
            for (int x = 0; x < 240; ++x)
                large += cell(1 + (x + y) % 31, "X+=");
            large += "\r\n";
        }
        const auto uhd = PreviewProviderLogTest::parse(large, codec, 3840, 2160);
        check(uhd.size() == 1 && uhd[0].size() == 32400 && uhd[0].last().qp == 2, "4K rows must not be truncated");
    }
    qInfo() << "PASS: Windows/POSIX H.264 and MPEG-4 logs, QP, types, frame gaps and 4K grids";
    return 0;
}
