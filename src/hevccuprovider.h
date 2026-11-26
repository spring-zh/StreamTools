#pragma once

#include "bitstreamtypes.h"

#include <QObject>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

namespace bitscope {

class HevcCuProvider final : public QObject {
    Q_OBJECT
public:
    explicit HevcCuProvider(QObject *parent = nullptr);
    ~HevcCuProvider() override;
    void setStream(StreamPtr stream);
    void requestFrame(int frameIndex);
    void requestRange(int first, int last);
    QString decoderPath() const;

Q_SIGNALS:
    void codingUnitsReady(int frameIndex, const QVector<HevcCodingUnitInfo> &units);
    void codingUnitsUnavailable(int frameIndex, const QString &reason);
    void statusChanged(const QString &status);

private Q_SLOTS:
    void processQueue();
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void processError(QProcess::ProcessError error);

private:
    int nearestStart(int frameIndex) const;
    QByteArray accessUnit(int frameIndex, bool includeParameters) const;
    QByteArray segment(int first, int last) const;
    QVector<QVector<HevcCodingUnitInfo>> parseOutput(QString *error) const;
    void clearActiveFiles();

    StreamPtr m_stream;
    QProcess m_process;
    QTemporaryDir m_temp;
    QSet<int> m_pending;
    QSet<int> m_cached;
    QSet<int> m_failed;
    QVector<int> m_activeRequested;
    int m_activeStart = 0;
    int m_activeEnd = 0;
    int m_job = 0;
    int m_priorityFrame = -1;
    QString m_activeInput;
    QString m_activeDirectory;
    QTimer m_queueTimer;
};

} // namespace bitscope
