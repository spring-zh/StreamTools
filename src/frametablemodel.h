#pragma once

#include "bitstreamtypes.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QList>

namespace bitscope {

class FrameTableModel final : public QAbstractTableModel {
    Q_OBJECT
public:
    enum class Mode { Frames, Nals };
    enum Roles { ItemIndexRole = Qt::UserRole + 1, TypeRole, SearchTextRole };

    explicit FrameTableModel(QObject *parent = nullptr);
    void setStream(StreamPtr stream);
    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    StreamPtr stream() const { return m_stream; }
    void setThumbnail(int frameIndex, const QImage &image);
    QImage thumbnail(int frameIndex) const;

    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

Q_SIGNALS:
    void thumbnailEvicted(int frameIndex);

private:
    static QString bytesText(qint64 bytes);
    static QString timeText(double seconds);
    StreamPtr m_stream;
    Mode m_mode = Mode::Frames;
    QHash<int,QImage> m_thumbnails;
    QList<int> m_thumbnailLru;
};

} // namespace bitscope
