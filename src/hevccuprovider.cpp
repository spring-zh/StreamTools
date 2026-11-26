#include "hevccuprovider.h"
#include "hevcaccess.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <functional>

namespace bitscope {

namespace {

struct AnalyzerRecord {
    int poc = 0;
    int ctuAddress = -1;
    QVector<int> values;
};

QVector<AnalyzerRecord> readRecords(const QString &path)
{
    QVector<AnalyzerRecord> records;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return records;
    const QRegularExpression pattern(QStringLiteral("^<(-?\\d+),(\\d+)>\\s*(.*)$"));
    while (!file.atEnd()) {
        const QString line = QString::fromLatin1(file.readLine()).trimmed();
        const auto match = pattern.match(line);
        if (!match.hasMatch())
            continue;
        AnalyzerRecord record;
        record.poc = match.captured(1).toInt();
        record.ctuAddress = match.captured(2).toInt();
        const QStringList tokens = match.captured(3).split(
            QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        record.values.reserve(tokens.size());
        for (const QString &token : tokens) {
            bool ok = false;
            const int value = token.toInt(&ok);
            if (ok)
                record.values.push_back(value);
        }
        records.push_back(std::move(record));
    }
    return records;
}

QString partitionName(int mode)
{
    static const QStringList names{
        QStringLiteral("2N×2N"), QStringLiteral("2N×N"),
        QStringLiteral("N×2N"), QStringLiteral("N×N"),
        QStringLiteral("2N×nU"), QStringLiteral("2N×nD"),
        QStringLiteral("nL×2N"), QStringLiteral("nR×2N")
    };
    return mode >= 0 && mode < names.size() ? names.at(mode) : QStringLiteral("None");
}

int predictionUnitCount(int mode)
{
    if (mode == 0)
        return 1;
    if (mode == 3)
        return 4;
    if (mode >= 1 && mode <= 7)
        return 2;
    return 0;
}

QString predictionName(const QVector<int> &values, int begin, int count)
{
    if (count <= 0 || begin < 0 || begin + count > values.size())
        return QStringLiteral("无");
    bool intra = false;
    bool inter = false;
    bool other = false;
    for (int index = begin; index < begin + count; ++index) {
        intra |= values.at(index) == 2;
        inter |= values.at(index) == 1;
        other |= values.at(index) != 1 && values.at(index) != 2;
    }
    if (intra && !inter && !other)
        return QStringLiteral("帧内预测");
    if (inter && !intra && !other)
        return QStringLiteral("帧间预测");
    return QStringLiteral("混合预测");
}

} // namespace

HevcCuProvider::HevcCuProvider(QObject *parent) : QObject(parent)
{
    m_queueTimer.setSingleShot(true);
    m_queueTimer.setInterval(45);
    connect(&m_queueTimer, &QTimer::timeout, this, &HevcCuProvider::processQueue);
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &HevcCuProvider::processFinished);
    connect(&m_process, &QProcess::errorOccurred, this, &HevcCuProvider::processError);
}

HevcCuProvider::~HevcCuProvider()
{
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

void HevcCuProvider::setStream(StreamPtr stream)
{
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

QString HevcCuProvider::decoderPath() const
{
    QString configured = qEnvironmentVariable("BITSCOPE_HEVC_ANALYZER");
#ifdef Q_OS_WIN
    const QString executableName = QStringLiteral("HM_180.exe");
#else
    const QString executableName = QStringLiteral("HM_180");
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

void HevcCuProvider::requestFrame(int frameIndex)
{
    m_pending.clear();
    if (m_process.state() != QProcess::NotRunning
        && m_activeRequested.size() == 1 && m_activeRequested.first() == frameIndex)
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

void HevcCuProvider::requestRange(int first, int last)
{
    if (!m_stream || m_stream->codec != Codec::Hevc || m_stream->frames.isEmpty())
        return;
    const int finalFrame = m_stream->frames.size() - 1;
    first = std::clamp(first, 0, finalFrame);
    last = std::clamp(last, first, finalFrame);
    for (int index = first; index <= last; ++index) {
        if (m_process.state() != QProcess::NotRunning && m_activeRequested.contains(index))
            continue;
        if (!m_cached.contains(index) && !m_failed.contains(index))
            m_pending.insert(index);
    }
    if (!m_pending.isEmpty() && m_process.state() == QProcess::NotRunning)
        m_queueTimer.start();
}

int HevcCuProvider::nearestStart(int frameIndex) const
{
    return nearestHevcDecodeStart(m_stream, frameIndex);
}

QByteArray HevcCuProvider::accessUnit(int frameIndex, bool includeParameters) const
{
    QByteArray output;
    QSet<int> seen;
    QVector<int> indices;
    if (includeParameters)
        indices += m_stream->parameterNalIndices;
    indices += m_stream->frames.at(frameIndex).nalIndices;
    for (int index : indices) {
        if (index < 0 || index >= m_stream->nals.size() || seen.contains(index))
            continue;
        seen.insert(index);
        output.append("\0\0\0\1", 4);
        output.append(m_stream->nals.at(index).payload);
    }
    return output;
}

QByteArray HevcCuProvider::segment(int first, int last) const
{
    QByteArray output;
    for (int index = first; index <= last; ++index)
        output.append(accessUnit(index, index == first));
    return output;
}

void HevcCuProvider::processQueue()
{
    if (!m_stream || m_pending.isEmpty() || m_process.state() != QProcess::NotRunning)
        return;
    const QString executable = decoderPath();
    if (executable.isEmpty()) {
        const QString reason = QStringLiteral("未找到 HEVC CU 参考解码器 HM_180，无法展开 CTU 内部四叉树");
        const auto pending = m_pending;
        m_pending.clear();
        for (int index : pending) {
            m_failed.insert(index);
            Q_EMIT codingUnitsUnavailable(index, reason);
        }
        Q_EMIT statusChanged(reason);
        return;
    }

    QList<int> sorted = m_pending.values();
    std::sort(sorted.begin(), sorted.end());
    m_activeInteractive = m_pending.contains(m_priorityFrame);
    const int anchor = m_activeInteractive ? m_priorityFrame : sorted.first();
    m_priorityFrame = -1;
    m_activeStart = nearestStart(anchor);
    m_activeEnd = anchor;
    m_activeRequested.clear();
    for (int index : sorted) {
        if (nearestStart(index) == m_activeStart && index <= anchor + 24) {
            m_activeRequested.push_back(index);
            m_activeEnd = std::max(m_activeEnd, index);
        }
    }
    for (int index : m_activeRequested)
        m_pending.remove(index);

    const int job = m_job++;
    m_activeDirectory = QDir(m_temp.path()).filePath(QStringLiteral("cu_%1").arg(job));
    QDir().mkpath(m_activeDirectory);
    m_activeInput = QDir(m_activeDirectory).filePath(QStringLiteral("segment.h265"));
    QFile input(m_activeInput);
    if (!input.open(QIODevice::WriteOnly)) {
        const QString reason = input.errorString();
        for (int index : m_activeRequested)
            Q_EMIT codingUnitsUnavailable(index, reason);
        clearActiveFiles();
        m_queueTimer.start();
        return;
    }
    input.write(segment(m_activeStart, m_activeEnd));
    input.close();

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QStringList outputFrames;
    for (int index : m_activeRequested)
        outputFrames.push_back(QString::number(
            hevcDecoderOutputOrdinal(m_stream, m_activeStart, index)));
    m_activeOutputFiltered = !outputFrames.isEmpty();
    environment.insert(QStringLiteral("BITSCOPE_HEVC_OUTPUT_FRAMES"),
                       outputFrames.join(QLatin1Char(',')));
    m_process.setProcessEnvironment(environment);

    m_process.setWorkingDirectory(m_activeDirectory);
    Q_EMIT statusChanged(m_activeInteractive
        ? QStringLiteral("CTU 根网格已显示 · 正在解码参考链 F%1→F%2，补充当前帧 CU 明细")
              .arg(m_activeStart).arg(anchor)
        : QStringLiteral("正在后台补充 HEVC CU 明细 F%1–F%2")
              .arg(anchor).arg(m_activeEnd));
    QString program = executable;
    QStringList arguments{QStringLiteral("-b"), m_activeInput};
#ifndef Q_OS_WIN
    const QString nice = QStandardPaths::findExecutable(QStringLiteral("nice"));
    if (!m_activeInteractive && !nice.isEmpty()) {
        arguments.prepend(executable);
        arguments.prepend(QStringLiteral("10"));
        arguments.prepend(QStringLiteral("-n"));
        program = nice;
    }
#endif
    m_process.start(program, arguments);
}

HevcCuParseResult HevcCuProvider::parseOutput(QString *error) const
{
    HevcCuParseResult result;
    const QVector<AnalyzerRecord> treeRecords = readRecords(
        QDir(m_activeDirectory).filePath(QStringLiteral("decoder_cupu.txt")));
    const QVector<AnalyzerRecord> predictionRecords = readRecords(
        QDir(m_activeDirectory).filePath(QStringLiteral("decoder_pred.txt")));
    const QVector<AnalyzerRecord> qpRecords = readRecords(
        QDir(m_activeDirectory).filePath(QStringLiteral("decoder_qp.txt")));
    const QVector<AnalyzerRecord> byteRecords = readRecords(
        QDir(m_activeDirectory).filePath(QStringLiteral("decoder_bit_scu.txt")));
    if (treeRecords.isEmpty()) {
        if (error)
            *error = QStringLiteral("参考解码器没有输出 decoder_cupu.txt");
        return result;
    }

    int currentPoc = 0;
    QSet<int> addresses;
    int frameGroup = -1;
    for (int recordIndex = 0; recordIndex < treeRecords.size(); ++recordIndex) {
        const AnalyzerRecord &tree = treeRecords.at(recordIndex);
        if (frameGroup < 0 || tree.poc != currentPoc || addresses.contains(tree.ctuAddress)) {
            result.frames.push_back({});
            result.pocs.push_back(tree.poc);
            ++frameGroup;
            currentPoc = tree.poc;
            addresses.clear();
        }
        addresses.insert(tree.ctuAddress);
        const QVector<int> predictions = recordIndex < predictionRecords.size()
            ? predictionRecords.at(recordIndex).values : QVector<int>();
        const QVector<int> qps = recordIndex < qpRecords.size()
            ? qpRecords.at(recordIndex).values : QVector<int>();
        const QVector<int> byteCounts = recordIndex < byteRecords.size()
            ? byteRecords.at(recordIndex).values : QVector<int>();

        const int ctuSize = m_stream->sps.value(m_stream->primarySpsId).ctbSize;
        const int columns = std::max(1, (m_stream->width + ctuSize - 1) / ctuSize);
        const int rootX = (tree.ctuAddress % columns) * ctuSize;
        const int rootY = (tree.ctuAddress / columns) * ctuSize;
        int treePosition = 0;
        int leafPosition = 0;
        int predictionPosition = 0;

        std::function<void(int, int, int, int)> parseNode;
        parseNode = [&](int x, int y, int size, int depth) {
            if (treePosition >= tree.values.size() || depth > 8)
                return;
            const int mode = tree.values.at(treePosition++);
            if (mode == 99 && size > 1) {
                const int half = size / 2;
                parseNode(x, y, half, depth + 1);
                parseNode(x + half, y, half, depth + 1);
                parseNode(x, y + half, half, depth + 1);
                parseNode(x + half, y + half, half, depth + 1);
                return;
            }

            const int qp = leafPosition < qps.size() ? qps.at(leafPosition) : -1;
            const int approximateBytes = leafPosition < byteCounts.size()
                ? byteCounts.at(leafPosition) : -1;
            const int puCount = predictionUnitCount(mode);
            const QString prediction = predictionName(
                predictions, predictionPosition, puCount);
            predictionPosition += puCount;
            ++leafPosition;

            if (mode == 15 || x >= m_stream->width || y >= m_stream->height)
                return;
            HevcCodingUnitInfo unit;
            unit.ctuAddress = tree.ctuAddress;
            unit.x = x;
            unit.y = y;
            unit.width = std::min(size, m_stream->width - x);
            unit.height = std::min(size, m_stream->height - y);
            unit.codedSize = size;
            unit.depth = depth;
            unit.partitionMode = mode;
            unit.partitionName = partitionName(mode);
            unit.prediction = prediction;
            unit.qp = qp;
            unit.approximateBytes = approximateBytes;
            result.frames[frameGroup].push_back(std::move(unit));
        };
        parseNode(rootX, rootY, ctuSize, 0);
    }
    return result;
}

void HevcCuProvider::processFinished(int exitCode, QProcess::ExitStatus status)
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
            ? QStringLiteral("HM_180 退出码 %1").arg(exitCode)
            : processErrorText.left(500);
        for (int index : m_activeRequested) {
            m_failed.insert(index);
            Q_EMIT codingUnitsUnavailable(index, reason);
        }
    } else {
        QString parseError;
        const auto result = parseOutput(&parseError);
        const bool filteredOutput = m_activeOutputFiltered
            && result.frames.size() <= m_activeRequested.size();
        const bool mapByPoc = !filteredOutput && result.frames.size() == result.pocs.size()
            && std::all_of(m_activeRequested.cbegin(), m_activeRequested.cend(),
                           [this](int index) { return m_stream->frames.at(index).poc >= 0; });
        for (int relative = 0; relative < result.frames.size(); ++relative) {
            int frameIndex = m_activeStart + relative;
            if (filteredOutput) {
                frameIndex = relative < m_activeRequested.size()
                    ? m_activeRequested.at(relative) : -1;
            } else if (mapByPoc) {
                frameIndex = -1;
                for (int index = m_activeStart; index <= m_activeEnd; ++index) {
                    if (m_stream->frames.at(index).poc == result.pocs.at(relative)) {
                        frameIndex = index;
                        break;
                    }
                }
            }
            if (frameIndex >= m_activeStart && frameIndex <= m_activeEnd
                && !result.frames.at(relative).isEmpty()) {
                m_cached.insert(frameIndex);
                Q_EMIT codingUnitsReady(frameIndex, result.frames.at(relative));
            }
        }
        for (int index : m_activeRequested) {
            if (!m_cached.contains(index)) {
                const QString reason = parseError.isEmpty()
                    ? QStringLiteral("参考解码器未输出 Frame %1 的 CU 树").arg(index)
                    : parseError;
                m_failed.insert(index);
                Q_EMIT codingUnitsUnavailable(index, reason);
            }
        }
    }

    clearActiveFiles();
    Q_EMIT statusChanged(m_pending.isEmpty()
        ? QStringLiteral("HEVC CTU/CU 四叉树已按需加载")
        : QStringLiteral("等待解析 %1 帧 HEVC CU 数据").arg(m_pending.size()));
    if (!m_pending.isEmpty())
        m_queueTimer.start();
}

void HevcCuProvider::processError(QProcess::ProcessError errorValue)
{
    if (m_cancelled)
        return;
    if (errorValue == QProcess::Crashed || errorValue == QProcess::FailedToStart) {
        const QString reason = m_process.errorString();
        for (int index : m_activeRequested) {
            m_failed.insert(index);
            Q_EMIT codingUnitsUnavailable(index, reason);
        }
    }
}

void HevcCuProvider::clearActiveFiles()
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
