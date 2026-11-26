#pragma once

#include <QByteArray>
#include <QImage>
#include <QMap>
#include <QString>
#include <QVariant>
#include <QVector>

#include <memory>

namespace bitscope {

enum class Codec { Avc, Hevc, Mpeg4Visual };

struct SyntaxField {
    QString name;
    QString value;
    QString bits;
    QString description;
};

struct Mp4BoxInfo {
    QString type;
    qint64 offset = 0;
    qint64 size = 0;
    qint64 headerSize = 8;
    QString description;
    QVector<SyntaxField> fields;
    QVector<Mp4BoxInfo> children;
};

struct SeiMessage {
    int type = -1;
    QString name;
    int payloadSize = 0;
    int recoveryFrameCount = -1;
    bool exactMatch = false;
    bool brokenLink = false;
    QVector<SyntaxField> fields;
};

struct SliceInfo {
    int firstMb = 0;
    bool firstSlice = true;
    int address = 0;
    QString type = QStringLiteral("?");
    int rawType = -1;
    int ppsId = -1;
    int frameNum = -1;
    int poc = -1;
    int qpDelta = 0;
    int numRefL0 = 0;
    int numRefL1 = 0;
    int quantizer = -1;
    int timeIncrement = -1;
    bool coded = true;
    QVector<SyntaxField> fields;
};

struct NalUnit {
    int index = 0;
    qint64 offset = 0;
    int startCodeSize = 0;
    int size = 0;
    int nalType = -1;
    int refIdc = 0;
    int temporalId = -1;
    int sampleIndex = -1;
    QString name;
    QString category;
    QByteArray payload;
    QVector<SyntaxField> fields;
    QVector<SeiMessage> sei;
    bool hasSlice = false;
    SliceInfo slice;
};

struct CodingUnitRegion {
    int sliceIndex = 0;
    int start = 0;
    int end = 0;
    QString type;
    int qp = -1;
};

struct DecodedUnitInfo {
    int address = -1;
    int column = -1;
    int row = -1;
    int qp = -1;
    QString decoderType;
    QString category;
};

struct HevcCodingUnitInfo {
    int ctuAddress = -1;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int codedSize = 0;
    int depth = 0;
    int partitionMode = -1;
    QString partitionName;
    QString prediction;
    int qp = -1;
    int approximateBytes = -1;
};

struct FrameInfo {
    int index = 0;
    int sampleIndex = -1;
    qint64 offset = 0;
    qint64 size = 0;
    double pts = 0.0;
    QString type;
    QString displayType;
    QString structure;
    QVector<int> nalIndices;
    QVector<int> sliceNalIndices;
    QVector<int> references;
    QVector<int> referencedBy;
    bool randomAccess = false;
    bool recoveryPoint = false;
    int recoveryFrameCount = -1;
    bool referencePicture = false;
    bool truncatedStart = false;
    int poc = -1;
    int unitSize = 16;
    int unitColumns = 0;
    int unitRows = 0;
    QVector<CodingUnitRegion> regions;
    QVector<DecodedUnitInfo> decodedUnits;
    bool decodedUnitsReady = false;
    QString decodedUnitsError;
    QVector<HevcCodingUnitInfo> hevcCodingUnits;
    bool hevcCodingUnitsReady = false;
    QString hevcCodingUnitsError;
};

struct ParameterSetInfo {
    int id = -1;
    int parentId = -1;
    int profile = 0;
    int level = 0;
    int width = 0;
    int height = 0;
    int chroma = 1;
    int bitDepth = 8;
    int log2FrameNum = 4;
    int pocType = 0;
    int log2Poc = 4;
    int maxReferences = 1;
    bool frameOnly = true;
    int minCodingBlockSize = 8;
    int ctbSize = 64;
    int sliceAddressBits = 0;
    int entropyCoding = 0;
    int initQp = 0;
    int defaultL0 = 1;
    int defaultL1 = 1;
    int sliceGroups = 0;
    int timeIncrementResolution = 0;
    int timeIncrementBits = 1;
    int quantPrecision = 5;
    int shape = 0;
    bool interlaced = false;
    double fps = 0.0;
    QVector<SyntaxField> fields;
};

struct StreamInfo {
    QString filePath;
    QString fileName;
    QString format;
    bool containerFile = false;
    QVector<Mp4BoxInfo> boxes;
    Codec codec = Codec::Avc;
    QByteArray bytes;
    QVector<NalUnit> nals;
    QVector<FrameInfo> frames;
    QMap<int, ParameterSetInfo> sps;
    QMap<int, ParameterSetInfo> pps;
    QVector<int> parameterNalIndices;
    int primarySpsId = -1;
    int width = 0;
    int height = 0;
    int profile = 0;
    int level = 0;
    QString profileName;
    int bitDepth = 8;
    int chroma = 1;
    double fps = 30.0;
    double duration = 0.0;
    double bitrate = 0.0;
    QString error;

    QString codecName() const {
        if (codec == Codec::Avc) return QStringLiteral("H.264 / AVC");
        if (codec == Codec::Hevc) return QStringLiteral("H.265 / HEVC");
        return QStringLiteral("MPEG-4 Part 2 Visual");
    }
};

using StreamPtr = std::shared_ptr<StreamInfo>;

} // namespace bitscope

Q_DECLARE_METATYPE(bitscope::StreamPtr)
