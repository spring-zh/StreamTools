#include "av1blockprovider.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>

namespace bitscope {

namespace {

using Cell = QVector<int>;
using Grid = QVector<QVector<Cell>>;

Grid decodeGrid(const QJsonValue &value, int tupleLength, int expectedColumns)
{
    Grid result;
    const QJsonArray rows = value.toArray();
    result.reserve(rows.size());
    for (const QJsonValue &rowValue : rows) {
        QVector<Cell> row;
        Cell previous;
        for (const QJsonValue &entry : rowValue.toArray()) {
            if (entry.isArray()) {
                const QJsonArray array = entry.toArray();
                if (array.size() == 1 && !previous.isEmpty()) {
                    const int repeats = array.first().toInt();
                    for (int index = 0; index < repeats && row.size() < expectedColumns; ++index)
                        row.push_back(previous);
                    continue;
                }
                Cell cell;
                cell.reserve(array.size());
                for (const QJsonValue &component : array)
                    cell.push_back(component.toInt());
                if (cell.size() == tupleLength) {
                    row.push_back(cell);
                    previous = cell;
                }
            } else if (tupleLength == 1) {
                previous = {entry.toInt()};
                row.push_back(previous);
            }
        }
        while (!previous.isEmpty() && row.size() < expectedColumns)
            row.push_back(previous);
        result.push_back(std::move(row));
    }
    return result;
}

Cell gridCell(const Grid &grid, int row, int column)
{
    if (row < 0 || row >= grid.size() || column < 0 || column >= grid.at(row).size())
        return {};
    return grid.at(row).at(column);
}

int gridValue(const Grid &grid, int row, int column, int fallback = -1)
{
    const Cell cell = gridCell(grid, row, column);
    return cell.isEmpty() ? fallback : cell.first();
}

QHash<int, QString> inverseMap(const QJsonObject &object)
{
    QHash<int, QString> result;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
        result.insert(it.value().toInt(), it.key());
    return result;
}

QString cleanEnumName(QString name)
{
    name.replace(QChar('_'), QChar(' '));
    return name;
}

QPair<int, int> blockDimensions(const QString &name)
{
    static const QRegularExpression expression(QStringLiteral("BLOCK_(\\d+)X(\\d+)"));
    const auto match = expression.match(name);
    if (!match.hasMatch())
        return {4, 4};
    return {match.captured(1).toInt(), match.captured(2).toInt()};
}

int visualDepth(int superblockSize, int width, int height)
{
    int depth = 0;
    int size = superblockSize;
    const int largest = std::max(width, height);
    while (size > largest && size > 4) {
        size /= 2;
        ++depth;
    }
    return depth;
}

} // namespace

Av1BlockProvider::Av1BlockProvider(QObject *parent) : QObject(parent)
{
    m_queueTimer.setSingleShot(true);
    m_queueTimer.setInterval(45);
    connect(&m_queueTimer, &QTimer::timeout, this, &Av1BlockProvider::processQueue);
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &Av1BlockProvider::processFinished);
    connect(&m_process, &QProcess::errorOccurred, this, &Av1BlockProvider::processError);
    connect(&m_parseWatcher, &QFutureWatcher<Av1BlockParseResult>::finished,
            this, &Av1BlockProvider::parseFinished);
}

Av1BlockProvider::~Av1BlockProvider()
{
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
    if (m_parseWatcher.isRunning())
        m_parseWatcher.waitForFinished();
}

void Av1BlockProvider::setStream(StreamPtr stream)
{
    ++m_generation;
    if (m_process.state() != QProcess::NotRunning) {
        m_cancelled = true;
        m_process.kill();
        m_process.waitForFinished(1000);
        m_cancelled = false;
    }
    clearActiveFiles();
    m_stream = std::move(stream);
    m_pending.clear();
    m_cached.clear();
    m_failed.clear();
    m_activeRequested.clear();
    m_priorityFrame = -1;
}

QString Av1BlockProvider::analyzerPath() const
{
    QString configured = qEnvironmentVariable("BITSCOPE_AV1_INSPECT");
#ifdef Q_OS_WIN
    const QString executableName = QStringLiteral("av1inspect.exe");
#else
    const QString executableName = QStringLiteral("av1inspect");
#endif
    if (!configured.isEmpty()) {
        const QFileInfo info(configured);
        if (info.isDir())
            configured = QDir(configured).filePath(executableName);
        if (QFileInfo(configured).isExecutable())
            return QFileInfo(configured).absoluteFilePath();
    }
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        appDir.filePath(executableName),
        appDir.filePath(QStringLiteral("tools/") + executableName),
        appDir.filePath(QStringLiteral("../Resources/tools/") + executableName)
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo(candidate).isExecutable())
            return QFileInfo(candidate).absoluteFilePath();
    }
    return QStandardPaths::findExecutable(executableName);
}

void Av1BlockProvider::requestFrame(int frameIndex)
{
    m_pending.clear();
    if ((m_process.state() != QProcess::NotRunning
         && m_activeRequested.size() == 1 && m_activeRequested.first() == frameIndex)
        || (m_parseWatcher.isRunning() && m_parseRequested.contains(frameIndex)))
        return;
    m_priorityFrame = frameIndex;
    if (m_process.state() != QProcess::NotRunning
        && (m_activeRequested.size() != 1 || m_activeRequested.first() != frameIndex)) {
        m_cancelled = true;
        m_process.kill();
        m_activeRequested.clear();
    }
    requestRange(frameIndex, frameIndex);
}

void Av1BlockProvider::requestRange(int first, int last)
{
    if (!m_stream || m_stream->codec != Codec::Av1 || m_stream->frames.isEmpty())
        return;
    const int finalFrame = m_stream->frames.size() - 1;
    first = std::clamp(first, 0, finalFrame);
    last = std::clamp(last, first, finalFrame);
    for (int index = first; index <= last; ++index) {
        if ((m_process.state() != QProcess::NotRunning && m_activeRequested.contains(index))
            || (m_parseWatcher.isRunning() && m_parseRequested.contains(index)))
            continue;
        if (m_cached.contains(index) || m_failed.contains(index))
            continue;
        if (m_stream->frames.at(index).displayType == QStringLiteral("SHOW_EXISTING")) {
            const QString reason = QStringLiteral("SHOW_EXISTING_FRAME 复用参考槽画面，本帧没有新的 Coding Block 数据");
            m_failed.insert(index);
            Q_EMIT codingBlocksUnavailable(index, reason);
            continue;
        }
        m_pending.insert(index);
    }
    if (!m_pending.isEmpty() && m_process.state() == QProcess::NotRunning
        && !m_parseWatcher.isRunning())
        m_queueTimer.start();
}

int Av1BlockProvider::nearestStart(int frameIndex) const
{
    if (!m_stream || frameIndex <= 0)
        return 0;
    for (int index = frameIndex; index >= 0; --index) {
        if (m_stream->frames.at(index).randomAccess)
            return index;
    }
    return 0;
}

QByteArray Av1BlockProvider::accessUnit(int frameIndex, bool includeSequenceHeader) const
{
    QByteArray output;
    output.append(char(0x12));
    output.append(char(0x00));
    QSet<int> seen;
    QVector<int> indices;
    if (includeSequenceHeader)
        indices += m_stream->parameterNalIndices;
    indices += m_stream->frames.at(frameIndex).nalIndices;
    for (int index : indices) {
        if (index < 0 || index >= m_stream->nals.size() || seen.contains(index))
            continue;
        seen.insert(index);
        const NalUnit &unit = m_stream->nals.at(index);
        if (unit.nalType == 2)
            continue;
        output.append(unit.payload);
    }
    return output;
}

QByteArray Av1BlockProvider::ivfSegment(int first, int last) const
{
    QByteArray output;
    QDataStream stream(&output, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("DKIF", 4);
    stream << quint16(0) << quint16(32);
    stream.writeRawData("AV01", 4);
    stream << quint16(std::clamp(m_stream->width, 0, 65535));
    stream << quint16(std::clamp(m_stream->height, 0, 65535));
    const quint32 rate = quint32(std::max(1, qRound(m_stream->fps * 1000.0)));
    stream << rate << quint32(1000);
    stream << quint32(last - first + 1) << quint32(0);
    for (int index = first; index <= last; ++index) {
        const QByteArray frame = accessUnit(index, index == first);
        stream << quint32(frame.size()) << quint64(index - first);
        stream.writeRawData(frame.constData(), frame.size());
    }
    return output;
}

void Av1BlockProvider::processQueue()
{
    if (!m_stream || m_pending.isEmpty() || m_process.state() != QProcess::NotRunning
        || m_parseWatcher.isRunning())
        return;
    const QString executable = analyzerPath();
    if (executable.isEmpty()) {
        const QString reason = QStringLiteral("未找到启用 inspection 的 AV1 分析器，无法导出真实 Coding Block");
        const auto pending = m_pending;
        m_pending.clear();
        for (int index : pending) {
            m_failed.insert(index);
            Q_EMIT codingBlocksUnavailable(index, reason);
        }
        Q_EMIT statusChanged(reason);
        return;
    }

    QList<int> sorted = m_pending.values();
    std::sort(sorted.begin(), sorted.end());
    const int anchor = m_pending.contains(m_priorityFrame)
        ? m_priorityFrame : sorted.first();
    m_priorityFrame = -1;
    m_activeStart = nearestStart(anchor);
    m_activeEnd = anchor;
    m_activeOutputStart = anchor - m_activeStart;
    m_activeRequested.clear();
    for (int index : sorted) {
        if (nearestStart(index) == m_activeStart && index <= anchor + 12) {
            m_activeRequested.push_back(index);
            m_activeEnd = std::max(m_activeEnd, index);
        }
    }
    for (int index : m_activeRequested)
        m_pending.remove(index);

    const int job = m_job++;
    m_activeDirectory = QDir(m_temp.path()).filePath(QStringLiteral("av1_%1").arg(job));
    QDir().mkpath(m_activeDirectory);
    m_activeInput = QDir(m_activeDirectory).filePath(QStringLiteral("segment.ivf"));
    QFile input(m_activeInput);
    if (!input.open(QIODevice::WriteOnly)) {
        const QString reason = input.errorString();
        for (int index : m_activeRequested) {
            m_failed.insert(index);
            Q_EMIT codingBlocksUnavailable(index, reason);
        }
        clearActiveFiles();
        m_queueTimer.start();
        return;
    }
    input.write(ivfSegment(m_activeStart, m_activeEnd));
    input.close();
    const QStringList arguments{
        m_activeInput,
        QStringLiteral("--output-start=%1").arg(m_activeOutputStart),
        QStringLiteral("--blockSize"), QStringLiteral("--mode"),
        QStringLiteral("--uv_mode"), QStringLiteral("--transformSize"),
        QStringLiteral("--transformType"), QStringLiteral("--delta_q"),
        QStringLiteral("--seg_id"), QStringLiteral("--referenceFrame"),
        QStringLiteral("--motionVectors"), QStringLiteral("--skip"),
        QStringLiteral("--intrabc"), QStringLiteral("--compress")
    };
    m_process.setWorkingDirectory(m_activeDirectory);
    Q_EMIT statusChanged(QStringLiteral("正在解析 AV1 Superblock/Coding Block F%1–F%2")
                             .arg(m_activeStart).arg(m_activeEnd));
    QString program = executable;
    QStringList processArguments = arguments;
#ifndef Q_OS_WIN
    const QString nice = QStandardPaths::findExecutable(QStringLiteral("nice"));
    if (!nice.isEmpty()) {
        processArguments.prepend(executable);
        processArguments.prepend(QStringLiteral("10"));
        processArguments.prepend(QStringLiteral("-n"));
        program = nice;
    }
#endif
    m_process.start(program, processArguments);
}

Av1BlockParseResult Av1BlockProvider::parseOutput(const QByteArray &data,
                                                  const StreamPtr &stream,
                                                  int activeStart)
{
    Av1BlockParseResult result;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
        result.error = QStringLiteral("AV1 inspection JSON 解析失败：%1")
                           .arg(parseError.errorString());
        return result;
    }

    const int miColumns = (stream->width + 3) / 4;
    const QJsonArray frameArray = document.array();
    for (const QJsonValue &frameValue : frameArray) {
        if (!frameValue.isObject())
            continue;
        const QJsonObject object = frameValue.toObject();
        result.inputFrames.push_back(object.value(QStringLiteral("inputFrame")).toInt(-1));
        const auto blockNames = inverseMap(object.value(QStringLiteral("blockSizeMap")).toObject());
        const auto modeNames = inverseMap(object.value(QStringLiteral("modeMap")).toObject());
        const auto uvModeNames = inverseMap(object.value(QStringLiteral("uv_modeMap")).toObject());
        const auto transformSizeNames = inverseMap(object.value(QStringLiteral("transformSizeMap")).toObject());
        const auto transformTypeNames = inverseMap(object.value(QStringLiteral("transformTypeMap")).toObject());
        const auto referenceNames = inverseMap(object.value(QStringLiteral("referenceFrameMap")).toObject());
        const Grid blockSizes = decodeGrid(object.value(QStringLiteral("blockSize")), 1, miColumns);
        const Grid modes = decodeGrid(object.value(QStringLiteral("mode")), 1, miColumns);
        const Grid uvModes = decodeGrid(object.value(QStringLiteral("uv_mode")), 1, miColumns);
        const Grid transformSizes = decodeGrid(object.value(QStringLiteral("transformSize")), 1, miColumns);
        const Grid transformTypes = decodeGrid(object.value(QStringLiteral("transformType")), 1, miColumns);
        const Grid qIndices = decodeGrid(object.value(QStringLiteral("delta_q")), 1, miColumns);
        const Grid segmentIds = decodeGrid(object.value(QStringLiteral("seg_id")), 1, miColumns);
        const Grid references = decodeGrid(object.value(QStringLiteral("referenceFrame")), 2, miColumns);
        const Grid motionVectors = decodeGrid(object.value(QStringLiteral("motionVectors")), 4, miColumns);
        const Grid skips = decodeGrid(object.value(QStringLiteral("skip")), 1, miColumns);
        const Grid intraBlockCopy = decodeGrid(object.value(QStringLiteral("intrabc")), 1, miColumns);

        QVector<Av1CodingBlockInfo> blocks;
        QSet<QString> seen;
        for (int row = 0; row < blockSizes.size(); ++row) {
            for (int column = 0; column < blockSizes.at(row).size(); ++column) {
                const int blockCode = gridValue(blockSizes, row, column);
                const QString enumName = blockNames.value(blockCode, QStringLiteral("BLOCK_4X4"));
                const auto dimensions = blockDimensions(enumName);
                const int widthMi = std::max(1, dimensions.first / 4);
                const int heightMi = std::max(1, dimensions.second / 4);
                const int originColumn = (column / widthMi) * widthMi;
                const int originRow = (row / heightMi) * heightMi;
                const int x = originColumn * 4;
                const int y = originRow * 4;
                if (x >= stream->width || y >= stream->height)
                    continue;
                const QString key = QStringLiteral("%1:%2:%3:%4")
                    .arg(x).arg(y).arg(dimensions.first).arg(dimensions.second);
                if (seen.contains(key))
                    continue;
                seen.insert(key);

                Av1CodingBlockInfo block;
                block.x = x;
                block.y = y;
                block.codedWidth = dimensions.first;
                block.codedHeight = dimensions.second;
                block.width = std::min(block.codedWidth, stream->width - x);
                block.height = std::min(block.codedHeight, stream->height - y);
                block.superblockAddress = (y / stream->frames.at(activeStart).unitSize)
                        * stream->frames.at(activeStart).unitColumns
                    + x / stream->frames.at(activeStart).unitSize;
                block.depth = visualDepth(stream->frames.at(activeStart).unitSize,
                                          block.codedWidth, block.codedHeight);
                block.blockSize = enumName.mid(QStringLiteral("BLOCK_").size());
                block.predictionMode = cleanEnumName(modeNames.value(
                    gridValue(modes, row, column), QStringLiteral("UNKNOWN")));
                block.uvPredictionMode = cleanEnumName(uvModeNames.value(
                    gridValue(uvModes, row, column), QStringLiteral("UNKNOWN")));
                block.transformSize = transformSizeNames.value(
                    gridValue(transformSizes, row, column), QStringLiteral("—"));
                block.transformSize.remove(QStringLiteral("TX_"));
                block.transformType = cleanEnumName(transformTypeNames.value(
                    gridValue(transformTypes, row, column), QStringLiteral("—")));
                block.qIndex = gridValue(qIndices, row, column);
                block.segmentId = gridValue(segmentIds, row, column);
                block.skip = gridValue(skips, row, column, 0) != 0;
                block.intraBlockCopy = gridValue(intraBlockCopy, row, column, 0) != 0;
                const Cell refs = gridCell(references, row, column);
                QStringList refText;
                for (int ref : refs) {
                    if (ref >= 0)
                        refText << cleanEnumName(referenceNames.value(ref, QString::number(ref)));
                }
                block.referenceFrames = refText.join(QStringLiteral(" + "));
                const Cell vectors = gridCell(motionVectors, row, column);
                if (vectors.size() >= 4) {
                    block.motionVector0X = vectors.at(0);
                    block.motionVector0Y = vectors.at(1);
                    block.motionVector1X = vectors.at(2);
                    block.motionVector1Y = vectors.at(3);
                }
                blocks.push_back(std::move(block));
            }
        }
        std::sort(blocks.begin(), blocks.end(), [](const auto &left, const auto &right) {
            return left.y == right.y ? left.x < right.x : left.y < right.y;
        });
        result.frames.push_back(std::move(blocks));
    }
    return result;
}

void Av1BlockProvider::processFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_cancelled) {
        m_cancelled = false;
        clearActiveFiles();
        if (!m_pending.isEmpty())
            m_queueTimer.start();
        return;
    }
    const QString processErrorText = QString::fromLocal8Bit(m_process.readAllStandardError()).trimmed();
    if (status != QProcess::NormalExit || exitCode != 0) {
        const QString reason = processErrorText.isEmpty()
            ? QStringLiteral("AV1 inspection 分析器退出码 %1").arg(exitCode)
            : processErrorText.left(500);
        for (int index : m_activeRequested) {
            m_failed.insert(index);
            Q_EMIT codingBlocksUnavailable(index, reason);
        }
        clearActiveFiles();
        Q_EMIT statusChanged(reason);
        if (!m_pending.isEmpty())
            m_queueTimer.start();
        return;
    }

    const QByteArray output = m_process.readAllStandardOutput();
    m_parseRequested = m_activeRequested;
    m_parseStart = m_activeStart;
    m_parseEnd = m_activeEnd;
    m_parseOutputStart = m_activeOutputStart;
    m_parseGeneration = m_generation;
    const StreamPtr stream = m_stream;
    clearActiveFiles();
    Q_EMIT statusChanged(QStringLiteral("正在后台整理 AV1 Coding Block 数据…"));
    m_parseWatcher.setFuture(QtConcurrent::run(
        [output, stream, start = m_parseStart] {
            return Av1BlockProvider::parseOutput(output, stream, start);
        }));
}

void Av1BlockProvider::parseFinished()
{
    const Av1BlockParseResult result = m_parseWatcher.result();
    if (m_parseGeneration == m_generation && m_stream
        && m_parseStart >= 0 && m_parseEnd < m_stream->frames.size()) {
        const bool hasInputFrameMapping = result.inputFrames.size() == result.frames.size()
            && std::all_of(result.inputFrames.cbegin(), result.inputFrames.cend(),
                           [](int index) { return index >= 0; });
        if (hasInputFrameMapping) {
            // AV1 may output more than one decoded picture for a single input
            // packet (hidden reference pictures followed by the shown picture).
            // Keep the final callback for that packet: it is the picture the
            // frame row and preview represent.
            QHash<int, int> lastResultForFrame;
            for (int relative = 0; relative < result.frames.size(); ++relative) {
                const int frameIndex = m_parseStart + result.inputFrames.at(relative);
                if (frameIndex >= m_parseStart && frameIndex <= m_parseEnd
                    && m_stream->frames.at(frameIndex).displayType
                        != QStringLiteral("SHOW_EXISTING"))
                    lastResultForFrame.insert(frameIndex, relative);
            }
            for (auto it = lastResultForFrame.constBegin();
                 it != lastResultForFrame.constEnd(); ++it) {
                const auto &blocks = result.frames.at(it.value());
                if (!blocks.isEmpty()) {
                    m_cached.insert(it.key());
                    Q_EMIT codingBlocksReady(it.key(), blocks);
                }
            }
        } else {
            // Backward compatibility for an official/unpatched inspection
            // helper, which has no inputFrame field and emits sequential JSON.
            QVector<int> decodedFrameIndices;
            for (int index = m_parseStart; index <= m_parseEnd; ++index) {
                if (m_stream->frames.at(index).displayType != QStringLiteral("SHOW_EXISTING"))
                    decodedFrameIndices.push_back(index);
            }
            int filteredOutputStart = 0;
            const int firstOutputFrame = m_parseStart + m_parseOutputStart;
            for (int index : decodedFrameIndices) {
                if (index >= firstOutputFrame)
                    break;
                ++filteredOutputStart;
            }
            const int expectedFilteredFrames = std::max(
                0, int(decodedFrameIndices.size()) - filteredOutputStart);
            const int mappingOffset = result.frames.size() > expectedFilteredFrames
                ? 0 : filteredOutputStart;
            for (int relative = 0; relative < result.frames.size()
                 && relative + mappingOffset < decodedFrameIndices.size(); ++relative) {
                const int frameIndex = decodedFrameIndices.at(relative + mappingOffset);
                if (!result.frames.at(relative).isEmpty()) {
                    m_cached.insert(frameIndex);
                    Q_EMIT codingBlocksReady(frameIndex, result.frames.at(relative));
                }
            }
        }
        for (int index : m_parseRequested) {
            if (!m_cached.contains(index)) {
                const QString reason = result.error.isEmpty()
                    ? QStringLiteral("分析器未输出 Frame %1 的 AV1 Coding Block").arg(index)
                    : result.error;
                m_failed.insert(index);
                Q_EMIT codingBlocksUnavailable(index, reason);
            }
        }
    }

    Q_EMIT statusChanged(m_pending.isEmpty()
        ? QStringLiteral("AV1 Superblock/Coding Block 已按需加载")
        : QStringLiteral("等待解析 %1 帧 AV1 块数据").arg(m_pending.size()));
    if (!m_pending.isEmpty())
        m_queueTimer.start();
}

void Av1BlockProvider::processError(QProcess::ProcessError errorValue)
{
    if (m_cancelled)
        return;
    if (errorValue == QProcess::FailedToStart) {
        const QString reason = m_process.errorString();
        for (int index : m_activeRequested) {
            m_failed.insert(index);
            Q_EMIT codingBlocksUnavailable(index, reason);
        }
        clearActiveFiles();
        Q_EMIT statusChanged(reason);
        if (!m_pending.isEmpty())
            m_queueTimer.start();
    }
}

void Av1BlockProvider::clearActiveFiles()
{
    if (!m_activeDirectory.isEmpty()) {
        const QFileInfo info(m_activeDirectory);
        if (info.absoluteFilePath().startsWith(QDir(m_temp.path()).absolutePath()))
            QDir(m_activeDirectory).removeRecursively();
    }
    m_activeDirectory.clear();
    m_activeInput.clear();
}

} // namespace bitscope
