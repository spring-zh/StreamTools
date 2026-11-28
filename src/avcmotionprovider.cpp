#include "avcmotionprovider.h"
#include "mpeg4access.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <sstream>
#include <QtConcurrent/QtConcurrentRun>

namespace bitscope {
AvcMotionProvider::AvcMotionProvider(QObject *parent) : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(180);
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(60000);
    connect(&m_debounce, &QTimer::timeout, this, &AvcMotionProvider::start);
    connect(&m_preparation, &QFutureWatcher<QString>::finished, this, [this] {
        m_preparing = false;
        if (m_active < 0) { if (m_pending >= 0) m_debounce.start(); return; }
        const auto error = m_preparation.result();
        if (!error.isEmpty()) {
            const int active = m_active;
            m_active = -1;
            Q_EMIT unavailable(active, error);
            return;
        }
        m_process.start();
        m_timeout.start();
    });
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_error = QStringLiteral("运动矢量提取超时（60 秒）；预览不受影响");
        m_process.kill();
    });
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_output += m_process.readAllStandardOutput();
        if (m_output.size() > 64 * 1024 * 1024) {
            m_output.clear();
            m_error = QStringLiteral("运动矢量输出超过安全大小限制");
            m_process.kill();
        }
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_timeout.stop();
        const int active = m_active;
        m_active = -1;
        if (active >= 0) Q_EMIT unavailable(active, QStringLiteral("无法启动运动矢量提取器：%1").arg(m_process.errorString()));
        if (m_pending >= 0) m_debounce.start();
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int exitCode, QProcess::ExitStatus status) {
            m_timeout.stop();
            const int active = m_active;
            m_active = -1;
            m_output += m_process.readAllStandardOutput();
            const auto diagnostic = QString::fromUtf8(m_process.readAllStandardError()).right(1000);
            QFile::remove(m_temp.filePath(QStringLiteral("input.bsmv")));
            if (active >= 0) {
                if (status != QProcess::NormalExit || exitCode != 0 || !m_error.isEmpty())
                    Q_EMIT unavailable(active, m_error.isEmpty() ? QStringLiteral("运动矢量提取失败：%1").arg(diagnostic) : m_error);
                else {
                    QString error;
                    const auto vectors = parseOutput(m_output, active, error);
                    if (error.isEmpty()) Q_EMIT ready(active, vectors);
                    else Q_EMIT unavailable(active, error);
                }
            }
            m_output.clear();
            if (m_pending >= 0) m_debounce.start();
        });
}

AvcMotionProvider::~AvcMotionProvider()
{
    m_process.disconnect(this);
    cancel();
    m_preparation.waitForFinished();
    if (m_process.state() != QProcess::NotRunning) m_process.waitForFinished(1000);
}

void AvcMotionProvider::cancel()
{
    m_pending = m_active = -1;
    if (m_stop) m_stop->store(true);
    m_debounce.stop();
    m_timeout.stop();
    if (m_process.state() != QProcess::NotRunning) m_process.kill();
}

void AvcMotionProvider::setStream(StreamPtr stream) { cancel(); m_stream = std::move(stream); }

QString AvcMotionProvider::helperPath()
{
    const QString name = QStringLiteral("bitscope-avc-motion")
#ifdef Q_OS_WIN
        + QStringLiteral(".exe")
#endif
        ;
    const QDir dir(QCoreApplication::applicationDirPath());
    const QStringList paths{qEnvironmentVariable("BITSCOPE_AVC_MOTION"), dir.filePath(name),
        dir.filePath(QStringLiteral("tools/") + name), dir.filePath(QStringLiteral("../Resources/tools/") + name)};
    for (const auto &path : paths) if (!path.isEmpty() && QFileInfo(path).isExecutable()) return QFileInfo(path).absoluteFilePath();
    return QStandardPaths::findExecutable(name);
}

void AvcMotionProvider::requestFrame(int frame)
{
    if (!m_stream || (m_stream->codec != Codec::Avc && m_stream->codec != Codec::Mpeg4Visual)
        || frame < 0 || frame >= m_stream->frames.size()
        || m_stream->frames.at(frame).motionVectorsReady || !m_stream->frames.at(frame).motionVectorsError.isEmpty()
        || m_active == frame || m_pending == frame) return;
    cancel();
    m_pending = frame;
    m_debounce.start();
}

void AvcMotionProvider::start()
{
    if (m_process.state() != QProcess::NotRunning || m_preparing || m_pending < 0 || !m_stream) return;
    const int target = m_pending;
    m_pending = -1;
    const auto executable = helperPath();
    if (executable.isEmpty()) {
        Q_EMIT unavailable(target, QStringLiteral("未找到 bitscope-avc-motion 提取器；请使用包含该工具的新版安装包"));
        return;
    }
    int first = target;
    while (first > 0 && !m_stream->frames.at(first).randomAccess) --first;
    if (m_stream->codec == Codec::Mpeg4Visual) first = nearestMpeg4DecodeStart(m_stream, target);
    if (!m_temp.isValid()) { Q_EMIT unavailable(target, QStringLiteral("无法创建运动矢量临时目录")); return; }
    m_output.clear();
    m_error.clear();
    m_active = target;
    m_stop = std::make_shared<std::atomic_bool>(false);
    const QString path = m_temp.filePath(QStringLiteral("input.bsmv"));
    m_process.setProgram(executable);
    m_process.setArguments({path, QString::number(target), m_stream->codec == Codec::Mpeg4Visual ? QStringLiteral("mpeg4") : QStringLiteral("h264")});
    m_preparing = true;
    m_preparation.setFuture(QtConcurrent::run([stream = m_stream, first, target, path, stop = m_stop]() -> QString {
    QFile input(path);
    if (!input.open(QIODevice::WriteOnly)) {
        return QStringLiteral("无法创建运动矢量输入文件：%1").arg(input.errorString());
    }
    input.write("BSMV1\n", 6);
    QDataStream writer(&input); // Big-endian, length-delimited complete access units.
    for (int index = first; index <= target; ++index) {
        if (stop->load()) return QStringLiteral("已取消");
        QByteArray bytes;
        QSet<int> seen;
        QVector<int> nals;
        // Do not prepend parameter sets from later in a changing stream.
        if (index == first) {
            const auto &firstNals = stream->frames.at(first).nalIndices;
            const int limit = firstNals.isEmpty() ? 0 : firstNals.first();
            for (int parameter : stream->parameterNalIndices) if (parameter < limit) nals.append(parameter);
        }
        nals += stream->frames.at(index).nalIndices;
        for (int nal : nals) {
            if (nal < 0 || nal >= stream->nals.size() || seen.contains(nal)) continue;
            seen.insert(nal);
            if (stream->codec == Codec::Mpeg4Visual) bytes.append("\0\0\1", 3);
            else bytes.append("\0\0\0\1", 4);
            bytes += stream->nals.at(nal).payload;
        }
        writer << quint32(index) << quint32(bytes.size());
        if (input.write(bytes) != bytes.size()) { writer.setStatus(QDataStream::WriteFailed); break; }
    }
    if (writer.status() != QDataStream::Ok || !input.flush()) {
        return QStringLiteral("运动矢量输入文件写入失败");
    }
    input.close();
    return {};
    }));
}

QVector<MotionVectorInfo> AvcMotionProvider::parseOutput(const QByteArray &output, int frame, QString &error)
{
    error.clear();
    std::istringstream input(std::string(output.constData(), size_t(output.size())));
    std::string magic;
    int identity = -1, width = 0, height = 0, count = -1;
    if (!(input >> magic >> identity >> width >> height >> count) || magic != "BSMV1" || identity != frame
        || width <= 0 || height <= 0 || width > 32768 || height > 32768 || count < 0 || count > 1000000) {
        error = QStringLiteral("运动矢量帧标识或输出头不匹配，拒绝绑定"); return {};
    }
    QVector<MotionVectorInfo> result;
    result.reserve(count);
    for (int i = 0; i < count; ++i) {
        MotionVectorInfo v;
        int source = 0;
        if (!(input >> source >> v.centerX >> v.centerY >> v.width >> v.height >> v.motionX >> v.motionY >> v.scale)
            || (source != -1 && source != 1) || v.scale <= 0 || v.scale > 65535
            || v.width <= 0 || v.width > 16 || v.height <= 0 || v.height > 16
            || v.centerX < 0 || v.centerX > width + 16 || v.centerY < 0 || v.centerY > height + 16) {
            error = QStringLiteral("运动矢量记录缺失或不合法"); return {};
        }
        v.list = source < 0 ? 0 : 1;
        result.append(v);
    }
    std::string trailing;
    if (input >> trailing) { error = QStringLiteral("运动矢量输出包含意外尾部数据"); return {}; }
    return result;
}
}
