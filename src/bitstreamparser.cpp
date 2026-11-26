#include "bitstreamparser.h"

#include "bitreader.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bitscope {
namespace {

struct RawUnit { qint64 offset = 0; int startCode = 0; QByteArray payload; bool lengthPrefixed = false; int sampleIndex = -1; };

QVector<RawUnit> splitUnits(const QByteArray &bytes);
QVector<RawUnit> splitAv1Obus(const QByteArray &bytes, qint64 baseOffset = 0,
                              int sampleIndex = -1);

void field(QVector<SyntaxField> &target, const QString &name, const QVariant &value,
           const QString &bits = {}, const QString &description = {}) {
    target.push_back({name, value.toString(), bits, description});
}

quint16 be16(const QByteArray &data, qint64 pos) {
    if (pos < 0 || pos + 2 > data.size()) return 0;
    return (quint16(quint8(data.at(pos))) << 8U) | quint16(quint8(data.at(pos + 1)));
}

quint32 be32(const QByteArray &data, qint64 pos) {
    if (pos < 0 || pos + 4 > data.size()) return 0;
    return (quint32(quint8(data.at(pos))) << 24U) | (quint32(quint8(data.at(pos + 1))) << 16U)
        | (quint32(quint8(data.at(pos + 2))) << 8U) | quint32(quint8(data.at(pos + 3)));
}

quint32 be24(const QByteArray &data, qint64 pos) {
    if (pos < 0 || pos + 3 > data.size()) return 0;
    return (quint32(quint8(data.at(pos))) << 16U)
        | (quint32(quint8(data.at(pos + 1))) << 8U)
        | quint32(quint8(data.at(pos + 2)));
}

quint64 be64(const QByteArray &data, qint64 pos) {
    return (quint64(be32(data, pos)) << 32U) | quint64(be32(data, pos + 4));
}

quint16 le16(const QByteArray &data, qint64 pos) {
    if (pos < 0 || pos + 2 > data.size()) return 0;
    return quint16(quint8(data.at(pos))) | (quint16(quint8(data.at(pos + 1))) << 8U);
}

quint32 le32(const QByteArray &data, qint64 pos) {
    if (pos < 0 || pos + 4 > data.size()) return 0;
    return quint32(quint8(data.at(pos))) | (quint32(quint8(data.at(pos + 1))) << 8U)
        | (quint32(quint8(data.at(pos + 2))) << 16U)
        | (quint32(quint8(data.at(pos + 3))) << 24U);
}

quint64 le64(const QByteArray &data, qint64 pos) {
    return quint64(le32(data, pos)) | (quint64(le32(data, pos + 4)) << 32U);
}

bool readLeb128(const QByteArray &data, int &position, int end, quint64 &value,
                int *encodedBytes = nullptr) {
    value = 0;
    int shift = 0;
    const int begin = position;
    while (position < end && position < data.size() && position - begin < 8) {
        const quint8 byte = quint8(data.at(position++));
        value |= quint64(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            if (encodedBytes) *encodedBytes = position - begin;
            return true;
        }
        shift += 7;
    }
    position = begin;
    return false;
}

struct Av1ObuHeader {
    int type = 0;
    bool extension = false;
    bool hasSize = false;
    int temporalId = 0;
    int spatialId = 0;
    int headerBytes = 0;
    int sizeBytes = 0;
    int payloadOffset = 0;
    int payloadSize = 0;
};

Av1ObuHeader av1ObuHeader(const QByteArray &obu) {
    Av1ObuHeader result;
    if (obu.isEmpty()) throw std::runtime_error("Empty AV1 OBU");
    const quint8 header = quint8(obu.at(0));
    if (header & 0x80) throw std::runtime_error("Invalid AV1 OBU forbidden bit");
    result.type = (header >> 3) & 0x0f;
    result.extension = (header & 0x04) != 0;
    result.hasSize = (header & 0x02) != 0;
    int position = 1;
    if (result.extension) {
        if (position >= obu.size()) throw std::runtime_error("Truncated AV1 OBU extension");
        const quint8 extension = quint8(obu.at(position++));
        result.temporalId = (extension >> 5) & 0x07;
        result.spatialId = (extension >> 3) & 0x03;
    }
    result.headerBytes = position;
    if (result.hasSize) {
        quint64 payloadSize = 0;
        if (!readLeb128(obu, position, obu.size(), payloadSize, &result.sizeBytes)
            || payloadSize > quint64(obu.size() - position))
            throw std::runtime_error("Invalid AV1 OBU size field");
        result.payloadSize = int(payloadSize);
    } else {
        result.payloadSize = obu.size() - position;
    }
    result.payloadOffset = position;
    return result;
}

QByteArray av1ObuPayload(const QByteArray &obu) {
    const Av1ObuHeader header = av1ObuHeader(obu);
    return obu.mid(header.payloadOffset, header.payloadSize);
}

QString fourcc(const QByteArray &data, qint64 pos) {
    return pos >= 0 && pos + 4 <= data.size() ? QString::fromLatin1(data.mid(pos, 4)) : QString();
}

QPair<qint64, qint64> mpeg4DecoderSpecificInfo(const QByteArray &data,
                                                qint64 begin, qint64 end) {
    for (qint64 pos = begin; pos + 2 < end; ++pos) {
        if (quint8(data.at(pos)) != 0x05)
            continue;
        qint64 cursor = pos + 1;
        qint64 length = 0;
        int lengthBytes = 0;
        while (cursor < end && lengthBytes < 4) {
            const int value = quint8(data.at(cursor++));
            length = (length << 7) | (value & 0x7f);
            ++lengthBytes;
            if (!(value & 0x80))
                break;
        }
        if (length <= 0 || cursor + length > end)
            continue;
        for (qint64 scan = cursor; scan + 4 <= cursor + length; ++scan) {
            if (quint8(data.at(scan)) == 0 && quint8(data.at(scan + 1)) == 0
                && quint8(data.at(scan + 2)) == 1) {
                const int code = quint8(data.at(scan + 3));
                if (code == 0xb0 || (code >= 0x20 && code <= 0x2f))
                    return {scan, cursor + length - scan};
            }
        }
    }
    return {-1, 0};
}

QString boxDescription(const QString &type) {
    static const QMap<QString, QString> descriptions{
        {"ftyp", "文件类型与兼容品牌"}, {"moov", "影片元数据"}, {"mvhd", "影片头"},
        {"trak", "轨道"}, {"tkhd", "轨道头"}, {"mdia", "媒体"}, {"mdhd", "媒体头"},
        {"hdlr", "处理器"}, {"minf", "媒体信息"}, {"vmhd", "视频媒体头"},
        {"smhd", "音频媒体头"}, {"dinf", "数据信息"}, {"dref", "数据引用"},
        {"stbl", "采样表"}, {"stsd", "采样描述"}, {"stts", "解码时间表"},
        {"ctts", "显示时间偏移表"}, {"stsc", "Sample-to-Chunk 表"}, {"stsz", "采样大小表"},
        {"stco", "32 位 Chunk 偏移表"}, {"co64", "64 位 Chunk 偏移表"},
        {"stss", "同步采样表"}, {"avc1", "H.264 视频采样项"}, {"avc3", "H.264 视频采样项"},
        {"hvc1", "H.265 视频采样项"}, {"hev1", "H.265 视频采样项"},
        {"mp4v", "MPEG-4 Part 2 视频采样项"},
        {"av01", "AV1 视频采样项"},
        {"avcC", "AVC 解码配置"}, {"hvcC", "HEVC 解码配置"},
        {"av1C", "AV1 Codec Configuration Record"},
        {"esds", "Elementary Stream Descriptor"},
        {"mdat", "媒体数据"}, {"free", "空闲空间"}, {"skip", "保留空间"},
        {"edts", "编辑列表容器"}, {"elst", "编辑列表"}, {"udta", "用户数据"},
        {"meta", "元数据"}, {"ilst", "元数据项目"}, {"moof", "分片影片"},
        {"traf", "分片轨道"}, {"mvex", "影片分片扩展"}, {"sidx", "分片索引"}
    };
    return descriptions.value(type, QStringLiteral("ISO BMFF Box"));
}

bool isBoxContainer(const QString &type) {
    static const QSet<QString> containers{"moov","trak","mdia","minf","dinf","stbl","edts","udta",
        "meta","ilst","moof","traf","mvex","mfra","ipro","sinf","schi","wave","stsd",
        "avc1","avc3","hvc1","hev1","mp4v","av01","encv","mp4a","enca"};
    return containers.contains(type);
}

void addBoxFields(Mp4BoxInfo &box, const QByteArray &data) {
    const qint64 payload = box.offset + box.headerSize;
    const qint64 end = box.offset + box.size;
    if (box.type == "ftyp" && payload + 8 <= end) {
        field(box.fields, "major_brand", fourcc(data, payload), "fourcc");
        field(box.fields, "minor_version", be32(data, payload + 4), "u(32)");
        QStringList brands; for (qint64 p = payload + 8; p + 4 <= end; p += 4) brands << fourcc(data, p);
        field(box.fields, "compatible_brands", brands.join(QStringLiteral(", ")), "fourcc[]");
    } else if ((box.type == "mvhd" || box.type == "mdhd") && payload + 20 <= end) {
        const int version = quint8(data.at(payload));
        const qint64 timePos = payload + (version == 1 ? 20 : 12);
        const quint32 timescale = be32(data, timePos);
        const quint64 duration = version == 1 ? be64(data, timePos + 4) : be32(data, timePos + 4);
        field(box.fields, "version", version, "u(8)");
        field(box.fields, "timescale", timescale, "u(32)");
        field(box.fields, "duration", duration, version == 1 ? "u(64)" : "u(32)");
        if (timescale) field(box.fields, "duration_seconds", double(duration) / timescale, "derived");
    } else if (box.type == "tkhd" && payload + 32 <= end) {
        const int version = quint8(data.at(payload));
        const qint64 trackPos = payload + (version == 1 ? 20 : 12);
        field(box.fields, "track_ID", be32(data, trackPos), "u(32)");
        if (end >= payload + 8) {
            field(box.fields, "width", be32(data, end - 8) / 65536.0, "16.16 fixed");
            field(box.fields, "height", be32(data, end - 4) / 65536.0, "16.16 fixed");
        }
    } else if (box.type == "hdlr" && payload + 12 <= end) {
        field(box.fields, "handler_type", fourcc(data, payload + 8), "fourcc");
        if (payload + 24 < end) field(box.fields, "name", QString::fromUtf8(data.mid(payload + 24, end - payload - 24)).remove(QChar('\0')), "utf-8");
    } else if (box.type == "stsd" && payload + 8 <= end) {
        field(box.fields, "entry_count", be32(data, payload + 4), "u(32)");
    } else if (QSet<QString>{"stts","ctts","stsc","stco","co64","stss"}.contains(box.type) && payload + 8 <= end) {
        field(box.fields, "entry_count", be32(data, payload + 4), "u(32)");
    } else if (box.type == "stsz" && payload + 12 <= end) {
        field(box.fields, "sample_size", be32(data, payload + 4), "u(32)");
        field(box.fields, "sample_count", be32(data, payload + 8), "u(32)");
    } else if (QSet<QString>{"avc1","avc3","hvc1","hev1","mp4v","av01"}.contains(box.type) && payload + 28 <= end) {
        field(box.fields, "data_reference_index", be16(data, payload + 6), "u(16)");
        field(box.fields, "width", be16(data, payload + 24), "u(16)");
        field(box.fields, "height", be16(data, payload + 26), "u(16)");
    } else if (box.type == "avcC" && payload + 7 <= end) {
        field(box.fields, "configurationVersion", quint8(data.at(payload)), "u(8)");
        field(box.fields, "AVCProfileIndication", quint8(data.at(payload + 1)), "u(8)");
        field(box.fields, "AVCLevelIndication", quint8(data.at(payload + 3)), "u(8)");
        field(box.fields, "nal_length_size", (quint8(data.at(payload + 4)) & 3) + 1, "u(2)+1");
    } else if (box.type == "hvcC" && payload + 23 <= end) {
        field(box.fields, "configurationVersion", quint8(data.at(payload)), "u(8)");
        field(box.fields, "general_profile_idc", quint8(data.at(payload + 1)) & 31, "u(5)");
        field(box.fields, "general_level_idc", quint8(data.at(payload + 12)), "u(8)");
        field(box.fields, "nal_length_size", (quint8(data.at(payload + 21)) & 3) + 1, "u(2)+1");
        field(box.fields, "numOfArrays", quint8(data.at(payload + 22)), "u(8)");
    } else if (box.type == "av1C" && payload + 4 <= end) {
        const quint8 byte0 = quint8(data.at(payload));
        const quint8 byte1 = quint8(data.at(payload + 1));
        const quint8 byte2 = quint8(data.at(payload + 2));
        const quint8 byte3 = quint8(data.at(payload + 3));
        field(box.fields, "marker", (byte0 >> 7) & 1, "u(1)");
        field(box.fields, "version", byte0 & 0x7f, "u(7)");
        field(box.fields, "seq_profile", (byte1 >> 5) & 0x07, "u(3)");
        field(box.fields, "seq_level_idx_0", byte1 & 0x1f, "u(5)");
        field(box.fields, "seq_tier_0", (byte2 >> 7) & 1, "u(1)");
        field(box.fields, "high_bitdepth", (byte2 >> 6) & 1, "u(1)");
        field(box.fields, "twelve_bit", (byte2 >> 5) & 1, "u(1)");
        field(box.fields, "monochrome", (byte2 >> 4) & 1, "u(1)");
        field(box.fields, "chroma_subsampling_x", (byte2 >> 3) & 1, "u(1)");
        field(box.fields, "chroma_subsampling_y", (byte2 >> 2) & 1, "u(1)");
        field(box.fields, "chroma_sample_position", byte2 & 0x03, "u(2)");
        const bool delayPresent = (byte3 & 0x10) != 0;
        field(box.fields, "initial_presentation_delay_present", delayPresent, "u(1)");
        if (delayPresent)
            field(box.fields, "initial_presentation_delay_minus_one", byte3 & 0x0f, "u(4)");
        field(box.fields, "configOBUs_size", end - payload - 4, "bytes");
    } else if (box.type == "esds" && payload + 4 <= end) {
        field(box.fields, "version", quint8(data.at(payload)), "u(8)");
        for (qint64 pos = payload + 4; pos + 15 < end; ++pos) {
            if (quint8(data.at(pos)) != 0x04)
                continue;
            qint64 cursor = pos + 1;
            qint64 descriptorLength = 0;
            for (int lengthByte = 0; cursor < end && lengthByte < 4; ++lengthByte) {
                const int value = quint8(data.at(cursor++));
                descriptorLength = (descriptorLength << 7) | (value & 0x7f);
                if (!(value & 0x80)) break;
            }
            if (descriptorLength < 13 || cursor + descriptorLength > end
                || quint8(data.at(cursor)) != 0x20)
                continue;
            field(box.fields, "objectTypeIndication", QStringLiteral("0x20 (MPEG-4 Visual)"), "u(8)");
            field(box.fields, "streamType", (quint8(data.at(cursor + 1)) >> 2) & 0x3f, "u(6)");
            field(box.fields, "bufferSizeDB", be24(data, cursor + 2), "u(24)");
            field(box.fields, "maxBitrate", be32(data, cursor + 5), "u(32)");
            field(box.fields, "avgBitrate", be32(data, cursor + 9), "u(32)");
            break;
        }
        const auto [start, length] = mpeg4DecoderSpecificInfo(data, payload + 4, end);
        if (start >= 0) {
            field(box.fields, "decoder_specific_info_offset",
                  QStringLiteral("0x%1").arg(start, 0, 16).toUpper(), "bytes");
            field(box.fields, "decoder_specific_info_size", length, "bytes");
        }
    }
}

QVector<Mp4BoxInfo> parseBoxRange(const QByteArray &data, qint64 begin, qint64 end, int depth = 0) {
    QVector<Mp4BoxInfo> boxes;
    if (depth > 24) return boxes;
    qint64 pos = begin;
    while (pos + 8 <= end && pos + 8 <= data.size()) {
        quint64 size = be32(data, pos);
        const QString type = fourcc(data, pos + 4);
        qint64 headerSize = 8;
        if (size == 1) { if (pos + 16 > end) break; size = be64(data, pos + 8); headerSize = 16; }
        else if (size == 0) size = quint64(end - pos);
        if (type == "uuid") headerSize += 16;
        if (size < quint64(headerSize) || size > quint64(end - pos) || pos + qint64(size) > data.size()) break;
        Mp4BoxInfo box; box.type = type; box.offset = pos; box.size = qint64(size); box.headerSize = headerSize; box.description = boxDescription(type);
        addBoxFields(box, data);
        if (isBoxContainer(type)) {
            qint64 childStart = pos + headerSize;
            if (type == "meta") childStart += 4;
            else if (type == "stsd") childStart += 8;
            else if (QSet<QString>{"avc1","avc3","hvc1","hev1","mp4v","av01","encv"}.contains(type)) childStart += 78;
            else if (type == "mp4a" || type == "enca") childStart += 28;
            if (childStart < pos + qint64(size)) box.children = parseBoxRange(data, childStart, pos + qint64(size), depth + 1);
        }
        boxes.push_back(std::move(box));
        pos += qint64(size);
    }
    return boxes;
}

const Mp4BoxInfo *directChild(const Mp4BoxInfo *box, const QString &type) {
    if (!box) return nullptr;
    for (const auto &child : box->children) if (child.type == type) return &child;
    return nullptr;
}

const Mp4BoxInfo *pathChild(const Mp4BoxInfo *box, std::initializer_list<QString> path) {
    const Mp4BoxInfo *current = box;
    for (const QString &type : path) { current = directChild(current, type); if (!current) break; }
    return current;
}

struct Mp4DemuxResult {
    QVector<RawUnit> units;
    QVector<double> pts;
    QVector<qint64> sampleSizes;
    Codec codec = Codec::Avc;
    QString sampleEntry;
    double fps = 0.0;
    double duration = 0.0;
};

QVector<quint32> table32(const QByteArray &data, const Mp4BoxInfo *box, qint64 firstEntry) {
    QVector<quint32> values; if (!box) return values;
    const qint64 payload = box->offset + box->headerSize; const quint32 count = be32(data, payload + 4);
    values.reserve(int(std::min<quint32>(count, 10000000)));
    for (quint32 i = 0; i < count && payload + firstEntry + qint64(i) * 4 + 4 <= box->offset + box->size; ++i) values << be32(data, payload + firstEntry + qint64(i) * 4);
    return values;
}

Mp4DemuxResult demuxMp4Video(const QByteArray &data, const QVector<Mp4BoxInfo> &boxes) {
    const Mp4BoxInfo *moov = nullptr; for (const auto &box : boxes) if (box.type == "moov") { moov = &box; break; }
    if (!moov) throw std::runtime_error("MP4 moov box not found");
    const Mp4BoxInfo *selectedTrak = nullptr; const Mp4BoxInfo *sampleEntry = nullptr; const Mp4BoxInfo *stbl = nullptr;
    for (const auto &trak : moov->children) {
        if (trak.type != "trak") continue;
        const auto *hdlr = pathChild(&trak, {"mdia","hdlr"});
        const auto *candidateStbl = pathChild(&trak, {"mdia","minf","stbl"});
        const auto *stsd = directChild(candidateStbl, "stsd");
        if (!hdlr || !candidateStbl || !stsd || fourcc(data, hdlr->offset + hdlr->headerSize + 8) != "vide") continue;
        for (const auto &entry : stsd->children) if (QSet<QString>{"avc1","avc3","hvc1","hev1","mp4v","av01"}.contains(entry.type)) { selectedTrak=&trak;sampleEntry=&entry;stbl=candidateStbl;break; }
        if (selectedTrak) break;
    }
    if (!selectedTrak || !sampleEntry || !stbl) throw std::runtime_error("No supported H.264/H.265/MPEG-4 Part 2/AV1 video track in MP4");

    Mp4DemuxResult result; result.sampleEntry = sampleEntry->type;
    result.codec = sampleEntry->type.startsWith("avc") ? Codec::Avc
        : (sampleEntry->type == QStringLiteral("mp4v") ? Codec::Mpeg4Visual
        : (sampleEntry->type == QStringLiteral("av01") ? Codec::Av1 : Codec::Hevc));
    const Mp4BoxInfo *config = directChild(sampleEntry,
        result.codec == Codec::Avc ? QStringLiteral("avcC")
        : (result.codec == Codec::Hevc ? QStringLiteral("hvcC")
        : (result.codec == Codec::Mpeg4Visual ? QStringLiteral("esds") : QStringLiteral("av1C"))));
    if (!config) throw std::runtime_error("MP4 video decoder configuration box is missing");
    const qint64 configPayload = config->offset + config->headerSize; const qint64 configEnd = config->offset + config->size;
    int nalLengthSize = 4;
    if (result.codec == Codec::Avc) {
        if (configPayload + 7 > configEnd) throw std::runtime_error("Invalid avcC box");
        nalLengthSize = (quint8(data.at(configPayload + 4)) & 3) + 1; qint64 pos = configPayload + 6;
        const int spsCount = quint8(data.at(configPayload + 5)) & 31;
        for (int i=0;i<spsCount&&pos+2<=configEnd;++i){const int length=be16(data,pos);pos+=2;if(pos+length>configEnd)break;result.units.push_back({pos,2,data.mid(pos,length),true,-1});pos+=length;}
        if(pos<configEnd){const int ppsCount=quint8(data.at(pos++));for(int i=0;i<ppsCount&&pos+2<=configEnd;++i){const int length=be16(data,pos);pos+=2;if(pos+length>configEnd)break;result.units.push_back({pos,2,data.mid(pos,length),true,-1});pos+=length;}}
    } else if (result.codec == Codec::Hevc) {
        if (configPayload + 23 > configEnd) throw std::runtime_error("Invalid hvcC box");
        nalLengthSize = (quint8(data.at(configPayload + 21)) & 3) + 1; qint64 pos = configPayload + 23; const int arrays = quint8(data.at(configPayload + 22));
        for(int a=0;a<arrays&&pos+3<=configEnd;++a){pos++;const int count=be16(data,pos);pos+=2;for(int i=0;i<count&&pos+2<=configEnd;++i){const int length=be16(data,pos);pos+=2;if(pos+length>configEnd)break;result.units.push_back({pos,2,data.mid(pos,length),true,-1});pos+=length;}}
    } else if (result.codec == Codec::Mpeg4Visual) {
        const auto [elementaryStart, elementaryLength] =
            mpeg4DecoderSpecificInfo(data, configPayload + 4, configEnd);
        if (elementaryStart < 0)
            throw std::runtime_error("Invalid esds box: MPEG-4 Visual decoder configuration is missing");
        auto configUnits = splitUnits(data.mid(elementaryStart, elementaryLength));
        for (auto &unit : configUnits) {
            unit.offset += elementaryStart;
            unit.sampleIndex = -1;
            result.units.push_back(std::move(unit));
        }
    } else {
        if (configPayload + 4 > configEnd)
            throw std::runtime_error("Invalid av1C box");
        auto configUnits = splitAv1Obus(data.mid(configPayload + 4,
                                                  configEnd - configPayload - 4),
                                         configPayload + 4, -1);
        for (auto &unit : configUnits)
            result.units.push_back(std::move(unit));
    }

    const auto *stsz = directChild(stbl,"stsz"); const auto *stsc = directChild(stbl,"stsc"); const auto *stco = directChild(stbl,"stco"); const auto *co64 = directChild(stbl,"co64");
    if (!stsz || !stsc || (!stco && !co64)) throw std::runtime_error("Incomplete MP4 sample table");
    const qint64 sizePayload=stsz->offset+stsz->headerSize;const quint32 fixedSize=be32(data,sizePayload+4),sampleCount=be32(data,sizePayload+8);
    result.sampleSizes.reserve(sampleCount);for(quint32 i=0;i<sampleCount;++i)result.sampleSizes<<qint64(fixedSize?fixedSize:be32(data,sizePayload+12+qint64(i)*4));
    QVector<quint64> chunkOffsets;if(stco){for(quint32 value:table32(data,stco,8))chunkOffsets<<value;}else{const qint64 p=co64->offset+co64->headerSize;const quint32 count=be32(data,p+4);for(quint32 i=0;i<count&&p+8+qint64(i)*8+8<=co64->offset+co64->size;++i)chunkOffsets<<be64(data,p+8+qint64(i)*8);}
    struct StscEntry{quint32 firstChunk;quint32 perChunk;quint32 description;};QVector<StscEntry> mapping;const qint64 scp=stsc->offset+stsc->headerSize;const quint32 scCount=be32(data,scp+4);for(quint32 i=0;i<scCount;++i){const qint64 p=scp+8+qint64(i)*12;if(p+12>stsc->offset+stsc->size)break;mapping.push_back({be32(data,p),be32(data,p+4),be32(data,p+8)});}
    QVector<quint64> sampleOffsets;sampleOffsets.reserve(sampleCount);int sample=0,mapIndex=0;for(int chunk=1;chunk<=chunkOffsets.size()&&sample<int(sampleCount);++chunk){while(mapIndex+1<mapping.size()&&mapping[mapIndex+1].firstChunk<=quint32(chunk))++mapIndex;if(mapping.isEmpty())break;quint64 offset=chunkOffsets.at(chunk-1);for(quint32 j=0;j<mapping[mapIndex].perChunk&&sample<int(sampleCount);++j,++sample){sampleOffsets<<offset;offset+=quint64(result.sampleSizes.at(sample));}}
    if(sampleOffsets.size()!=int(sampleCount))throw std::runtime_error("MP4 sample offsets could not be resolved");

    const auto *stts=directChild(stbl,"stts");QVector<quint32> deltas;if(stts){const qint64 p=stts->offset+stts->headerSize;const quint32 count=be32(data,p+4);for(quint32 i=0;i<count;++i){const qint64 e=p+8+qint64(i)*8;if(e+8>stts->offset+stts->size)break;const quint32 n=be32(data,e),delta=be32(data,e+4);for(quint32 j=0;j<n&&deltas.size()<int(sampleCount);++j)deltas<<delta;}}
    QVector<qint64> composition(sampleCount,0);const auto *ctts=directChild(stbl,"ctts");if(ctts){const qint64 p=ctts->offset+ctts->headerSize;const int version=quint8(data.at(p));const quint32 count=be32(data,p+4);int index=0;for(quint32 i=0;i<count&&index<int(sampleCount);++i){const qint64 e=p+8+qint64(i)*8;if(e+8>ctts->offset+ctts->size)break;const quint32 n=be32(data,e);const quint32 raw=be32(data,e+4);const qint64 value=version==1?qint32(raw):qint64(raw);for(quint32 j=0;j<n&&index<int(sampleCount);++j)composition[index++]=value;}}
    quint32 timescale=0;const auto *mdhd=pathChild(selectedTrak,{"mdia","mdhd"});if(mdhd){const qint64 p=mdhd->offset+mdhd->headerSize;const int version=quint8(data.at(p));timescale=be32(data,p+(version==1?20:12));}
    qint64 mediaStart=0;const auto *elst=pathChild(selectedTrak,{"edts","elst"});if(elst){const qint64 p=elst->offset+elst->headerSize;const int version=quint8(data.at(p));const quint32 count=be32(data,p+4);qint64 entry=p+8;for(quint32 i=0;i<count;++i){if(version==1){if(entry+20>elst->offset+elst->size)break;const qint64 value=qint64(be64(data,entry+8));if(value>=0){mediaStart=value;break;}entry+=20;}else{if(entry+12>elst->offset+elst->size)break;const qint64 value=qint32(be32(data,entry+4));if(value>=0){mediaStart=value;break;}entry+=12;}}}
    quint64 dts=0;result.pts.resize(sampleCount);for(int i=0;i<int(sampleCount);++i){result.pts[i]=timescale?double(qint64(dts)+composition.at(i)-mediaStart)/timescale:double(i)/25.0;dts+=i<deltas.size()?deltas.at(i):1;}result.duration=timescale?double(dts)/timescale:double(sampleCount)/25.0;result.fps=result.duration>0?sampleCount/result.duration:25.0;

    for(int i=0;i<int(sampleCount);++i){
        const qint64 sampleOffset=qint64(sampleOffsets.at(i));
        const qint64 sampleSize=result.sampleSizes.at(i);
        if(result.codec==Codec::Mpeg4Visual){
            auto sampleUnits=splitUnits(data.mid(sampleOffset,sampleSize));
            for(auto &unit:sampleUnits){unit.offset+=sampleOffset;unit.sampleIndex=i;result.units.push_back(std::move(unit));}
            continue;
        }
        if(result.codec==Codec::Av1){
            auto sampleUnits=splitAv1Obus(data.mid(sampleOffset,sampleSize),sampleOffset,i);
            for(auto &unit:sampleUnits)result.units.push_back(std::move(unit));
            continue;
        }
        qint64 pos=sampleOffset;const qint64 end=pos+sampleSize;
        while(pos+nalLengthSize<=end&&pos+nalLengthSize<=data.size()){quint32 length=0;for(int b=0;b<nalLengthSize;++b)length=(length<<8U)|quint8(data.at(pos+b));if(length==0||pos+nalLengthSize+length>end||pos+nalLengthSize+length>data.size())break;result.units.push_back({pos,nalLengthSize,data.mid(pos+nalLengthSize,length),true,i});pos+=nalLengthSize+length;}
    }
    return result;
}

bool looksLikeMp4(const QByteArray &data, const QString &path) {
    return path.endsWith(QStringLiteral(".mp4"),Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".mov"),Qt::CaseInsensitive)
        || (data.size()>=12&&fourcc(data,4)==QStringLiteral("ftyp"));
}

QString avcName(int type) {
    static const QMap<int, QString> names{{1,"Coded slice"},{2,"Slice data partition A"},{3,"Slice data partition B"},
        {4,"Slice data partition C"},{5,"IDR slice"},{6,"SEI"},{7,"SPS"},{8,"PPS"},{9,"Access Unit Delimiter"},
        {10,"End of Sequence"},{11,"End of Stream"},{12,"Filler Data"},{13,"SPS Extension"},{19,"Auxiliary coded picture"}};
    return names.value(type, QStringLiteral("NAL type %1").arg(type));
}

QString hevcName(int type) {
    static const QMap<int, QString> names{{0,"TRAIL_N"},{1,"TRAIL_R"},{2,"TSA_N"},{3,"TSA_R"},{4,"STSA_N"},{5,"STSA_R"},
        {6,"RADL_N"},{7,"RADL_R"},{8,"RASL_N"},{9,"RASL_R"},{16,"BLA_W_LP"},{17,"BLA_W_RADL"},{18,"BLA_N_LP"},
        {19,"IDR_W_RADL"},{20,"IDR_N_LP"},{21,"CRA_NUT"},{32,"VPS"},{33,"SPS"},{34,"PPS"},
        {35,"Access Unit Delimiter"},{36,"End of Sequence"},{37,"End of Bitstream"},{38,"Filler Data"},{39,"Prefix SEI"},{40,"Suffix SEI"}};
    return names.value(type, QStringLiteral("NAL type %1").arg(type));
}

QString mpeg4Name(int code) {
    if (code >= 0x00 && code <= 0x1f)
        return QStringLiteral("Video Object %1").arg(code);
    if (code >= 0x20 && code <= 0x2f)
        return QStringLiteral("Video Object Layer %1").arg(code & 0x0f);
    static const QMap<int, QString> names{
        {0xb0, "Visual Object Sequence"}, {0xb1, "Visual Object Sequence End"},
        {0xb2, "User Data"}, {0xb3, "Group of VOP"},
        {0xb5, "Visual Object"}, {0xb6, "Video Object Plane (VOP)"},
        {0xb7, "Video Packet / Slice"}, {0xb8, "Extension"}
    };
    return names.value(code, QStringLiteral("Start code 0x%1").arg(code, 2, 16, QChar('0')).toUpper());
}

QString mpeg4ProfileName(int indication) {
    const QString hex = QStringLiteral("0x%1").arg(indication, 2, 16, QChar('0')).toUpper();
    static const QMap<int, QString> simpleLevels{
        {0x01, "1"}, {0x02, "2"}, {0x03, "3"}, {0x08, "0"},
        {0x09, "0b"}, {0x0a, "4a"}, {0x0b, "5"}, {0x0c, "6"}
    };
    static const QMap<int, QString> advancedSimpleLevels{
        {0xf0, "0"}, {0xf1, "1"}, {0xf2, "2"}, {0xf3, "3"},
        {0xf4, "4"}, {0xf5, "5"}, {0xf7, "3b"}, {0xf8, "6"}
    };
    if (simpleLevels.contains(indication))
        return QStringLiteral("Simple Profile Level %1 · %2").arg(simpleLevels.value(indication), hex);
    if (advancedSimpleLevels.contains(indication))
        return QStringLiteral("Advanced Simple Profile Level %1 · %2").arg(advancedSimpleLevels.value(indication), hex);
    return QStringLiteral("profile_and_level_indication %1").arg(hex);
}

QString av1Name(int type) {
    static const QMap<int, QString> names{
        {0, "Reserved OBU"}, {1, "Sequence Header OBU"},
        {2, "Temporal Delimiter OBU"}, {3, "Frame Header OBU"},
        {4, "Tile Group OBU"}, {5, "Metadata OBU"}, {6, "Frame OBU"},
        {7, "Redundant Frame Header OBU"}, {8, "Tile List OBU"},
        {15, "Padding OBU"}
    };
    return names.value(type, QStringLiteral("Reserved OBU type %1").arg(type));
}

QString av1ProfileName(int profile, int level) {
    static const QStringList profiles{QStringLiteral("Main Profile"),
                                      QStringLiteral("High Profile"),
                                      QStringLiteral("Professional Profile")};
    QString levelName = QStringLiteral("level_idx %1").arg(level);
    if (level >= 0 && level <= 23)
        levelName = QStringLiteral("Level %1.%2").arg(2 + level / 4).arg(level % 4);
    return QStringLiteral("%1 · %2")
        .arg(profiles.value(profile, QStringLiteral("Profile %1").arg(profile)), levelName);
}

QString av1MetadataName(quint64 type) {
    static const QMap<quint64, QString> names{
        {1, "HDR Content Light Level"}, {2, "HDR Mastering Display Colour Volume"},
        {3, "Scalability"}, {4, "ITU-T T.35"}, {5, "Timecode"}
    };
    if (type >= 6 && type <= 31)
        return QStringLiteral("Unregistered private metadata %1").arg(type);
    return names.value(type, QStringLiteral("Metadata type %1").arg(type));
}

ParameterSetInfo parseAv1SequenceHeader(const QByteArray &obu) {
    ParameterSetInfo sequence;
    sequence.id = 0;
    BitReader br(av1ObuPayload(obu));
    sequence.profile = int(br.readBits(3));
    field(sequence.fields, "seq_profile", sequence.profile, "f(3)");
    const bool stillPicture = br.flag();
    field(sequence.fields, "still_picture", stillPicture, "f(1)");
    sequence.reducedStillPictureHeader = br.flag();
    field(sequence.fields, "reduced_still_picture_header",
          sequence.reducedStillPictureHeader, "f(1)");

    bool timingInfoPresent = false;
    bool initialDisplayDelayPresent = false;
    int bufferDelayLength = 0;
    int operatingPoints = 1;
    if (sequence.reducedStillPictureHeader) {
        sequence.level = int(br.readBits(5));
        field(sequence.fields, "seq_level_idx[0]", sequence.level, "f(5)");
    } else {
        timingInfoPresent = br.flag();
        field(sequence.fields, "timing_info_present_flag", timingInfoPresent, "f(1)");
        if (timingInfoPresent) {
            const quint32 displayTick = br.readBits(32);
            const quint32 timeScale = br.readBits(32);
            field(sequence.fields, "num_units_in_display_tick", displayTick, "f(32)");
            field(sequence.fields, "time_scale", timeScale, "f(32)");
            const bool equalInterval = br.flag();
            field(sequence.fields, "equal_picture_interval", equalInterval, "f(1)");
            if (equalInterval) {
                const quint32 ticksMinusOne = br.ue();
                field(sequence.fields, "num_ticks_per_picture_minus_1", ticksMinusOne, "uvlc()");
                if (displayTick && ticksMinusOne < 1000000)
                    sequence.fps = double(timeScale) /
                        (double(displayTick) * (ticksMinusOne + 1.0));
            }
            sequence.decoderModelInfoPresent = br.flag();
            field(sequence.fields, "decoder_model_info_present_flag",
                  sequence.decoderModelInfoPresent, "f(1)");
            if (sequence.decoderModelInfoPresent) {
                bufferDelayLength = int(br.readBits(5)) + 1;
                field(sequence.fields, "buffer_delay_length_minus_1",
                      bufferDelayLength - 1, "f(5)");
                field(sequence.fields, "num_units_in_decoding_tick", br.readBits(32), "f(32)");
                field(sequence.fields, "buffer_removal_time_length_minus_1", br.readBits(5), "f(5)");
                field(sequence.fields, "frame_presentation_time_length_minus_1", br.readBits(5), "f(5)");
            }
        }
        initialDisplayDelayPresent = br.flag();
        field(sequence.fields, "initial_display_delay_present_flag",
              initialDisplayDelayPresent, "f(1)");
        operatingPoints = int(br.readBits(5)) + 1;
        field(sequence.fields, "operating_points_cnt_minus_1", operatingPoints - 1, "f(5)");
        for (int op = 0; op < operatingPoints; ++op) {
            const int operatingPointIdc = int(br.readBits(12));
            const int level = int(br.readBits(5));
            field(sequence.fields, QStringLiteral("operating_point_idc[%1]").arg(op),
                  operatingPointIdc, "f(12)");
            field(sequence.fields, QStringLiteral("seq_level_idx[%1]").arg(op),
                  level, "f(5)");
            if (op == 0) sequence.level = level;
            if (level > 7)
                field(sequence.fields, QStringLiteral("seq_tier[%1]").arg(op), br.flag(), "f(1)");
            if (sequence.decoderModelInfoPresent) {
                const bool present = br.flag();
                field(sequence.fields,
                      QStringLiteral("decoder_model_present_for_this_op[%1]").arg(op),
                      present, "f(1)");
                if (present) {
                    field(sequence.fields, QStringLiteral("decoder_buffer_delay[%1]").arg(op),
                          br.readBits(bufferDelayLength), QStringLiteral("f(%1)").arg(bufferDelayLength));
                    field(sequence.fields, QStringLiteral("encoder_buffer_delay[%1]").arg(op),
                          br.readBits(bufferDelayLength), QStringLiteral("f(%1)").arg(bufferDelayLength));
                    field(sequence.fields, QStringLiteral("low_delay_mode_flag[%1]").arg(op),
                          br.flag(), "f(1)");
                }
            }
            if (initialDisplayDelayPresent) {
                const bool present = br.flag();
                field(sequence.fields,
                      QStringLiteral("initial_display_delay_present_for_this_op[%1]").arg(op),
                      present, "f(1)");
                if (present)
                    field(sequence.fields,
                          QStringLiteral("initial_display_delay_minus_1[%1]").arg(op),
                          br.readBits(4), "f(4)");
            }
        }
    }

    sequence.frameWidthBits = int(br.readBits(4)) + 1;
    sequence.frameHeightBits = int(br.readBits(4)) + 1;
    field(sequence.fields, "frame_width_bits_minus_1", sequence.frameWidthBits - 1, "f(4)");
    field(sequence.fields, "frame_height_bits_minus_1", sequence.frameHeightBits - 1, "f(4)");
    sequence.width = int(br.readBits(sequence.frameWidthBits)) + 1;
    sequence.height = int(br.readBits(sequence.frameHeightBits)) + 1;
    field(sequence.fields, "max_frame_width_minus_1", sequence.width - 1,
          QStringLiteral("f(%1)").arg(sequence.frameWidthBits));
    field(sequence.fields, "max_frame_height_minus_1", sequence.height - 1,
          QStringLiteral("f(%1)").arg(sequence.frameHeightBits));

    if (!sequence.reducedStillPictureHeader) {
        sequence.frameIdNumbersPresent = br.flag();
        field(sequence.fields, "frame_id_numbers_present_flag",
              sequence.frameIdNumbersPresent, "f(1)");
        if (sequence.frameIdNumbersPresent) {
            sequence.deltaFrameIdLength = int(br.readBits(4)) + 2;
            sequence.additionalFrameIdLength = int(br.readBits(3)) + 1;
            field(sequence.fields, "delta_frame_id_length_minus_2",
                  sequence.deltaFrameIdLength - 2, "f(4)");
            field(sequence.fields, "additional_frame_id_length_minus_1",
                  sequence.additionalFrameIdLength - 1, "f(3)");
        }
    }
    sequence.use128x128Superblock = br.flag();
    sequence.ctbSize = sequence.use128x128Superblock ? 128 : 64;
    sequence.minCodingBlockSize = 4;
    field(sequence.fields, "use_128x128_superblock",
          sequence.use128x128Superblock, "f(1)");
    field(sequence.fields, "enable_filter_intra", br.flag(), "f(1)");
    field(sequence.fields, "enable_intra_edge_filter", br.flag(), "f(1)");
    if (!sequence.reducedStillPictureHeader) {
        field(sequence.fields, "enable_interintra_compound", br.flag(), "f(1)");
        field(sequence.fields, "enable_masked_compound", br.flag(), "f(1)");
        field(sequence.fields, "enable_warped_motion", br.flag(), "f(1)");
        field(sequence.fields, "enable_dual_filter", br.flag(), "f(1)");
        sequence.enableOrderHint = br.flag();
        field(sequence.fields, "enable_order_hint", sequence.enableOrderHint, "f(1)");
        if (sequence.enableOrderHint) {
            field(sequence.fields, "enable_jnt_comp", br.flag(), "f(1)");
            field(sequence.fields, "enable_ref_frame_mvs", br.flag(), "f(1)");
        }
        const bool chooseScreenTools = br.flag();
        field(sequence.fields, "seq_choose_screen_content_tools", chooseScreenTools, "f(1)");
        sequence.forceScreenContentTools = chooseScreenTools ? 2 : int(br.flag());
        if (!chooseScreenTools)
            field(sequence.fields, "seq_force_screen_content_tools",
                  sequence.forceScreenContentTools, "f(1)");
        if (sequence.forceScreenContentTools > 0) {
            const bool chooseIntegerMv = br.flag();
            field(sequence.fields, "seq_choose_integer_mv", chooseIntegerMv, "f(1)");
            sequence.forceIntegerMv = chooseIntegerMv ? 2 : int(br.flag());
            if (!chooseIntegerMv)
                field(sequence.fields, "seq_force_integer_mv", sequence.forceIntegerMv, "f(1)");
        }
        if (sequence.enableOrderHint) {
            sequence.orderHintBits = int(br.readBits(3)) + 1;
            field(sequence.fields, "order_hint_bits_minus_1",
                  sequence.orderHintBits - 1, "f(3)");
        }
    }
    sequence.enableSuperres = br.flag();
    sequence.enableCdef = br.flag();
    sequence.enableRestoration = br.flag();
    field(sequence.fields, "enable_superres", sequence.enableSuperres, "f(1)");
    field(sequence.fields, "enable_cdef", sequence.enableCdef, "f(1)");
    field(sequence.fields, "enable_restoration", sequence.enableRestoration, "f(1)");

    const bool highBitdepth = br.flag();
    bool twelveBit = false;
    if (sequence.profile == 2 && highBitdepth) twelveBit = br.flag();
    sequence.bitDepth = twelveBit ? 12 : (highBitdepth ? 10 : 8);
    field(sequence.fields, "high_bitdepth", highBitdepth, "f(1)");
    if (sequence.profile == 2 && highBitdepth)
        field(sequence.fields, "twelve_bit", twelveBit, "f(1)");
    const bool monochrome = sequence.profile == 1 ? false : br.flag();
    field(sequence.fields, "mono_chrome", monochrome, sequence.profile == 1 ? "derived" : "f(1)");
    const bool colorDescription = br.flag();
    field(sequence.fields, "color_description_present_flag", colorDescription, "f(1)");
    int primaries = 2, transfer = 2, matrix = 2;
    if (colorDescription) {
        primaries = int(br.readBits(8)); transfer = int(br.readBits(8)); matrix = int(br.readBits(8));
        field(sequence.fields, "color_primaries", primaries, "f(8)");
        field(sequence.fields, "transfer_characteristics", transfer, "f(8)");
        field(sequence.fields, "matrix_coefficients", matrix, "f(8)");
    }
    int subsamplingX = 1, subsamplingY = 1;
    if (monochrome) {
        field(sequence.fields, "color_range", br.flag(), "f(1)");
        sequence.chroma = 0;
    } else if (primaries == 1 && transfer == 13 && matrix == 0) {
        sequence.chroma = 3;
        subsamplingX = subsamplingY = 0;
        field(sequence.fields, "color_range", 1, "derived");
    } else {
        field(sequence.fields, "color_range", br.flag(), "f(1)");
        if (sequence.profile == 0) { subsamplingX = 1; subsamplingY = 1; }
        else if (sequence.profile == 1) { subsamplingX = 0; subsamplingY = 0; }
        else if (sequence.bitDepth == 12) {
            subsamplingX = int(br.flag());
            subsamplingY = subsamplingX ? int(br.flag()) : 0;
        } else { subsamplingX = 1; subsamplingY = 0; }
        sequence.chroma = subsamplingX ? (subsamplingY ? 1 : 2) : 3;
        if (subsamplingX && subsamplingY)
            field(sequence.fields, "chroma_sample_position", br.readBits(2), "f(2)");
    }
    field(sequence.fields, "chroma_subsampling_x", subsamplingX, "derived");
    field(sequence.fields, "chroma_subsampling_y", subsamplingY, "derived");
    if (monochrome)
        field(sequence.fields, "separate_uv_delta_q", 0, "derived");
    else
        field(sequence.fields, "separate_uv_delta_q", br.flag(), "f(1)");
    sequence.filmGrainParamsPresent = br.flag();
    field(sequence.fields, "film_grain_params_present",
          sequence.filmGrainParamsPresent, "f(1)");
    field(sequence.fields, "calculated_width", sequence.width, "pixels");
    field(sequence.fields, "calculated_height", sequence.height, "pixels");
    field(sequence.fields, "calculated_bit_depth", sequence.bitDepth, "bits");
    return sequence;
}

SliceInfo parseAv1FrameHeader(const QByteArray &obu, const ParameterSetInfo &sequence) {
    SliceInfo frame;
    frame.firstSlice = true;
    frame.firstMb = 0;
    BitReader br(av1ObuPayload(obu));
    static const QStringList frameTypes{QStringLiteral("KEY"), QStringLiteral("INTER"),
                                        QStringLiteral("INTRA_ONLY"), QStringLiteral("SWITCH")};
    if (sequence.reducedStillPictureHeader) {
        frame.rawType = 0;
        frame.type = QStringLiteral("KEY");
        frame.showFrame = true;
        frame.refreshFrameFlags = 0xff;
        field(frame.fields, "frame_type", QStringLiteral("0 (KEY)"), "derived");
        field(frame.fields, "show_frame", true, "derived");
        field(frame.fields, "refresh_frame_flags", QStringLiteral("0xFF"), "derived");
        return frame;
    }
    frame.showExistingFrame = br.flag();
    field(frame.fields, "show_existing_frame", frame.showExistingFrame, "f(1)");
    if (frame.showExistingFrame) {
        frame.frameToShowMapIndex = int(br.readBits(3));
        frame.type = QStringLiteral("SHOW_EXISTING");
        frame.showFrame = true;
        frame.referenceSlots.push_back(frame.frameToShowMapIndex);
        field(frame.fields, "frame_to_show_map_idx", frame.frameToShowMapIndex, "f(3)");
        return frame;
    }
    frame.rawType = int(br.readBits(2));
    frame.type = frameTypes.value(frame.rawType, QStringLiteral("?"));
    field(frame.fields, "frame_type",
          QStringLiteral("%1 (%2)").arg(frame.rawType).arg(frame.type), "f(2)");
    frame.showFrame = br.flag();
    field(frame.fields, "show_frame", frame.showFrame, "f(1)");
    if (!frame.showFrame)
        field(frame.fields, "showable_frame", br.flag(), "f(1)");
    const bool frameIsIntra = frame.rawType == 0 || frame.rawType == 2;
    bool errorResilient = false;
    if (frame.rawType == 3 || (frame.rawType == 0 && frame.showFrame))
        errorResilient = true;
    else
        errorResilient = br.flag();
    field(frame.fields, "error_resilient_mode", errorResilient,
          (frame.rawType == 3 || (frame.rawType == 0 && frame.showFrame)) ? "derived" : "f(1)");
    field(frame.fields, "disable_cdf_update", br.flag(), "f(1)");
    int allowScreenTools = sequence.forceScreenContentTools;
    if (allowScreenTools == 2) allowScreenTools = int(br.flag());
    field(frame.fields, "allow_screen_content_tools", allowScreenTools,
          sequence.forceScreenContentTools == 2 ? "f(1)" : "derived");
    int forceIntegerMv = 0;
    if (allowScreenTools) {
        if (frame.rawType == 3) forceIntegerMv = 1;
        else if (sequence.forceIntegerMv == 2) forceIntegerMv = int(br.flag());
        else forceIntegerMv = sequence.forceIntegerMv;
    }
    field(frame.fields, "force_integer_mv", forceIntegerMv,
          allowScreenTools && frame.rawType != 3 && sequence.forceIntegerMv == 2 ? "f(1)" : "derived");
    if (sequence.frameIdNumbersPresent) {
        const int bits = sequence.additionalFrameIdLength + sequence.deltaFrameIdLength;
        field(frame.fields, "current_frame_id", br.readBits(bits),
              QStringLiteral("f(%1)").arg(bits));
    }
    const bool sizeOverride = frame.rawType == 3 ? true : br.flag();
    field(frame.fields, "frame_size_override_flag", sizeOverride,
          frame.rawType == 3 ? "derived" : "f(1)");
    if (sequence.enableOrderHint) {
        frame.poc = int(br.readBits(sequence.orderHintBits));
        field(frame.fields, "order_hint", frame.poc,
              QStringLiteral("f(%1)").arg(sequence.orderHintBits));
    }
    if (!(frameIsIntra || errorResilient))
        field(frame.fields, "primary_ref_frame", br.readBits(3), "f(3)");
    if (sequence.decoderModelInfoPresent) {
        field(frame.fields, "remaining_header",
              QStringLiteral("decoder model timing present; advanced fields deferred"),
              "payload");
        return frame;
    }
    if (frame.rawType == 3 || (frame.rawType == 0 && frame.showFrame))
        frame.refreshFrameFlags = 0xff;
    else
        frame.refreshFrameFlags = int(br.readBits(8));
    field(frame.fields, "refresh_frame_flags",
          QStringLiteral("0x%1").arg(frame.refreshFrameFlags, 2, 16, QChar('0')).toUpper(),
          frame.refreshFrameFlags == 0xff && (frame.rawType == 3 || frame.rawType == 0)
              ? "derived" : "f(8)");
    if (!frameIsIntra) {
        bool shortSignaling = false;
        if (sequence.enableOrderHint) {
            shortSignaling = br.flag();
            field(frame.fields, "frame_refs_short_signaling", shortSignaling, "f(1)");
        }
        if (shortSignaling) {
            const int last = int(br.readBits(3));
            const int golden = int(br.readBits(3));
            frame.referenceSlots << last << golden;
            field(frame.fields, "last_frame_idx", last, "f(3)");
            field(frame.fields, "gold_frame_idx", golden, "f(3)");
        } else {
            for (int reference = 0; reference < 7; ++reference) {
                const int slot = int(br.readBits(3));
                if (!frame.referenceSlots.contains(slot)) frame.referenceSlots.push_back(slot);
                field(frame.fields, QStringLiteral("ref_frame_idx[%1]").arg(reference),
                      slot, "f(3)");
                if (sequence.frameIdNumbersPresent) {
                    field(frame.fields,
                          QStringLiteral("delta_frame_id_minus_1[%1]").arg(reference),
                          br.readBits(sequence.deltaFrameIdLength),
                          QStringLiteral("f(%1)").arg(sequence.deltaFrameIdLength));
                }
            }
        }
    }
    return frame;
}

QString seiName(int type) {
    static const QMap<int, QString> names{{0,"Buffering Period"},{1,"Picture Timing"},{2,"Pan-Scan Rectangle"},
        {3,"Filler Payload"},{4,"User Data Registered (ITU-T T.35)"},{5,"User Data Unregistered"},{6,"Recovery Point"},
        {7,"Decoded Reference Picture Marking Repetition"},{8,"Spare Picture"},{9,"Scene Information"},
        {19,"Film Grain Characteristics"},{20,"Deblocking Filter Display Preference"},{21,"Stereo Video Information"},
        {23,"Tone Mapping Information"},{45,"Frame Packing Arrangement"},{47,"Display Orientation"},{56,"Green Metadata"},
        {129,"Active Parameter Sets"},{130,"Decoding Unit Information"},{132,"Decoded Picture Hash"},{133,"Scalable Nesting"},
        {136,"Time Code"},{137,"Mastering Display Colour Volume"},{144,"Content Light Level Information"},
        {147,"Alternative Transfer Characteristics"}};
    return names.value(type, QStringLiteral("SEI Type %1").arg(type));
}

QString sliceName(int raw) {
    static const QStringList names{QStringLiteral("P"),QStringLiteral("B"),QStringLiteral("I"),QStringLiteral("SP"),QStringLiteral("SI")};
    return names.value(raw % 5, QStringLiteral("?"));
}

QVector<RawUnit> splitUnits(const QByteArray &bytes) {
    struct Start { int at; int length; };
    QVector<Start> starts;
    for (int i = 0; i + 3 < bytes.size(); ++i) {
        const auto b0 = quint8(bytes.at(i)), b1 = quint8(bytes.at(i + 1)), b2 = quint8(bytes.at(i + 2));
        if (b0 == 0 && b1 == 0 && b2 == 1) { starts.push_back({i,3}); i += 2; }
        else if (i + 3 < bytes.size() && b0 == 0 && b1 == 0 && b2 == 0 && quint8(bytes.at(i + 3)) == 1) {
            starts.push_back({i,4}); i += 3;
        }
    }
    QVector<RawUnit> out;
    if (!starts.isEmpty()) {
        out.reserve(starts.size());
        for (int i = 0; i < starts.size(); ++i) {
            const int begin = starts[i].at + starts[i].length;
            const int end = i + 1 < starts.size() ? starts[i + 1].at : bytes.size();
            if (end > begin) out.push_back({starts[i].at, starts[i].length, bytes.mid(begin, end - begin), false});
        }
        return out;
    }
    int pos = 0;
    while (pos + 4 <= bytes.size()) {
        const quint32 length = (quint32(quint8(bytes.at(pos))) << 24U) | (quint32(quint8(bytes.at(pos + 1))) << 16U)
            | (quint32(quint8(bytes.at(pos + 2))) << 8U) | quint32(quint8(bytes.at(pos + 3)));
        if (length == 0 || pos + 4 + int(length) > bytes.size()) break;
        out.push_back({pos, 4, bytes.mid(pos + 4, int(length)), true});
        pos += 4 + int(length);
    }
    return out;
}

QVector<RawUnit> splitAv1Obus(const QByteArray &bytes, qint64 baseOffset,
                              int sampleIndex) {
    QVector<RawUnit> out;
    int position = 0;
    while (position < bytes.size()) {
        const int begin = position;
        const quint8 header = quint8(bytes.at(position++));
        if ((header & 0x80) != 0 || (header & 0x01) != 0)
            break;
        const bool extension = (header & 0x04) != 0;
        const bool hasSize = (header & 0x02) != 0;
        if (extension) {
            if (position >= bytes.size()) break;
            ++position;
        }
        quint64 payloadSize = 0;
        if (hasSize) {
            if (!readLeb128(bytes, position, bytes.size(), payloadSize)
                || payloadSize > quint64(bytes.size() - position))
                break;
        } else {
            // Low-overhead OBU streams need a size field when multiple OBUs are
            // concatenated. A size-less OBU can only consume the remaining bytes.
            payloadSize = quint64(bytes.size() - position);
        }
        const qint64 total = qint64(position - begin) + qint64(payloadSize);
        if (total <= 0 || begin + total > bytes.size()) break;
        out.push_back({baseOffset + begin, 0, bytes.mid(begin, int(total)), false,
                       sampleIndex});
        position = begin + int(total);
        if (!hasSize) break;
    }
    return out;
}

QVector<RawUnit> splitIvfAv1(const QByteArray &bytes, double &fps,
                             double &duration) {
    QVector<RawUnit> out;
    if (bytes.size() < 32 || bytes.left(4) != QByteArrayLiteral("DKIF")
        || bytes.mid(8, 4) != QByteArrayLiteral("AV01"))
        return out;
    const int headerSize = std::max(32, int(le16(bytes, 6)));
    const quint32 rate = le32(bytes, 16);
    const quint32 scale = le32(bytes, 20);
    const quint32 declaredFrames = le32(bytes, 24);
    fps = rate && scale ? double(rate) / scale : 0.0;
    QVector<quint64> timestamps;
    int position = headerSize;
    int frameIndex = 0;
    while (position + 12 <= bytes.size()) {
        const quint32 frameSize = le32(bytes, position);
        const quint64 timestamp = le64(bytes, position + 4);
        timestamps.push_back(timestamp);
        const int payload = position + 12;
        if (frameSize == 0 || quint64(payload) + frameSize > quint64(bytes.size())) break;
        auto frameUnits = splitAv1Obus(bytes.mid(payload, int(frameSize)), payload,
                                        frameIndex);
        for (auto &unit : frameUnits) out.push_back(std::move(unit));
        position = payload + int(frameSize);
        ++frameIndex;
    }
    const int frames = declaredFrames > 0 ? int(declaredFrames) : frameIndex;
    quint64 timestampStep = 1;
    QVector<quint64> steps;
    for (int i = 1; i < timestamps.size(); ++i)
        if (timestamps.at(i) > timestamps.at(i - 1))
            steps.push_back(timestamps.at(i) - timestamps.at(i - 1));
    if (!steps.isEmpty()) {
        std::sort(steps.begin(), steps.end());
        timestampStep = steps.at(steps.size() / 2);
        if (rate && scale)
            fps = double(rate) / (double(scale) * timestampStep);
    }
    if (rate && scale && !timestamps.isEmpty())
        duration = double(timestamps.last() - timestamps.first() + timestampStep)
            * scale / rate;
    else
        duration = fps > 0 ? frames / fps : 0.0;
    return out;
}

bool looksLikeAv1ObuStream(const QByteArray &bytes, const QString &path) {
    if (path.endsWith(QStringLiteral(".obu"), Qt::CaseInsensitive)
        || path.endsWith(QStringLiteral(".av1"), Qt::CaseInsensitive))
        return true;
    const auto units = splitAv1Obus(bytes.left(int(std::min<qsizetype>(bytes.size(), 1024 * 1024))));
    if (units.isEmpty()) return false;
    bool sequenceHeader = false;
    for (const auto &unit : units) {
        if (unit.payload.isEmpty()) continue;
        const int type = (quint8(unit.payload.at(0)) >> 3) & 0x0f;
        sequenceHeader |= type == 1;
    }
    return sequenceHeader && (units.size() >= 2 || units.first().payload.size() == bytes.size());
}

Codec detectCodec(const QVector<RawUnit> &units, const QString &path) {
    if (path.contains(QRegularExpression(QStringLiteral("(av1|\\.obu)"), QRegularExpression::CaseInsensitiveOption))) return Codec::Av1;
    if (path.contains(QRegularExpression(QStringLiteral("(mpeg4|mp4v|\\.m4v)"), QRegularExpression::CaseInsensitiveOption))) return Codec::Mpeg4Visual;
    if (path.contains(QRegularExpression(QStringLiteral("(h265|hevc|\\.265)"), QRegularExpression::CaseInsensitiveOption))) return Codec::Hevc;
    if (path.contains(QRegularExpression(QStringLiteral("(h264|avc|\\.264)"), QRegularExpression::CaseInsensitiveOption))) return Codec::Avc;
    int avc = 0, hevc = 0, mpeg4 = 0, av1 = 0;
    for (int i = 0; i < std::min(60, int(units.size())); ++i) {
        if (units[i].payload.isEmpty()) continue;
        const int a = quint8(units[i].payload.at(0)) & 31;
        const int h = (quint8(units[i].payload.at(0)) >> 1) & 63;
        const int m = quint8(units[i].payload.at(0));
        const int o = (quint8(units[i].payload.at(0)) >> 3) & 0x0f;
        if (a > 0 && a <= 23) ++avc;
        if (a == 5 || a == 7 || a == 8) avc += 3;
        if (h == 32 || h == 33 || h == 34 || h == 39 || h == 40) hevc += 4;
        if (m == 0xb0 || m == 0xb3 || m == 0xb5 || m == 0xb6 || (m >= 0x20 && m <= 0x2f)) mpeg4 += 4;
        if ((m & 0x80) == 0 && (m & 0x01) == 0 && (m & 0x02) != 0
            && o >= 1 && o <= 8) av1 += o == 1 ? 5 : 1;
    }
    if (av1 > std::max({avc, hevc, mpeg4})) return Codec::Av1;
    if (mpeg4 > std::max(avc, hevc)) return Codec::Mpeg4Visual;
    return hevc > avc ? Codec::Hevc : Codec::Avc;
}

void skipScalingList(BitReader &br, int size) {
    int last = 8, next = 8;
    for (int i = 0; i < size; ++i) {
        if (next != 0) next = (last + br.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

ParameterSetInfo parseAvcSps(const QByteArray &payload) {
    ParameterSetInfo sps;
    BitReader br(rbsp(payload, 1));
    const int profile = int(br.readBits(8)); sps.profile = profile; field(sps.fields,"profile_idc",profile,"u(8)");
    field(sps.fields,"constraint_set_flags",QStringLiteral("0x%1").arg(br.readBits(8),2,16,QChar('0')).toUpper(),"u(8)");
    sps.level = int(br.readBits(8)); field(sps.fields,"level_idc",sps.level,"u(8)");
    sps.id = int(br.ue()); field(sps.fields,"seq_parameter_set_id",sps.id,"ue(v)");
    int chroma = 1;
    if (QSet<int>{100,110,122,244,44,83,86,118,128,138,139,134,135}.contains(profile)) {
        chroma = int(br.ue()); sps.chroma = chroma; field(sps.fields,"chroma_format_idc",chroma,"ue(v)");
        if (chroma == 3) field(sps.fields,"separate_colour_plane_flag",br.flag(),"u(1)");
        sps.bitDepth = int(br.ue()) + 8; field(sps.fields,"bit_depth_luma_minus8",sps.bitDepth - 8,"ue(v)");
        field(sps.fields,"bit_depth_chroma_minus8",int(br.ue()),"ue(v)");
        field(sps.fields,"qpprime_y_zero_transform_bypass_flag",br.flag(),"u(1)");
        const bool scaling = br.flag(); field(sps.fields,"seq_scaling_matrix_present_flag",scaling,"u(1)");
        if (scaling) for (int i = 0; i < (chroma == 3 ? 12 : 8); ++i) if (br.flag()) skipScalingList(br, i < 6 ? 16 : 64);
    }
    sps.log2FrameNum = int(br.ue()) + 4; field(sps.fields,"log2_max_frame_num_minus4",sps.log2FrameNum - 4,"ue(v)");
    sps.pocType = int(br.ue()); field(sps.fields,"pic_order_cnt_type",sps.pocType,"ue(v)");
    if (sps.pocType == 0) { sps.log2Poc = int(br.ue()) + 4; field(sps.fields,"log2_max_pic_order_cnt_lsb_minus4",sps.log2Poc - 4,"ue(v)"); }
    else if (sps.pocType == 1) {
        br.flag(); br.se(); br.se(); const int count = int(br.ue()); for (int i = 0; i < count; ++i) br.se();
    }
    sps.maxReferences = int(br.ue()); field(sps.fields,"max_num_ref_frames",sps.maxReferences,"ue(v)"); br.flag();
    const int widthMbs = int(br.ue()) + 1, heightMap = int(br.ue()) + 1;
    field(sps.fields,"pic_width_in_mbs_minus1",widthMbs - 1,"ue(v)"); field(sps.fields,"pic_height_in_map_units_minus1",heightMap - 1,"ue(v)");
    sps.frameOnly = br.flag(); field(sps.fields,"frame_mbs_only_flag",sps.frameOnly,"u(1)"); if (!sps.frameOnly) br.flag();
    br.flag(); const bool crop = br.flag(); field(sps.fields,"frame_cropping_flag",crop,"u(1)");
    int left=0,right=0,top=0,bottom=0;
    if (crop) { left=int(br.ue());right=int(br.ue());top=int(br.ue());bottom=int(br.ue());field(sps.fields,"frame_crop_left_offset",left,"ue(v)");field(sps.fields,"frame_crop_right_offset",right,"ue(v)");field(sps.fields,"frame_crop_top_offset",top,"ue(v)");field(sps.fields,"frame_crop_bottom_offset",bottom,"ue(v)"); }
    const int subW = chroma == 3 ? 1 : 2, subH = chroma == 1 ? 2 : 1;
    const int cropX = chroma == 0 ? 1 : subW, cropY = (chroma == 0 ? 1 : subH) * (2 - int(sps.frameOnly));
    sps.width = widthMbs * 16 - (left + right) * cropX;
    sps.height = (2 - int(sps.frameOnly)) * heightMap * 16 - (top + bottom) * cropY;
    field(sps.fields,"calculated_width",sps.width,"pixels"); field(sps.fields,"calculated_height",sps.height,"pixels");
    const bool vui = br.flag(); field(sps.fields,"vui_parameters_present_flag",vui,"u(1)");
    if (vui) {
        if (br.flag()) { const int ar=int(br.readBits(8)); if (ar==255) { br.readBits(16);br.readBits(16); } }
        if (br.flag()) br.flag();
        if (br.flag()) { br.readBits(3);br.flag();if(br.flag()){br.readBits(8);br.readBits(8);br.readBits(8);} }
        if (br.flag()) { br.ue();br.ue(); }
        const bool timing=br.flag();field(sps.fields,"timing_info_present_flag",timing,"u(1)");
        if (timing) { const quint32 units=br.readBits(32),scale=br.readBits(32);const bool fixed=br.flag();field(sps.fields,"num_units_in_tick",units,"u(32)");field(sps.fields,"time_scale",scale,"u(32)");field(sps.fields,"fixed_frame_rate_flag",fixed,"u(1)");if(units)sps.fps=double(scale)/(2.0*units); }
    }
    return sps;
}

ParameterSetInfo parseAvcPps(const QByteArray &payload) {
    ParameterSetInfo pps; BitReader br(rbsp(payload,1));
    pps.id=int(br.ue());pps.parentId=int(br.ue());field(pps.fields,"pic_parameter_set_id",pps.id,"ue(v)");field(pps.fields,"seq_parameter_set_id",pps.parentId,"ue(v)");
    pps.entropyCoding=br.flag();field(pps.fields,"entropy_coding_mode_flag",pps.entropyCoding,"u(1)");br.flag();
    pps.sliceGroups=int(br.ue());field(pps.fields,"num_slice_groups_minus1",pps.sliceGroups,"ue(v)");
    if (pps.sliceGroups > 0) return pps;
    pps.defaultL0=int(br.ue())+1;pps.defaultL1=int(br.ue())+1;field(pps.fields,"num_ref_idx_l0_default_active_minus1",pps.defaultL0-1,"ue(v)");field(pps.fields,"num_ref_idx_l1_default_active_minus1",pps.defaultL1-1,"ue(v)");
    br.flag();br.readBits(2);pps.initQp=int(br.se());field(pps.fields,"pic_init_qp_minus26",pps.initQp,"se(v)");br.se();br.se();
    field(pps.fields,"deblocking_filter_control_present_flag",br.flag(),"u(1)");field(pps.fields,"constrained_intra_pred_flag",br.flag(),"u(1)");field(pps.fields,"redundant_pic_cnt_present_flag",br.flag(),"u(1)");
    return pps;
}

ParameterSetInfo parseMpeg4Vol(const QByteArray &payload, int profileLevel) {
    ParameterSetInfo vol;
    vol.id = payload.isEmpty() ? 0 : (quint8(payload.at(0)) & 0x0f);
    vol.parentId = -1;
    vol.profile = profileLevel;
    vol.level = profileLevel & 0x0f;
    vol.maxReferences = 2;
    vol.ctbSize = 16;
    vol.minCodingBlockSize = 16;
    BitReader br(payload.mid(1));
    const bool randomAccessible = br.flag();
    const int objectType = int(br.readBits(8));
    field(vol.fields, "random_accessible_vol", randomAccessible, "u(1)");
    field(vol.fields, "video_object_type_indication", objectType, "u(8)");
    int versionId = 1;
    const bool layerIdentifier = br.flag();
    field(vol.fields, "is_object_layer_identifier", layerIdentifier, "u(1)");
    if (layerIdentifier) {
        versionId = int(br.readBits(4));
        const int priority = int(br.readBits(3));
        field(vol.fields, "video_object_layer_verid", versionId, "u(4)");
        field(vol.fields, "video_object_layer_priority", priority, "u(3)");
    }
    const int aspectRatio = int(br.readBits(4));
    field(vol.fields, "aspect_ratio_info", aspectRatio, "u(4)");
    if (aspectRatio == 15) {
        field(vol.fields, "par_width", br.readBits(8), "u(8)");
        field(vol.fields, "par_height", br.readBits(8), "u(8)");
    }
    const bool volControl = br.flag();
    field(vol.fields, "vol_control_parameters", volControl, "u(1)");
    if (volControl) {
        vol.chroma = int(br.readBits(2));
        const bool lowDelay = br.flag();
        const bool vbv = br.flag();
        field(vol.fields, "chroma_format", vol.chroma, "u(2)");
        field(vol.fields, "low_delay", lowDelay, "u(1)");
        field(vol.fields, "vbv_parameters", vbv, "u(1)");
        if (vbv) {
            br.readBits(15); br.flag(); br.readBits(15); br.flag();
            br.readBits(15); br.flag(); br.readBits(3); br.readBits(11);
            br.flag(); br.readBits(15); br.flag();
        }
    } else {
        vol.chroma = 1;
    }
    vol.shape = int(br.readBits(2));
    field(vol.fields, "video_object_layer_shape", vol.shape, "u(2)");
    if (vol.shape == 3 && versionId != 1)
        field(vol.fields, "video_object_layer_shape_extension", br.readBits(4), "u(4)");
    field(vol.fields, "marker_bit_before_time_increment_resolution", br.flag(), "u(1)");
    vol.timeIncrementResolution = int(br.readBits(16));
    vol.timeIncrementBits = std::max(1, int(std::ceil(std::log2(std::max(2, vol.timeIncrementResolution)))));
    field(vol.fields, "vop_time_increment_resolution", vol.timeIncrementResolution, "u(16)");
    field(vol.fields, "marker_bit_after_time_increment_resolution", br.flag(), "u(1)");
    const bool fixedRate = br.flag();
    field(vol.fields, "fixed_vop_rate", fixedRate, "u(1)");
    if (fixedRate) {
        const int increment = int(br.readBits(vol.timeIncrementBits));
        field(vol.fields, "fixed_vop_time_increment", increment,
              QStringLiteral("u(%1)").arg(vol.timeIncrementBits));
        if (increment > 0)
            vol.fps = double(vol.timeIncrementResolution) / increment;
    }
    if (vol.shape == 0) {
        field(vol.fields, "marker_bit_before_width", br.flag(), "u(1)");
        vol.width = int(br.readBits(13));
        field(vol.fields, "video_object_layer_width", vol.width, "u(13)");
        field(vol.fields, "marker_bit_before_height", br.flag(), "u(1)");
        vol.height = int(br.readBits(13));
        field(vol.fields, "video_object_layer_height", vol.height, "u(13)");
        field(vol.fields, "marker_bit_after_height", br.flag(), "u(1)");
    }
    vol.interlaced = br.flag();
    vol.frameOnly = !vol.interlaced;
    field(vol.fields, "interlaced", vol.interlaced, "u(1)");
    field(vol.fields, "obmc_disable", br.flag(), "u(1)");
    const int spriteUsage = versionId == 1 ? int(br.readBits(1)) : int(br.readBits(2));
    field(vol.fields, "sprite_enable", spriteUsage, versionId == 1 ? "u(1)" : "u(2)");
    if (spriteUsage == 0 && br.bitsLeft() > 0) {
        const bool not8Bit = br.flag();
        field(vol.fields, "not_8_bit", not8Bit, "u(1)");
        if (not8Bit) {
            vol.quantPrecision = int(br.readBits(4));
            vol.bitDepth = int(br.readBits(4));
        } else {
            vol.quantPrecision = 5;
            vol.bitDepth = 8;
        }
        field(vol.fields, "quant_precision", vol.quantPrecision,
              not8Bit ? QStringLiteral("u(4)") : QStringLiteral("default"));
        field(vol.fields, "bits_per_pixel", vol.bitDepth,
              not8Bit ? QStringLiteral("u(4)") : QStringLiteral("default"));
        if (br.bitsLeft() > 0)
            field(vol.fields, "vol_quant_type", br.flag(), "u(1)");
    }
    field(vol.fields, "calculated_width", vol.width, "pixels");
    field(vol.fields, "calculated_height", vol.height, "pixels");
    return vol;
}

SliceInfo parseMpeg4Vop(const NalUnit &unit, const ParameterSetInfo &vol) {
    SliceInfo vop;
    vop.firstMb = 0;
    vop.firstSlice = true;
    BitReader br(unit.payload.mid(1));
    vop.rawType = int(br.readBits(2));
    static const QStringList types{QStringLiteral("I"), QStringLiteral("P"),
                                   QStringLiteral("B"), QStringLiteral("S")};
    vop.type = types.value(vop.rawType, QStringLiteral("?"));
    field(vop.fields, "vop_coding_type",
          QStringLiteral("%1 (%2)").arg(vop.rawType).arg(vop.type), "u(2)");
    int moduloTimeBase = 0;
    while (br.bitsLeft() > 0 && br.flag())
        ++moduloTimeBase;
    field(vop.fields, "modulo_time_base", moduloTimeBase, "unary");
    field(vop.fields, "marker_bit_before_vop_time_increment", br.flag(), "u(1)");
    vop.timeIncrement = int(br.readBits(std::max(1, vol.timeIncrementBits)));
    field(vop.fields, "vop_time_increment", vop.timeIncrement,
          QStringLiteral("u(%1)").arg(std::max(1, vol.timeIncrementBits)));
    field(vop.fields, "marker_bit_after_vop_time_increment", br.flag(), "u(1)");
    vop.coded = br.flag();
    field(vop.fields, "vop_coded", vop.coded, "u(1)");
    if (!vop.coded)
        return vop;
    if (vop.type == QStringLiteral("P"))
        field(vop.fields, "vop_rounding_type", br.flag(), "u(1)");
    field(vop.fields, "intra_dc_vlc_thr", br.readBits(3), "u(3)");
    if (vol.interlaced) {
        field(vop.fields, "top_field_first", br.flag(), "u(1)");
        field(vop.fields, "alternate_vertical_scan_flag", br.flag(), "u(1)");
    }
    vop.quantizer = int(br.readBits(std::clamp(vol.quantPrecision, 3, 9)));
    field(vop.fields, "vop_quant", vop.quantizer,
          QStringLiteral("u(%1)").arg(std::clamp(vol.quantPrecision, 3, 9)));
    if (vop.type != QStringLiteral("I")) {
        const int forward = int(br.readBits(3));
        field(vop.fields, "vop_fcode_forward", forward, "u(3)");
        vop.numRefL0 = 1;
    }
    if (vop.type == QStringLiteral("B")) {
        field(vop.fields, "vop_fcode_backward", br.readBits(3), "u(3)");
        vop.numRefL1 = 1;
    }
    return vop;
}

SliceInfo parseAvcSlice(const NalUnit &nal, const QMap<int,ParameterSetInfo> &spsMap, const QMap<int,ParameterSetInfo> &ppsMap) {
    SliceInfo s; BitReader br(rbsp(nal.payload,1));
    s.firstMb=int(br.ue());s.rawType=int(br.ue());s.type=sliceName(s.rawType);s.ppsId=int(br.ue());
    field(s.fields,"first_mb_in_slice",s.firstMb,"ue(v)");field(s.fields,"slice_type",QStringLiteral("%1 (%2)").arg(s.rawType).arg(s.type),"ue(v)");field(s.fields,"pic_parameter_set_id",s.ppsId,"ue(v)");
    const auto pps=ppsMap.value(s.ppsId);const auto sps=spsMap.value(pps.parentId);
    if (sps.id >= 0) {
        s.frameNum=int(br.readBits(sps.log2FrameNum));field(s.fields,"frame_num",s.frameNum,QStringLiteral("u(%1)").arg(sps.log2FrameNum));
        if (!sps.frameOnly) { const bool fieldPic=br.flag();if(fieldPic)br.flag(); }
        if (nal.nalType==5) { const int idr=int(br.ue());field(s.fields,"idr_pic_id",idr,"ue(v)"); }
        if (sps.pocType==0) { s.poc=int(br.readBits(sps.log2Poc));field(s.fields,"pic_order_cnt_lsb",s.poc,QStringLiteral("u(%1)").arg(sps.log2Poc)); }
        if (s.type=="B") field(s.fields,"direct_spatial_mv_pred_flag",br.flag(),"u(1)");
        if (s.type=="P"||s.type=="SP"||s.type=="B") {
            s.numRefL0=std::max(1,pps.defaultL0);if(s.type=="B")s.numRefL1=std::max(1,pps.defaultL1);
            const bool overrideRefs=br.flag();field(s.fields,"num_ref_idx_active_override_flag",overrideRefs,"u(1)");
            if (overrideRefs) { s.numRefL0=int(br.ue())+1;field(s.fields,"num_ref_idx_l0_active_minus1",s.numRefL0-1,"ue(v)");if(s.type=="B"){s.numRefL1=int(br.ue())+1;field(s.fields,"num_ref_idx_l1_active_minus1",s.numRefL1-1,"ue(v)");} }
        }
    }
    // QP lies near the end of the complete slice header. A conservative scan is intentionally avoided here.
    return s;
}

void skipProfileTierLevel(BitReader &br, int maxSubLayers, ParameterSetInfo &sps) {
    br.readBits(2);br.flag();sps.profile=int(br.readBits(5));field(sps.fields,"general_profile_idc",sps.profile,"u(5)");br.skip(32);br.skip(48);sps.level=int(br.readBits(8));field(sps.fields,"general_level_idc",sps.level,"u(8)");
    QVector<bool> pp(maxSubLayers),lp(maxSubLayers);for(int i=0;i<maxSubLayers;++i){pp[i]=br.flag();lp[i]=br.flag();}if(maxSubLayers)for(int i=maxSubLayers;i<8;++i)br.skip(2);for(int i=0;i<maxSubLayers;++i){if(pp[i])br.skip(88);if(lp[i])br.skip(8);}
}

ParameterSetInfo parseHevcSps(const QByteArray &payload) {
    ParameterSetInfo sps;BitReader br(rbsp(payload,2));const int vps=int(br.readBits(4));const int maxSub=int(br.readBits(3));br.flag();field(sps.fields,"sps_video_parameter_set_id",vps,"u(4)");skipProfileTierLevel(br,maxSub,sps);
    sps.id=int(br.ue());field(sps.fields,"sps_seq_parameter_set_id",sps.id,"ue(v)");sps.chroma=int(br.ue());field(sps.fields,"chroma_format_idc",sps.chroma,"ue(v)");if(sps.chroma==3)br.flag();
    int width=int(br.ue()),height=int(br.ue());field(sps.fields,"pic_width_in_luma_samples",width,"ue(v)");field(sps.fields,"pic_height_in_luma_samples",height,"ue(v)");
    if(br.flag()){const int l=int(br.ue()),r=int(br.ue()),t=int(br.ue()),b=int(br.ue()),sw=(sps.chroma==1||sps.chroma==2)?2:1,sh=sps.chroma==1?2:1;width-=(l+r)*sw;height-=(t+b)*sh;}
    sps.width=width;sps.height=height;sps.bitDepth=int(br.ue())+8;br.ue();sps.log2Poc=int(br.ue())+4;
    const bool ordering=br.flag();for(int i=ordering?0:maxSub;i<=maxSub;++i){sps.maxReferences=int(br.ue())+1;br.ue();br.ue();}
    const int log2Min=int(br.ue())+3,diff=int(br.ue()),log2Ctb=log2Min+diff;
    sps.minCodingBlockSize=1<<log2Min;sps.ctbSize=1<<log2Ctb;br.ue();br.ue();br.ue();br.ue();
    const int count=int(std::ceil(double(width)/sps.ctbSize)*std::ceil(double(height)/sps.ctbSize));sps.sliceAddressBits=int(std::ceil(std::log2(std::max(1,count))));
    field(sps.fields,"min_coding_block_size",sps.minCodingBlockSize,"pixels","SPS 允许的最小亮度 CU 边长");
    field(sps.fields,"ctb_size",sps.ctbSize,"pixels","最大编码树单元 CTU 边长");field(sps.fields,"ctb_count",count,"units");return sps;
}

ParameterSetInfo parseHevcPps(const QByteArray &payload) {
    ParameterSetInfo pps;BitReader br(rbsp(payload,2));pps.id=int(br.ue());pps.parentId=int(br.ue());field(pps.fields,"pps_pic_parameter_set_id",pps.id,"ue(v)");field(pps.fields,"pps_seq_parameter_set_id",pps.parentId,"ue(v)");field(pps.fields,"dependent_slice_segments_enabled_flag",br.flag(),"u(1)");field(pps.fields,"output_flag_present_flag",br.flag(),"u(1)");field(pps.fields,"num_extra_slice_header_bits",br.readBits(3),"u(3)");return pps;
}

SliceInfo parseHevcSlice(const NalUnit &nal, const QMap<int,ParameterSetInfo> &spsMap, const QMap<int,ParameterSetInfo> &ppsMap) {
    SliceInfo s;BitReader br(rbsp(nal.payload,2));s.firstSlice=br.flag();field(s.fields,"first_slice_segment_in_pic_flag",s.firstSlice,"u(1)");if(nal.nalType>=16&&nal.nalType<=23)br.flag();s.ppsId=int(br.ue());field(s.fields,"slice_pic_parameter_set_id",s.ppsId,"ue(v)");const auto pps=ppsMap.value(s.ppsId),sps=spsMap.value(pps.parentId);
    if(!s.firstSlice&&sps.sliceAddressBits>0){s.address=int(br.readBits(sps.sliceAddressBits));field(s.fields,"slice_segment_address",s.address,QStringLiteral("u(%1)").arg(sps.sliceAddressBits));}
    if(nal.nalType>=16&&nal.nalType<=21){s.type="I";s.rawType=2;}else{s.rawType=int(br.ue());s.type=sliceName(s.rawType);field(s.fields,"slice_type",QStringLiteral("%1 (%2)").arg(s.rawType).arg(s.type),"ue(v)");if(s.type=="P"||s.type=="B"){s.numRefL0=1;if(s.type=="B")s.numRefL1=1;}}
    return s;
}

QVector<SeiMessage> parseSei(const QByteArray &payload, Codec codec) {
    const QByteArray data=rbsp(payload,codec==Codec::Avc?1:2);QVector<SeiMessage> messages;int pos=0;
    while(pos<data.size()){
        if(pos==data.size()-1&&quint8(data.at(pos))==0x80)break;int type=0,size=0;while(pos<data.size()&&quint8(data.at(pos))==0xff){type+=255;++pos;}if(pos>=data.size())break;type+=quint8(data.at(pos++));while(pos<data.size()&&quint8(data.at(pos))==0xff){size+=255;++pos;}if(pos>=data.size())break;size+=quint8(data.at(pos++));const QByteArray body=data.mid(pos,std::min(size,int(data.size()-pos)));pos+=size;
        SeiMessage msg;msg.type=type;msg.name=seiName(type);msg.payloadSize=size;field(msg.fields,"payload_type",type,"bytes",msg.name);field(msg.fields,"payload_size",size,"bytes");
        try{if(type==6&&codec==Codec::Avc){BitReader br(body);msg.recoveryFrameCount=int(br.ue());msg.exactMatch=br.flag();msg.brokenLink=br.flag();const int changing=int(br.readBits(2));field(msg.fields,"recovery_frame_cnt",msg.recoveryFrameCount,"ue(v)");field(msg.fields,"exact_match_flag",msg.exactMatch,"u(1)");field(msg.fields,"broken_link_flag",msg.brokenLink,"u(1)");field(msg.fields,"changing_slice_group_idc",changing,"u(2)");}else{field(msg.fields,"payload_hex",QString::fromLatin1(body.left(64).toHex(' ')),"bytes");}}catch(...){field(msg.fields,"parse_status","payload truncated");}
        messages.push_back(msg);if(pos>data.size())break;
    }
    return messages;
}

bool isVcl(Codec codec,int type){
    if(codec==Codec::Avc)return type>=1&&type<=5;
    if(codec==Codec::Hevc)return type<=31;
    if(codec==Codec::Mpeg4Visual)return type==0xb6;
    return type==3||type==6;
}
bool isParameter(Codec codec,int type){
    if(codec==Codec::Avc)return type==7||type==8||type==13;
    if(codec==Codec::Hevc)return type==32||type==33||type==34;
    if(codec==Codec::Mpeg4Visual)return type==0xb0||type==0xb5||(type>=0x00&&type<=0x2f);
    return type==1;
}

void analyzeFrames(StreamInfo &stream) {
    QVector<int> dpb;QVector<int> av1Slots(8,-1);const int maxReferences=std::max(1,stream.sps.value(stream.primarySpsId).maxReferences);const int unitSize=(stream.codec==Codec::Hevc||stream.codec==Codec::Av1)?stream.sps.value(stream.primarySpsId).ctbSize:16;
    const int cols=stream.width>0?int(std::ceil(double(stream.width)/unitSize)):0,rows=stream.height>0?int(std::ceil(double(stream.height)/unitSize)):0,total=cols*rows;
    for(auto &frame:stream.frames){
        QSet<QString> types;frame.randomAccess=false;frame.referencePicture=false;if(stream.codec!=Codec::Mpeg4Visual)frame.poc=-1;
        for(int ni:frame.sliceNalIndices){const auto &nal=stream.nals.at(ni);types.insert(nal.slice.type);if(stream.codec==Codec::Avc){frame.randomAccess|=nal.nalType==5;frame.referencePicture|=nal.refIdc>0;}else if(stream.codec==Codec::Hevc){frame.randomAccess|=nal.nalType>=16&&nal.nalType<=21;frame.referencePicture|=(nal.nalType&1)!=0||nal.nalType>=16;}else if(stream.codec==Codec::Mpeg4Visual){frame.randomAccess|=nal.slice.type==QStringLiteral("I");frame.referencePicture|=nal.slice.type==QStringLiteral("I")||nal.slice.type==QStringLiteral("P")||nal.slice.type==QStringLiteral("S");}else{frame.randomAccess|=nal.slice.type==QStringLiteral("KEY")&&nal.slice.showFrame;frame.referencePicture|=nal.slice.refreshFrameFlags!=0;}if(frame.poc<0||stream.codec==Codec::Av1&&nal.slice.showFrame&&nal.slice.poc>=0)frame.poc=nal.slice.poc;}
        if(stream.codec==Codec::Av1&&frame.type==QStringLiteral("SHOW_EXISTING"))frame.poc=-1;
        QStringList typeList(types.begin(),types.end());std::sort(typeList.begin(),typeList.end());frame.displayType=frame.randomAccess?(stream.codec==Codec::Avc?"IDR":(stream.codec==Codec::Hevc?"IRAP":(stream.codec==Codec::Av1?"KEY":"I"))):typeList.join('/');if(frame.displayType.isEmpty())frame.displayType=frame.type;
        frame.truncatedStart=(stream.codec==Codec::Mpeg4Visual||stream.codec==Codec::Av1)?false:(!frame.sliceNalIndices.isEmpty()&&(stream.codec==Codec::Avc?stream.nals.at(frame.sliceNalIndices.first()).slice.firstMb!=0:!stream.nals.at(frame.sliceNalIndices.first()).slice.firstSlice));
        frame.structure=stream.codec==Codec::Mpeg4Visual?QStringLiteral("完整 VOP"):(stream.codec==Codec::Av1?(std::any_of(frame.sliceNalIndices.begin(),frame.sliceNalIndices.end(),[&stream](int index){return stream.nals.at(index).nalType==6;})?QStringLiteral("Frame OBU"):QStringLiteral("Frame Header + Tile Group")):(frame.truncatedStart?QStringLiteral("起始分片缺失"):(frame.sliceNalIndices.size()>1?QStringLiteral("分片帧 ×%1").arg(frame.sliceNalIndices.size()):QStringLiteral("完整帧"))));
        for(int ni:frame.nalIndices)for(const auto &sei:stream.nals.at(ni).sei)if(sei.type==6){frame.recoveryPoint=true;frame.recoveryFrameCount=sei.recoveryFrameCount;}
        const bool idr=std::any_of(frame.sliceNalIndices.begin(),frame.sliceNalIndices.end(),[&stream](int index){const auto &unit=stream.nals.at(index);return stream.codec==Codec::Avc?unit.nalType==5:(stream.codec==Codec::Hevc&&(unit.nalType==19||unit.nalType==20));});
        if(idr)dpb.clear();
        int l0=0,l1=0;for(int ni:frame.sliceNalIndices){const auto &slice=stream.nals.at(ni).slice;l0=std::max(l0,slice.numRefL0);l1=std::max(l1,slice.numRefL1);}
        auto appendReference=[&frame](int index){if(index>=0&&index!=frame.index&&!frame.references.contains(index))frame.references.push_back(index);};
        if(stream.codec==Codec::Av1){
            for(int ni:frame.sliceNalIndices){const auto &header=stream.nals.at(ni).slice;for(int slot:header.referenceSlots)if(slot>=0&&slot<av1Slots.size())appendReference(av1Slots.at(slot));for(int slot=0;slot<8;++slot)if((header.refreshFrameFlags&(1<<slot))!=0)av1Slots[slot]=frame.index;}
        }else if(types.contains("B")){
            QVector<int> lower,higher;for(int ref:dpb){const int refPoc=stream.frames.at(ref).poc;if(frame.poc>=0&&refPoc>=0&&(refPoc>frame.poc))higher.push_back(ref);else lower.push_back(ref);}
            if(stream.codec==Codec::Mpeg4Visual){lower.clear();higher.clear();for(int ref:dpb){if(stream.frames.at(ref).pts>frame.pts+0.000001)higher.push_back(ref);else lower.push_back(ref);}if(higher.isEmpty()&&dpb.size()>=2){lower={dpb.at(dpb.size()-2)};higher={dpb.last()};}}
            std::sort(lower.begin(),lower.end(),[&stream](int a,int b){return stream.frames.at(a).poc>stream.frames.at(b).poc;});
            std::sort(higher.begin(),higher.end(),[&stream](int a,int b){return stream.frames.at(a).poc<stream.frames.at(b).poc;});
            QVector<int> l0Candidates=lower;l0Candidates+=higher;for(int i=0;i<std::min(l0,int(l0Candidates.size()));++i)appendReference(l0Candidates.at(i));for(int i=0;i<std::min(l1,int(higher.size()));++i)appendReference(higher.at(i));
        }else if(types.contains("P")||types.contains("SP")){
            for(int i=int(dpb.size())-1,count=0;i>=0&&count<l0;--i,++count)appendReference(dpb.at(i));
        }
        if(stream.codec!=Codec::Av1&&frame.referencePicture){dpb.push_back(frame.index);while(dpb.size()>maxReferences)dpb.removeFirst();}
        frame.unitSize=unitSize;frame.unitColumns=cols;frame.unitRows=rows;
        QVector<QPair<int,int>> starts;for(int i=0;i<frame.sliceNalIndices.size();++i){const auto &s=stream.nals.at(frame.sliceNalIndices[i]).slice;starts.push_back({stream.codec==Codec::Hevc?s.address:s.firstMb,i});}
        std::sort(starts.begin(),starts.end());for(int i=0;i<starts.size();++i){const int start=std::clamp(starts[i].first,0,total),end=std::clamp(i+1<starts.size()?starts[i+1].first:total,start,total),sliceIndex=starts[i].second;const auto &slice=stream.nals.at(frame.sliceNalIndices[sliceIndex]).slice;frame.regions.push_back({sliceIndex,start,end,slice.type,-1});}
    }
    for(const auto &frame:stream.frames)for(int ref:frame.references)if(ref>=0&&ref<stream.frames.size())stream.frames[ref].referencedBy.push_back(frame.index);
}

} // namespace

StreamPtr BitstreamParser::parseFile(const QString &path) {
    auto stream=std::make_shared<StreamInfo>();stream->filePath=path;stream->fileName=QFileInfo(path).fileName();
    QFile file(path);if(!file.open(QIODevice::ReadOnly)){stream->error=file.errorString();return stream;}stream->bytes=file.readAll();if(stream->bytes.isEmpty()){stream->error=QStringLiteral("文件为空");return stream;}
    try{
        QVector<RawUnit> raw;Mp4DemuxResult mp4;double elementaryFps=0.0,elementaryDuration=0.0;bool ivfAv1=false;
        if(looksLikeMp4(stream->bytes,path)){
            stream->containerFile=true;stream->boxes=parseBoxRange(stream->bytes,0,stream->bytes.size());mp4=demuxMp4Video(stream->bytes,stream->boxes);raw=mp4.units;stream->codec=mp4.codec;stream->format=QStringLiteral("ISO BMFF / MP4 (%1)").arg(mp4.sampleEntry);
        }else{
            ivfAv1=stream->bytes.size()>=32&&stream->bytes.left(4)==QByteArrayLiteral("DKIF")&&stream->bytes.mid(8,4)==QByteArrayLiteral("AV01");
            const bool av1Obu=!ivfAv1&&looksLikeAv1ObuStream(stream->bytes,QFileInfo(path).fileName());
            raw=ivfAv1?splitIvfAv1(stream->bytes,elementaryFps,elementaryDuration):(av1Obu?splitAv1Obus(stream->bytes):splitUnits(stream->bytes));stream->codec=(ivfAv1||av1Obu)?Codec::Av1:detectCodec(raw,QFileInfo(path).fileName());stream->format=ivfAv1?QStringLiteral("AV1 IVF"):(stream->codec==Codec::Av1?QStringLiteral("AV1 low-overhead OBU stream"):(stream->codec==Codec::Mpeg4Visual?QStringLiteral("MPEG-4 Visual start-code stream"):(!raw.isEmpty()&&raw.first().lengthPrefixed?QStringLiteral("Length-prefixed"):QStringLiteral("Annex-B"))));
        }
        if(raw.isEmpty())throw std::runtime_error("No supported video bitstream units found");
        stream->nals.reserve(raw.size());
        int mpeg4ProfileLevel=-1;
        for(int i=0;i<raw.size();++i){
            NalUnit nal;nal.index=i;nal.offset=raw[i].offset;nal.startCodeSize=raw[i].startCode;nal.size=raw[i].payload.size()+raw[i].startCode;nal.payload=raw[i].payload;nal.sampleIndex=raw[i].sampleIndex;if(nal.payload.isEmpty())continue;
            const quint8 h0=quint8(nal.payload.at(0));
            nal.nalType=stream->codec==Codec::Avc?(h0&31):(stream->codec==Codec::Hevc?((h0>>1)&63):(stream->codec==Codec::Av1?((h0>>3)&15):h0));
            nal.refIdc=stream->codec==Codec::Avc?((h0>>5)&3):0;
            nal.temporalId=stream->codec==Codec::Hevc&&nal.payload.size()>1?((quint8(nal.payload.at(1))&7)-1):-1;
            nal.name=stream->codec==Codec::Avc?avcName(nal.nalType):(stream->codec==Codec::Hevc?hevcName(nal.nalType):(stream->codec==Codec::Mpeg4Visual?mpeg4Name(nal.nalType):av1Name(nal.nalType)));
            try{
                if(stream->codec==Codec::Avc){
                    if(nal.nalType==7){auto s=parseAvcSps(nal.payload);nal.fields=s.fields;stream->sps.insert(s.id,s);stream->primarySpsId=s.id;}
                    else if(nal.nalType==8){auto p=parseAvcPps(nal.payload);nal.fields=p.fields;stream->pps.insert(p.id,p);}
                    else if(nal.nalType==6)nal.sei=parseSei(nal.payload,stream->codec);
                    else if(isVcl(stream->codec,nal.nalType)){nal.hasSlice=true;nal.slice=parseAvcSlice(nal,stream->sps,stream->pps);nal.fields=nal.slice.fields;}
                }else if(stream->codec==Codec::Hevc){
                    if(nal.nalType==33){auto s=parseHevcSps(nal.payload);nal.fields=s.fields;stream->sps.insert(s.id,s);stream->primarySpsId=s.id;}
                    else if(nal.nalType==34){auto p=parseHevcPps(nal.payload);nal.fields=p.fields;stream->pps.insert(p.id,p);}
                    else if(nal.nalType==39||nal.nalType==40)nal.sei=parseSei(nal.payload,stream->codec);
                    else if(isVcl(stream->codec,nal.nalType)){nal.hasSlice=true;nal.slice=parseHevcSlice(nal,stream->sps,stream->pps);nal.fields=nal.slice.fields;}
                }else if(stream->codec==Codec::Mpeg4Visual){
                    if(nal.nalType==0xb0&&nal.payload.size()>1){
                        mpeg4ProfileLevel=quint8(nal.payload.at(1));
                        field(nal.fields,"profile_and_level_indication",QStringLiteral("0x%1").arg(mpeg4ProfileLevel,2,16,QChar('0')).toUpper(),"u(8)",mpeg4ProfileName(mpeg4ProfileLevel));
                    }else if(nal.nalType>=0x20&&nal.nalType<=0x2f){
                        auto vol=parseMpeg4Vol(nal.payload,mpeg4ProfileLevel);
                        nal.fields=vol.fields;stream->sps.insert(vol.id,vol);stream->primarySpsId=vol.id;
                    }else if(nal.nalType==0xb5&&nal.payload.size()>1){
                        BitReader br(nal.payload.mid(1));const bool identifier=br.flag();field(nal.fields,"is_visual_object_identifier",identifier,"u(1)");if(identifier){field(nal.fields,"visual_object_verid",br.readBits(4),"u(4)");field(nal.fields,"visual_object_priority",br.readBits(3),"u(3)");}field(nal.fields,"visual_object_type",br.readBits(4),"u(4)");
                    }else if(nal.nalType==0xb3&&nal.payload.size()>1){
                        BitReader br(nal.payload.mid(1));field(nal.fields,"time_code_hours",br.readBits(5),"u(5)");field(nal.fields,"time_code_minutes",br.readBits(6),"u(6)");field(nal.fields,"marker_bit",br.flag(),"u(1)");field(nal.fields,"time_code_seconds",br.readBits(6),"u(6)");field(nal.fields,"closed_gov",br.flag(),"u(1)");field(nal.fields,"broken_link",br.flag(),"u(1)");
                    }else if(nal.nalType==0xb2){
                        field(nal.fields,"user_data",QString::fromLatin1(nal.payload.mid(1)).remove(QChar('\0')),"bytes");
                    }else if(nal.nalType==0xb6){
                        nal.hasSlice=true;nal.slice=parseMpeg4Vop(nal,stream->sps.value(stream->primarySpsId));nal.fields=nal.slice.fields;
                    }
                }else{
                    const Av1ObuHeader obuHeader=av1ObuHeader(nal.payload);
                    nal.temporalId=obuHeader.extension?obuHeader.temporalId:0;
                    field(nal.fields,"obu_forbidden_bit",0,"f(1)");
                    field(nal.fields,"obu_type",QStringLiteral("%1 (%2)").arg(obuHeader.type).arg(av1Name(obuHeader.type)),"f(4)");
                    field(nal.fields,"obu_extension_flag",obuHeader.extension,"f(1)");
                    field(nal.fields,"obu_has_size_field",obuHeader.hasSize,"f(1)");
                    if(obuHeader.extension){field(nal.fields,"temporal_id",obuHeader.temporalId,"f(3)");field(nal.fields,"spatial_id",obuHeader.spatialId,"f(2)");}
                    field(nal.fields,"obu_size",obuHeader.payloadSize,"leb128()");
                    if(nal.nalType==1){auto sequence=parseAv1SequenceHeader(nal.payload);nal.fields+=sequence.fields;stream->sps.insert(sequence.id,sequence);stream->primarySpsId=sequence.id;}
                    else if(nal.nalType==3||nal.nalType==6){nal.hasSlice=true;nal.slice=parseAv1FrameHeader(nal.payload,stream->sps.value(stream->primarySpsId));nal.fields+=nal.slice.fields;}
                    else if(nal.nalType==5){QByteArray metadata=av1ObuPayload(nal.payload);int position=0;quint64 metadataType=0;if(readLeb128(metadata,position,metadata.size(),metadataType)){field(nal.fields,"metadata_type",QStringLiteral("%1 (%2)").arg(metadataType).arg(av1MetadataName(metadataType)),"leb128()");field(nal.fields,"metadata_payload_size",metadata.size()-position,"bytes");BitReader metadataBits(metadata.mid(position));if(metadataType==1&&metadataBits.bitsLeft()>=32){field(nal.fields,"max_cll",metadataBits.readBits(16),"f(16)");field(nal.fields,"max_fall",metadataBits.readBits(16),"f(16)");}else if(metadataType==2&&metadataBits.bitsLeft()>=192){for(int primary=0;primary<3;++primary){field(nal.fields,QStringLiteral("primary_chromaticity_x[%1]").arg(primary),metadataBits.readBits(16),"f(16)");field(nal.fields,QStringLiteral("primary_chromaticity_y[%1]").arg(primary),metadataBits.readBits(16),"f(16)");}field(nal.fields,"white_point_chromaticity_x",metadataBits.readBits(16),"f(16)");field(nal.fields,"white_point_chromaticity_y",metadataBits.readBits(16),"f(16)");field(nal.fields,"luminance_max",metadataBits.readBits(32),"f(32)");field(nal.fields,"luminance_min",metadataBits.readBits(32),"f(32)");}else if(metadataType==4&&metadataBits.bitsLeft()>=8){const int countryCode=int(metadataBits.readBits(8));field(nal.fields,"itu_t_t35_country_code",countryCode,"f(8)");if(countryCode==0xff&&metadataBits.bitsLeft()>=8)field(nal.fields,"itu_t_t35_country_code_extension_byte",metadataBits.readBits(8),"f(8)");field(nal.fields,"itu_t_t35_payload_bits",metadataBits.bitsLeft(),"bits");}}}
                }
            }catch(const std::exception &e){field(nal.fields,"parse_error",QString::fromUtf8(e.what()));}
            nal.category=nal.hasSlice?nal.slice.type:(isParameter(stream->codec,nal.nalType)?QStringLiteral("PARAM"):(!nal.sei.isEmpty()?QStringLiteral("SEI"):(stream->codec==Codec::Mpeg4Visual&&nal.nalType==0xb2?QStringLiteral("USER"):(stream->codec==Codec::Av1&&nal.nalType==5?QStringLiteral("METADATA"):(stream->codec==Codec::Av1&&nal.nalType==4?QStringLiteral("TILE"):QStringLiteral("OTHER"))))));
            if(isParameter(stream->codec,nal.nalType))stream->parameterNalIndices.push_back(i);
            stream->nals.push_back(std::move(nal));
        }
        QVector<int> pending;FrameInfo *current=nullptr;
        for(const auto &nal:stream->nals){
            if(!nal.hasSlice){
                if(stream->codec==Codec::Av1&&current&&nal.nalType==4){current->nalIndices.push_back(nal.index);current->size+=nal.size;}
                else pending.push_back(nal.index);
                continue;
            }
            bool starts=false;
            if(stream->containerFile)starts=nal.sampleIndex>=0&&(!current||current->sampleIndex!=nal.sampleIndex);
            else if(stream->codec==Codec::Av1){
                if(nal.sampleIndex>=0)starts=!current||current->sampleIndex!=nal.sampleIndex;
                else{const bool temporalDelimiter=std::any_of(pending.begin(),pending.end(),[&stream](int index){return stream->nals.at(index).nalType==2;});const bool currentAlreadyShown=current&&std::any_of(current->sliceNalIndices.begin(),current->sliceNalIndices.end(),[&stream](int index){const auto &header=stream->nals.at(index).slice;return header.showFrame||header.showExistingFrame;});starts=!current||temporalDelimiter||currentAlreadyShown;}
            }else starts=stream->codec==Codec::Mpeg4Visual||(stream->codec==Codec::Avc?nal.slice.firstMb==0:nal.slice.firstSlice);
            if(!current||starts){FrameInfo frame;frame.index=stream->frames.size();frame.sampleIndex=nal.sampleIndex;frame.nalIndices=pending;frame.offset=stream->containerFile?nal.offset:(pending.isEmpty()?nal.offset:stream->nals.at(pending.first()).offset);for(int ni:pending)if(!stream->containerFile||stream->nals.at(ni).sampleIndex>=0)frame.size+=stream->nals.at(ni).size;frame.pts=frame.index/30.0;stream->frames.push_back(frame);current=&stream->frames.last();pending.clear();}
            current->nalIndices.push_back(nal.index);current->sliceNalIndices.push_back(nal.index);current->size+=nal.size;current->type=nal.slice.type;
        }
        if(current)for(int ni:pending){current->nalIndices.push_back(ni);current->size+=stream->nals.at(ni).size;}
        const auto primary=stream->sps.value(stream->primarySpsId);stream->width=primary.width;stream->height=primary.height;stream->profile=primary.profile;stream->level=primary.level;stream->bitDepth=primary.bitDepth;stream->chroma=primary.chroma;if(stream->codec==Codec::Mpeg4Visual)stream->profileName=mpeg4ProfileName(primary.profile);else if(stream->codec==Codec::Av1)stream->profileName=av1ProfileName(primary.profile,primary.level);
        if(stream->containerFile){stream->fps=mp4.fps>=1.0&&mp4.fps<=240.0?mp4.fps:25.0;stream->duration=mp4.duration;for(auto &frame:stream->frames){if(frame.sampleIndex>=0&&frame.sampleIndex<mp4.pts.size())frame.pts=mp4.pts.at(frame.sampleIndex);else frame.pts=frame.index/stream->fps;if(frame.sampleIndex>=0&&frame.sampleIndex<mp4.sampleSizes.size())frame.size=mp4.sampleSizes.at(frame.sampleIndex);}}
        else{stream->fps=elementaryFps>=1.0&&elementaryFps<=240.0?elementaryFps:(primary.fps>=1.0&&primary.fps<=240.0?primary.fps:25.0);for(auto &frame:stream->frames)frame.pts=frame.index/stream->fps;stream->duration=elementaryDuration>0.0?elementaryDuration:(stream->frames.isEmpty()?0.0:stream->frames.size()/stream->fps);}
        if(stream->codec==Codec::Mpeg4Visual){
            if(stream->containerFile){
                QVector<int> displayOrder;for(int i=0;i<stream->frames.size();++i)displayOrder.push_back(i);std::stable_sort(displayOrder.begin(),displayOrder.end(),[&stream](int a,int b){return stream->frames.at(a).pts<stream->frames.at(b).pts;});for(int display=0;display<displayOrder.size();++display)stream->frames[displayOrder.at(display)].poc=display;
            }else{
                int previousReferenceDisplay=-1;
                for(int index=0;index<stream->frames.size();++index){
                    if(stream->frames.at(index).type==QStringLiteral("B"))continue;
                    int followingB=0;while(index+followingB+1<stream->frames.size()&&stream->frames.at(index+followingB+1).type==QStringLiteral("B"))++followingB;
                    const int display=previousReferenceDisplay<0?0:previousReferenceDisplay+followingB+1;
                    stream->frames[index].poc=display;for(int offset=1;offset<=followingB;++offset)stream->frames[index+offset].poc=display-followingB-1+offset;
                    previousReferenceDisplay=display;
                }
                int maximumDisplay=-1;for(auto &frame:stream->frames){if(frame.poc<0)frame.poc=++maximumDisplay;maximumDisplay=std::max(maximumDisplay,frame.poc);frame.pts=frame.poc/stream->fps;}stream->duration=maximumDisplay>=0?(maximumDisplay+1)/stream->fps:0.0;
            }
        }
        stream->bitrate=stream->duration>0?stream->bytes.size()*8.0/stream->duration:0.0;analyzeFrames(*stream);
    }catch(const std::exception &e){stream->error=QString::fromUtf8(e.what());}
    return stream;
}

} // namespace bitscope
