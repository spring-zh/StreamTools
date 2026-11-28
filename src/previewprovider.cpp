#include "previewprovider.h"
#include "hevcaccess.h"
#include "mpeg4access.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>

namespace bitscope {

PreviewProvider::PreviewProvider(QObject *parent) : QObject(parent) {
    m_queueTimer.setSingleShot(true);m_queueTimer.setInterval(35);
    connect(&m_queueTimer,&QTimer::timeout,this,&PreviewProvider::processQueue);
    connect(&m_process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,&PreviewProvider::processFinished);
    connect(&m_process,&QProcess::errorOccurred,this,&PreviewProvider::processError);
}

PreviewProvider::~PreviewProvider(){if(m_process.state()!=QProcess::NotRunning){m_process.kill();m_process.waitForFinished(1000);}}

void PreviewProvider::setStream(StreamPtr stream){if(m_process.state()!=QProcess::NotRunning){m_cancelled=true;m_process.kill();m_process.waitForFinished(1000);m_cancelled=false;}m_stream=std::move(stream);m_pending.clear();m_cached.clear();m_failed.clear();m_activeRequested.clear();m_activeOutputOrder.clear();m_priorityFrame=-1;clearActiveFiles();}

namespace {

QString decodedUnitCategory(const QString &code)
{
    if (code.contains(QLatin1Char('I')) || code.contains(QLatin1Char('i'))
        || code.contains(QLatin1Char('A')) || code.contains(QLatin1Char('P')))
        return QStringLiteral("帧内预测");
    if (code.contains(QLatin1Char('S')) || code.contains(QLatin1Char('g')))
        return QStringLiteral("跳过");
    if (code.contains(QLatin1Char('X')) || code.contains(QLatin1Char('d'))
        || code.contains(QLatin1Char('D')))
        return QStringLiteral("双向/直接");
    if (code.contains(QLatin1Char('<')))
        return QStringLiteral("反向预测");
    if (code.contains(QLatin1Char('>')))
        return QStringLiteral("前向预测");
    if (code.contains(QLatin1Char('G')))
        return QStringLiteral("全局运动预测");
    return QStringLiteral("其他");
}

struct DecodedFrameBlock {
    QVector<DecodedUnitInfo> units;
    int validCount = 0;
    int serial = 0;
};

} // namespace

QString PreviewProvider::ffmpegPath() const {
    QString configured=qEnvironmentVariable("BITSCOPE_FFMPEG");
    const QString name=
#ifdef Q_OS_WIN
        QStringLiteral("ffmpeg.exe");
#else
        QStringLiteral("ffmpeg");
#endif
    if(!configured.isEmpty()){
        const QFileInfo configuredInfo(configured);if(configuredInfo.isDir())configured=QDir(configured).filePath(name);
        if(QFileInfo(configured).isExecutable())return QFileInfo(configured).absoluteFilePath();
    }
    const QDir appDir(QCoreApplication::applicationDirPath());
    QStringList candidates{appDir.filePath(name),appDir.filePath(QStringLiteral("tools/")+name),appDir.filePath(QStringLiteral("../Resources/")+name)};
#ifdef Q_OS_MACOS
    candidates<<QStringLiteral("/opt/homebrew/bin/ffmpeg")<<QStringLiteral("/usr/local/bin/ffmpeg")<<QStringLiteral("/opt/local/bin/ffmpeg");
#elif defined(Q_OS_LINUX)
    candidates<<QStringLiteral("/usr/bin/ffmpeg")<<QStringLiteral("/usr/local/bin/ffmpeg")<<QStringLiteral("/snap/bin/ffmpeg");
#elif defined(Q_OS_WIN)
    const QString localAppData=qEnvironmentVariable("LOCALAPPDATA");if(!localAppData.isEmpty())candidates<<QDir(localAppData).filePath(QStringLiteral("Microsoft/WinGet/Links/ffmpeg.exe"));
    const QString programData=qEnvironmentVariable("ProgramData");if(!programData.isEmpty())candidates<<QDir(programData).filePath(QStringLiteral("chocolatey/bin/ffmpeg.exe"));
#endif
    for(const QString &candidate:candidates)if(QFileInfo(candidate).isExecutable())return QFileInfo(candidate).absoluteFilePath();
    return QStandardPaths::findExecutable(name);
}

void PreviewProvider::requestFrame(int frameIndex){m_pending.clear();m_failed.remove(frameIndex);if(m_process.state()!=QProcess::NotRunning&&m_activeRequested.contains(frameIndex))return;m_priorityFrame=frameIndex;if(m_process.state()!=QProcess::NotRunning){m_cancelled=true;m_process.kill();m_activeRequested.clear();}requestRange(frameIndex,frameIndex);}
void PreviewProvider::forgetFrame(int frameIndex){m_cached.remove(frameIndex);}
bool PreviewProvider::isIdle() const{return m_pending.isEmpty()&&m_process.state()==QProcess::NotRunning&&!m_queueTimer.isActive();}

void PreviewProvider::requestRange(int first,int last){if(!m_stream||m_stream->frames.isEmpty())return;const int finalFrame=int(m_stream->frames.size())-1;first=std::clamp(first,0,finalFrame);last=std::clamp(last,first,finalFrame);for(int i=first;i<=last;++i)if(!m_cached.contains(i)&&!m_failed.contains(i))m_pending.insert(i);if(!m_pending.isEmpty()&&m_process.state()==QProcess::NotRunning)m_queueTimer.start();}

int PreviewProvider::nearestStart(int frameIndex) const
{
    if (!m_stream || frameIndex <= 0)
        return 0;
    if (m_stream->codec == Codec::Hevc)
        return nearestHevcDecodeStart(m_stream, frameIndex);
    if (m_stream->codec == Codec::Mpeg4Visual)
        return nearestMpeg4DecodeStart(m_stream, frameIndex);
    for (int index = frameIndex; index >= 0; --index) {
        const auto &frame = m_stream->frames.at(index);
        // IDR/IRAP is immediately safe. A Recovery Point is only safe for a
        // target after recovery_frame_cnt pictures have been decoded. Starting
        // at the Recovery Point itself is what caused intra-refresh previews
        // to turn green because their older reference pictures were absent.
        if (frame.randomAccess)
            return index;
        if (frame.recoveryPoint) {
            const int recoveryCount = std::max(0, frame.recoveryFrameCount);
            if (index + recoveryCount <= frameIndex)
                return index;
        }
    }
    return 0;
}

QByteArray PreviewProvider::accessUnit(int frameIndex,bool includeParameters) const {
    QByteArray out;if(m_stream->codec==Codec::Av1){out.append(char(0x12));out.append(char(0x00));}QSet<int> seen;QVector<int> indices;if(includeParameters)indices+=m_stream->parameterNalIndices;indices+=m_stream->frames.at(frameIndex).nalIndices;
    for(int index:indices){if(seen.contains(index)||index<0||index>=m_stream->nals.size())continue;seen.insert(index);if(m_stream->codec==Codec::Av1&&m_stream->nals.at(index).nalType==2)continue;if(m_stream->codec!=Codec::Av1)out.append("\0\0\0\1",4);out.append(m_stream->nals.at(index).payload);}return out;
}

QByteArray PreviewProvider::segment(int first,int last) const {QByteArray out;for(int i=first;i<=last;++i)out.append(accessUnit(i,i==first));return out;}

QString PreviewProvider::failureReason(const QString &summary, const QString &stderrText,
                                       const QString &outputPath) const
{
    // qp+mb_type logs can contain millions of cells. Keep actionable errors and
    // a bounded tail, not the initial FFmpeg banner or whole macroblock dump.
    const QRegularExpression errorPattern(QStringLiteral(
        "error|failed|invalid|cannot|could not|not found|no such|unsupported|"
        "unknown (?:decoder|encoder|format|option)|permission denied|missing|output file is empty|nothing was encoded"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression zeroErrors(QStringLiteral("\\b0 decod(?:e|ing) errors\\b"),
                                       QRegularExpression::CaseInsensitiveOption);
    QStringList errors, tail;
    for (const QString &line : stderrText.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;
        const QString bounded = trimmed.left(1000);
        tail.append(bounded);
        if (tail.size() > 12) tail.removeFirst();
        // FFmpeg versions use both "0 decode errors" and "0 decoding errors".
        if (errorPattern.match(trimmed).hasMatch()
            && !zeroErrors.match(trimmed).hasMatch()
            && !trimmed.startsWith(QStringLiteral("Reading option"))) {
            errors.append(bounded);
            if (errors.size() > 8) errors.removeFirst();
        }
    }
    QString reason = summary;
    if (!errors.isEmpty()) reason += QStringLiteral(" · ") + errors.last();
    reason += QStringLiteral("\n解码范围：F%1–F%2；预计输出 %3 帧\nFFmpeg：%4\n输入片段：%5")
                  .arg(m_activeStart).arg(m_activeEnd).arg(m_activeOutputOrder.size())
                  .arg(m_process.program(), m_activeInput);
    if (!outputPath.isEmpty()) reason += QStringLiteral("\n预期图片：") + outputPath;
    if (!m_processIoError.isEmpty()) reason += QStringLiteral("\n进程错误：") + m_processIoError;
    reason += QStringLiteral("\n参数：") + QString::fromUtf8(QJsonDocument(
        QJsonArray::fromStringList(m_process.arguments())).toJson(QJsonDocument::Compact));
    reason += QStringLiteral("\nFFmpeg 关键日志：\n")
        + (errors.isEmpty() ? QStringLiteral("未报告明确错误。") : errors.join(QLatin1Char('\n')));
    reason += QStringLiteral("\nFFmpeg 日志末尾（最多 12 行，每行最多 1000 字符）：\n")
        + (tail.isEmpty() ? QStringLiteral("无 stderr 输出。") : tail.join(QLatin1Char('\n')));
    return reason;
}

void PreviewProvider::failActivePreviews(const QString &reason)
{
    for (int index : m_activeRequested) {
        m_failed.insert(index);
        Q_EMIT previewFailed(index, reason);
        Q_EMIT decodedUnitsUnavailable(index, reason.left(300));
    }
}

void PreviewProvider::processQueue(){if(!m_stream||m_pending.isEmpty()||m_process.state()!=QProcess::NotRunning)return;const QString executable=ffmpegPath();if(executable.isEmpty()){const auto pending=m_pending;m_pending.clear();const QString reason=QStringLiteral("未找到 FFmpeg。macOS 可执行 brew install ffmpeg；也可把 ffmpeg 放到应用旁边，或设置 BITSCOPE_FFMPEG。 ");for(int i:pending){m_failed.insert(i);Q_EMIT previewFailed(i,reason);}Q_EMIT statusChanged(QStringLiteral("预览不可用：未找到 FFmpeg（码流分析不受影响）"));Q_EMIT queueIdle();return;}
    QList<int> sorted=m_pending.values();std::sort(sorted.begin(),sorted.end());const int anchor=m_pending.contains(m_priorityFrame)?m_priorityFrame:sorted.first();m_priorityFrame=-1;m_activeStart=nearestStart(anchor);m_activeRequested.clear();m_activeEnd=anchor;
    for(int index:sorted){if(nearestStart(index)==m_activeStart&&index<=anchor+64){m_activeRequested.push_back(index);m_activeEnd=std::max(m_activeEnd,index);}}
    for(int index:m_activeRequested)m_pending.remove(index);
    m_activeOutputOrder.clear();
    for(int index=m_activeStart;index<=m_activeEnd;++index){
        if(m_stream->codec!=Codec::Hevc
            || !hevcFrameSuppressedAtStart(m_stream,m_activeStart,index))
            m_activeOutputOrder.push_back(index);
    }
    if (m_stream->codec == Codec::Mpeg4Visual)
        m_activeOutputOrder = mpeg4PreviewOutputOrder(m_stream, m_activeStart, m_activeEnd);
    if(m_stream->containerFile
        && (m_stream->codec==Codec::Avc||m_stream->codec==Codec::Hevc))
        std::stable_sort(m_activeOutputOrder.begin(),m_activeOutputOrder.end(),[this](int a,int b){return m_stream->frames.at(a).pts<m_stream->frames.at(b).pts;});
    const QString suffix=m_stream->codec==Codec::Avc?QStringLiteral("h264"):(m_stream->codec==Codec::Hevc?QStringLiteral("h265"):(m_stream->codec==Codec::Mpeg4Visual?QStringLiteral("m4v"):QStringLiteral("obu")));
    m_processIoError.clear();
    m_process.setProgram(executable);
    m_process.setArguments({});
    const int job=m_job++;m_activePrefix=QDir(m_temp.path()).filePath(QStringLiteral("preview_%1_").arg(job));m_activeInput=QDir(m_temp.path()).filePath(QStringLiteral("preview_%1.%2").arg(job).arg(suffix));
    QFile input(m_activeInput);
    QString inputError;
    if (!m_temp.isValid()) inputError = QStringLiteral("无法创建预览临时目录：%1").arg(m_temp.errorString());
    else if (!input.open(QIODevice::WriteOnly)) inputError = QStringLiteral("无法创建预览输入文件：%1").arg(input.errorString());
    else {
        const QByteArray data = segment(m_activeStart, m_activeEnd);
        if (input.write(data) != data.size() || !input.flush())
            inputError = QStringLiteral("写入预览输入文件失败：%1").arg(input.errorString());
        input.close();
    }
    if (!inputError.isEmpty()) {
        failActivePreviews(failureReason(inputError));
        clearActiveFiles();
        if (!m_pending.isEmpty()) m_queueTimer.start(); else Q_EMIT queueIdle();
        return;
    }
    const QString output=m_activePrefix+QStringLiteral("%05d.jpg");const QString fps=QString::number(m_stream->fps,'f',3);
    const bool macroblockDebug=m_stream->codec==Codec::Avc||m_stream->codec==Codec::Mpeg4Visual;
    // Progress output uses carriage returns and can share a line with MB logs.
    QStringList args{QStringLiteral("-hide_banner"),QStringLiteral("-nostats"),QStringLiteral("-loglevel"),macroblockDebug?QStringLiteral("repeat+debug"):QStringLiteral("error")};
    if(macroblockDebug)args<<QStringLiteral("-threads")<<QStringLiteral("1")<<QStringLiteral("-debug")<<QStringLiteral("qp+mb_type")<<QStringLiteral("-probesize")<<QStringLiteral("32")<<QStringLiteral("-analyzeduration")<<QStringLiteral("0");
    // MPEG-4's delayed EOF output omits ff_print_debug_info in FFmpeg 8.
    // Request each decoded picture immediately (including B pictures), retaining
    // reference decoding but mapping BOTH JPEGs and MB logs in input/coded order.
    // Do not PTS-sort m_activeOutputOrder for this codec above.
    const QString decoderFlags = m_stream->codec == Codec::Mpeg4Visual
        ? QStringLiteral("+output_corrupt+low_delay") : QStringLiteral("+output_corrupt");
    const QString demuxer=m_stream->codec==Codec::Avc?QStringLiteral("h264"):(m_stream->codec==Codec::Hevc?QStringLiteral("hevc"):(m_stream->codec==Codec::Mpeg4Visual?QStringLiteral("m4v"):QStringLiteral("obu")));
    args<<QStringLiteral("-fflags")<<QStringLiteral("+genpts")<<QStringLiteral("-err_detect")<<QStringLiteral("ignore_err")<<QStringLiteral("-flags")<<decoderFlags<<QStringLiteral("-r")<<fps<<QStringLiteral("-f")<<demuxer<<QStringLiteral("-i")<<m_activeInput<<QStringLiteral("-an")<<QStringLiteral("-vf")<<QStringLiteral("scale=320:-2")<<QStringLiteral("-fps_mode")<<QStringLiteral("passthrough")<<QStringLiteral("-q:v")<<QStringLiteral("5")<<QStringLiteral("-start_number")<<QStringLiteral("0")<<QStringLiteral("-frames:v")<<QString::number(m_activeOutputOrder.size())<<QStringLiteral("-y")<<output;
    m_process.setArguments(args);
    Q_EMIT statusChanged(QStringLiteral("正在生成预览 F%1–F%2 · %3").arg(m_activeStart).arg(m_activeEnd).arg(executable));m_process.start();
}

void PreviewProvider::processFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_cancelled) {
        m_cancelled = false;
        clearActiveFiles();
        if (!m_pending.isEmpty())
            m_queueTimer.start();
        else
            Q_EMIT queueIdle();
        return;
    }
    const QByteArray stderrData = m_process.readAllStandardError();
    const QString stderrText = QString::fromLocal8Bit(stderrData).trimmed();
    const bool mpeg4 = m_stream && m_stream->codec == Codec::Mpeg4Visual;
    const int imageCount = mpeg4 ? QDir(m_temp.path()).entryList(
        {QFileInfo(m_activePrefix).fileName() + QStringLiteral("*.jpg")}, QDir::Files).size() : -1;
    if (status != QProcess::NormalExit || exitCode != 0 || !m_processIoError.isEmpty()) {
        const QString summary = status == QProcess::CrashExit
            ? QStringLiteral("FFmpeg 进程异常终止（退出码 %1）：%2").arg(exitCode).arg(m_process.errorString())
            : QStringLiteral("FFmpeg 解码失败（退出码 %1）").arg(exitCode);
        failActivePreviews(failureReason(summary, stderrText));
    } else if (mpeg4 && imageCount != m_activeOutputOrder.size()) {
        // Unexpected skips (e.g. damaged or uncoded VOPs) must never silently
        // bind following pictures or macroblocks to the wrong source frames.
        failActivePreviews(failureReason(QStringLiteral(
            "MPEG-4 输出帧数不匹配：预计 %1，实际 %2；已停止绑定图片和宏块，避免错帧")
            .arg(m_activeOutputOrder.size()).arg(imageCount), stderrText));
    } else {
        if (m_stream && (m_stream->codec == Codec::Avc
                         || m_stream->codec == Codec::Mpeg4Visual)) {
            const int expected = m_activeOutputOrder.size();
            const auto decoded = parseAvcDecodedUnits(stderrData, expected);
            for (int relative = 0; relative < decoded.size() && relative < expected; ++relative) {
                const int frameIndex = m_activeOutputOrder.at(relative);
                if (!decoded.at(relative).isEmpty())
                    Q_EMIT decodedUnitsReady(frameIndex, decoded.at(relative));
            }
            for (int index : m_activeRequested) {
                const int relative = m_activeOutputOrder.indexOf(index);
                if (relative < 0 || relative >= decoded.size() || decoded.at(relative).isEmpty())
                    Q_EMIT decodedUnitsUnavailable(index,
                        QStringLiteral("FFmpeg 没有导出该帧的宏块数据"));
            }
        }
        QString diagnosticContext;
        const auto pictureFailure = [&](const QString &summary, const QString &path = QString()) {
            // A batch can miss many thumbnails; scan its large MB debug log
            // only once, and only if an image actually fails.
            if (diagnosticContext.isEmpty()) diagnosticContext = failureReason({}, stderrText);
            return summary + diagnosticContext
                + (path.isEmpty() ? QString() : QStringLiteral("\n预期图片：") + path);
        };
        for (int index : m_activeRequested) {
            const int relative = m_activeOutputOrder.indexOf(index);
            if (relative < 0) {
                m_failed.insert(index);
                Q_EMIT previewFailed(index, pictureFailure(
                    QStringLiteral("F%1 不在当前解码段的输出映射中（可能为随机访问起点抑制的图片）").arg(index)));
                continue;
            }
            const QString path = m_activePrefix
                + QStringLiteral("%1.jpg").arg(relative, 5, 10, QChar('0'));
            QImageReader reader(path);
            reader.setAutoTransform(true);
            const QImage image = reader.read();
            if (image.isNull()) {
                m_failed.insert(index);
                const QFileInfo outputInfo(path);
                const QString summary = !outputInfo.exists()
                    ? QStringLiteral("FFmpeg 正常退出，但未生成 F%1 的预览图片（输出序号 %2）").arg(index).arg(relative)
                    : QStringLiteral("F%1 预览图片读取失败（%2 字节）：%3")
                          .arg(index).arg(outputInfo.size()).arg(reader.errorString());
                Q_EMIT previewFailed(index, pictureFailure(summary, path));
            } else {
                m_cached.insert(index);
                Q_EMIT thumbnailReady(index, image);
            }
        }
    }
    clearActiveFiles();
    Q_EMIT statusChanged(m_pending.isEmpty()
        ? QStringLiteral("预览与宏块数据按可见区域加载")
        : QStringLiteral("等待生成 %1 帧预览").arg(m_pending.size()));
    if (!m_pending.isEmpty())
        m_queueTimer.start();
    else
        Q_EMIT queueIdle();
}

QVector<QVector<DecodedUnitInfo>> PreviewProvider::parseAvcDecodedUnits(const QByteArray &log,
                                                                        int expectedFrames) const
{
    QVector<QVector<DecodedUnitInfo>> result;
    if (!m_stream || (m_stream->codec != Codec::Avc
                      && m_stream->codec != Codec::Mpeg4Visual)
        || expectedFrames <= 0)
        return result;
    const int columns = int(std::ceil(double(m_stream->width) / 16.0));
    const int rows = int(std::ceil(double(m_stream->height) / 16.0));
    if (columns <= 0 || rows <= 0)
        return result;

    // FFmpeg formats contexts with %p: Windows may omit the POSIX 0x prefix.
    const QString prefix = QStringLiteral("\\[(?:h264|mpeg4) @ ((?:0[xX])?[0-9a-fA-F]+)\\]");
    const QRegularExpression contextPattern(prefix);
    const QRegularExpression framePattern(prefix + QStringLiteral(" New frame, type:"));
    // qp+mb_type emits exactly 5 columns per MB, not whitespace-separated
    // tokens. In particular, interlaced partitions can have no separator.
    const QRegularExpression typePattern(QStringLiteral("^[PiIAdDgGS<>X][ +|?\\-][ =]$"));
    const QRegularExpression rowPattern(QStringLiteral(
        "(?:^|\\s)(\\d+) ((?:[ 0-9]{2}[PiIAdDgGS<>X][ +|?\\-][ =]){%1})$").arg(columns));
    const int minimumValid = std::max(1, (columns * rows + 1) / 2);
    QHash<QString, QVector<DecodedFrameBlock>> blocks;
    QHash<QString, int> current;
    QHash<QString, int> lastSerial;
    int serial = 0;
    bool pendingFrameHeader = false;
    QString lastContext;
    const QRegularExpression unprefixedFramePattern(QStringLiteral("New frame, type: [IPB]"));
    const auto startBlock = [&](const QString &context) {
        DecodedFrameBlock block;
        block.units.resize(columns * rows);
        for (int address = 0; address < block.units.size(); ++address) {
            block.units[address].address = address;
            block.units[address].column = address % columns;
            block.units[address].row = address / columns;
        }
        block.serial = serial++;
        blocks[context].push_back(std::move(block));
        current[context] = blocks[context].size() - 1;
        lastSerial[context] = serial;
        pendingFrameHeader = false;
    };

    const QStringList lines = QString::fromLocal8Bit(log).split(QLatin1Char('\n'));
    for (const QString &rawLine : lines) {
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1); // Preserve the fixed-width cell padding.
        const auto contextMatch = contextPattern.match(line);
        if (contextMatch.hasMatch())
            lastContext = contextMatch.captured(1);
        const auto frameMatch = framePattern.match(line);
        if (frameMatch.hasMatch()) {
            startBlock(frameMatch.captured(1));
            continue;
        }
        // FFmpeg's log prefix state is global. A partial filter diagnostic can
        // suppress the context prefix of the next decoder message. Bind such
        // a frame header to the next validated MB row, not to a guessed pointer.
        if (unprefixedFramePattern.match(line).hasMatch()) {
            pendingFrameHeader = true;
            continue;
        }

        const auto rowMatch = rowPattern.match(line);
        if (!rowMatch.hasMatch())
            continue;
        // Decoder threads are forced to one. A row without its prefix belongs
        // to the last decoder context; require a full fixed-width grid row so
        // unrelated filter/progress messages cannot become macroblock data.
        const QString context = lastContext;
        if (context.isEmpty())
            continue;
        bool rowOk = false;
        const int pixelY = rowMatch.captured(1).toInt(&rowOk);
        if (!rowOk || pixelY % 16 != 0)
            continue;
        const int row = pixelY / 16;
        if (row < 0 || row >= rows)
            continue;
        const QString cells = rowMatch.captured(2);
        if (cells.size() < columns * 5)
            continue;
        if (!typePattern.match(cells.mid(2, 3)).hasMatch())
            continue; // Column-coordinate heading, not a macroblock row.
        if (pendingFrameHeader || !current.contains(context)
            || (row == 0 && blocks[context][current[context]].units[0].qp >= 0))
            startBlock(context);

        auto &block = blocks[context][current[context]];
        for (int column = 0; column < columns; ++column) {
            const QString cell = cells.mid(column * 5, 5);
            const QString type = cell.mid(2, 3);
            if (!typePattern.match(type).hasMatch())
                continue;
            bool qpOk = false;
            const int qp = cell.left(2).trimmed().toInt(&qpOk);
            if (!qpOk || qp < 0 || qp > 51)
                continue;
            DecodedUnitInfo &unit = block.units[row * columns + column];
            if (unit.qp < 0)
                ++block.validCount;
            unit.qp = qp;
            unit.decoderType = type.trimmed();
            unit.category = decodedUnitCategory(unit.decoderType);
        }
    }

    QString bestContext;
    int bestCount = -1;
    int bestSerial = -1;
    for (auto it = blocks.cbegin(); it != blocks.cend(); ++it) {
        int validBlocks = 0;
        for (const auto &block : it.value())
            if (block.validCount >= minimumValid)
                ++validBlocks;
        if (validBlocks > bestCount || (validBlocks == bestCount && lastSerial.value(it.key()) > bestSerial)) {
            bestContext = it.key();
            bestCount = validBlocks;
            bestSerial = lastSerial.value(it.key());
        }
    }
    if (bestContext.isEmpty())
        return result;
    for (const auto &block : blocks.value(bestContext)) {
        // Keep a slot for missing frame data; compacting the list would assign
        // the following picture's QP and MB types to the wrong preview.
        result.push_back(block.validCount >= minimumValid
                             ? block.units : QVector<DecodedUnitInfo>{});
        if (result.size() >= expectedFrames)
            break;
    }
    return result;
}

void PreviewProvider::processError(QProcess::ProcessError error)
{
    if (m_cancelled) return;
    m_processIoError = m_process.errorString();
    if (error == QProcess::FailedToStart) {
        failActivePreviews(failureReason(QStringLiteral("FFmpeg 无法启动：%1").arg(m_process.errorString()),
                                        QString::fromLocal8Bit(m_process.readAllStandardError())));
        clearActiveFiles();
        if (!m_pending.isEmpty()) m_queueTimer.start(); else Q_EMIT queueIdle();
    }
}

void PreviewProvider::clearActiveFiles(){if(!m_activeInput.isEmpty())QFile::remove(m_activeInput);if(!m_activePrefix.isEmpty()){QDir dir(m_temp.path());const QString stem=QFileInfo(m_activePrefix).fileName();for(const QString &name:dir.entryList({stem+QStringLiteral("*.jpg")},QDir::Files))dir.remove(name);}m_activeInput.clear();m_activePrefix.clear();}

} // namespace bitscope
