#pragma once
#include "bitstreamtypes.h"
#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QFutureWatcher>
#include <atomic>

namespace bitscope {
class AvcMotionProvider final : public QObject {
    Q_OBJECT
public:
    explicit AvcMotionProvider(QObject *parent = nullptr);
    ~AvcMotionProvider() override;
    void setStream(StreamPtr stream);
    void requestFrame(int frame);
    void cancel();
    static QString helperPath();
    static QVector<MotionVectorInfo> parseOutput(const QByteArray &output, int frame, QString &error);
Q_SIGNALS:
    void ready(int frame, const QVector<MotionVectorInfo> &vectors);
    void unavailable(int frame, const QString &reason);
private:
    void start();
    StreamPtr m_stream;
    QTemporaryDir m_temp;
    QProcess m_process;
    QTimer m_debounce, m_timeout;
    int m_pending = -1, m_active = -1;
    QString m_error;
    QByteArray m_output;
    QFutureWatcher<QString> m_preparation;
    bool m_preparing = false;
    std::shared_ptr<std::atomic_bool> m_stop;
};
}
