#pragma once

#include "bitstreamtypes.h"

#include <QFutureWatcher>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

namespace bitscope {

struct Av1BlockParseResult {
    QVector<QVector<Av1CodingBlockInfo>> frames;
    QVector<int> inputFrames;
    QString error;
};

class Av1BlockProvider final : public QObject {
    Q_OBJECT
public:
    explicit Av1BlockProvider(QObject *parent = nullptr);
    ~Av1BlockProvider() override;
    void setStream(StreamPtr stream);
    void requestFrame(int frameIndex);
    void requestRange(int first, int last);
    QString analyzerPath() const;

Q_SIGNALS:
    void codingBlocksReady(int frameIndex, const QVector<Av1CodingBlockInfo> &blocks);
    void codingBlocksUnavailable(int frameIndex, const QString &reason);
    void statusChanged(const QString &status);

private Q_SLOTS:
    void processQueue();
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void processError(QProcess::ProcessError error);
    void parseFinished();

private:
    int nearestStart(int frameIndex) const;
    QByteArray accessUnit(int frameIndex, bool includeSequenceHeader) const;
    QByteArray ivfSegment(int first, int last) const;
    static Av1BlockParseResult parseOutput(const QByteArray &data,
                                           const StreamPtr &stream,
                                           int activeStart);
    void clearActiveFiles();

    StreamPtr m_stream;
    QProcess m_process;
    QFutureWatcher<Av1BlockParseResult> m_parseWatcher;
    QTemporaryDir m_temp;
    QSet<int> m_pending;
    QSet<int> m_cached;
    QSet<int> m_failed;
    QVector<int> m_activeRequested;
    QVector<int> m_parseRequested;
    int m_activeStart = 0;
    int m_activeEnd = 0;
    int m_activeOutputStart = 0;
    int m_parseStart = 0;
    int m_parseEnd = 0;
    int m_parseOutputStart = 0;
    int m_job = 0;
    int m_priorityFrame = -1;
    bool m_activeInteractive = false;
    bool m_cancelled = false;
    int m_generation = 0;
    int m_parseGeneration = 0;
    QString m_activeInput;
    QString m_activeDirectory;
    QTimer m_queueTimer;
};

} // namespace bitscope
