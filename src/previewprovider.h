#pragma once

#include "bitstreamtypes.h"

#include <QObject>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

namespace bitscope {

class PreviewProvider final : public QObject {
    Q_OBJECT
public:
    explicit PreviewProvider(QObject *parent = nullptr);
    ~PreviewProvider() override;
    void setStream(StreamPtr stream);
    void requestFrame(int frameIndex);
    void requestRange(int first, int last);
    void forgetFrame(int frameIndex);
    QString ffmpegPath() const;
    bool isIdle() const;

Q_SIGNALS:
    void thumbnailReady(int frameIndex, const QImage &image);
    void decodedUnitsReady(int frameIndex, const QVector<DecodedUnitInfo> &units);
    void decodedUnitsUnavailable(int frameIndex, const QString &reason);
    void statusChanged(const QString &status);
    void previewFailed(int frameIndex, const QString &reason);
    void queueIdle();

private Q_SLOTS:
    void processQueue();
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void processError(QProcess::ProcessError error);

private:
    int nearestStart(int frameIndex) const;
    QByteArray accessUnit(int frameIndex, bool includeParameters) const;
    QByteArray segment(int first, int last) const;
    void clearActiveFiles();
    QVector<QVector<DecodedUnitInfo>> parseAvcDecodedUnits(const QByteArray &log,
                                                           int expectedFrames) const;

    StreamPtr m_stream;
    QProcess m_process;
    QTemporaryDir m_temp;
    QSet<int> m_pending;
    QSet<int> m_cached;
    QSet<int> m_failed;
    QVector<int> m_activeRequested;
    QVector<int> m_activeOutputOrder;
    int m_activeStart = 0;
    int m_activeEnd = 0;
    int m_priorityFrame = -1;
    int m_job = 0;
    QString m_activeInput;
    QString m_activePrefix;
    QTimer m_queueTimer;
};

} // namespace bitscope
