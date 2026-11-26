#include "frametablemodel.h"

#include <QBrush>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QSize>
#include <cmath>

namespace bitscope {

FrameTableModel::FrameTableModel(QObject *parent) : QAbstractTableModel(parent) {}

void FrameTableModel::setStream(StreamPtr stream) {
    beginResetModel();m_stream=std::move(stream);m_thumbnails.clear();m_thumbnailLru.clear();endResetModel();
}

void FrameTableModel::setMode(Mode mode) {
    if(m_mode==mode)return;beginResetModel();m_mode=mode;endResetModel();
}

void FrameTableModel::setThumbnail(int frameIndex,const QImage &image) {
    if(image.isNull())return;m_thumbnails.insert(frameIndex,image);m_thumbnailLru.removeAll(frameIndex);m_thumbnailLru.append(frameIndex);constexpr int maxCachedThumbnails=180;while(m_thumbnailLru.size()>maxCachedThumbnails){const int evicted=m_thumbnailLru.takeFirst();m_thumbnails.remove(evicted);Q_EMIT thumbnailEvicted(evicted);if(m_mode==Mode::Frames&&evicted>=0&&evicted<rowCount())Q_EMIT dataChanged(index(evicted,1),index(evicted,1),{Qt::DecorationRole,Qt::DisplayRole});}if(m_mode==Mode::Frames&&frameIndex>=0&&frameIndex<rowCount())Q_EMIT dataChanged(index(frameIndex,1),index(frameIndex,1),{Qt::DecorationRole,Qt::DisplayRole});
}

QImage FrameTableModel::thumbnail(int frameIndex) const { return m_thumbnails.value(frameIndex); }

int FrameTableModel::rowCount(const QModelIndex &parent) const { return parent.isValid()||!m_stream?0:(m_mode==Mode::Frames?m_stream->frames.size():m_stream->nals.size()); }
int FrameTableModel::columnCount(const QModelIndex &parent) const { return parent.isValid()?0:(m_mode==Mode::Frames?9:6); }

QString FrameTableModel::bytesText(qint64 n){if(n>=1048576)return QStringLiteral("%1 MB").arg(n/1048576.0,0,'f',2);if(n>=1024)return QStringLiteral("%1 KB").arg(n/1024.0,0,'f',1);return QStringLiteral("%1 B").arg(n);}
QString FrameTableModel::timeText(double seconds){const int ms=int(std::round(seconds*1000.0)),h=ms/3600000,m=(ms/60000)%60,s=(ms/1000)%60;return QStringLiteral("%1:%2:%3.%4").arg(h,2,10,QChar('0')).arg(m,2,10,QChar('0')).arg(s,2,10,QChar('0')).arg(ms%1000,3,10,QChar('0'));}

QVariant FrameTableModel::data(const QModelIndex &idx,int role) const {
    if(!idx.isValid()||!m_stream)return{};
    if(m_mode==Mode::Frames){const auto &f=m_stream->frames.at(idx.row());
        if(role==ItemIndexRole)return f.index;if(role==TypeRole)return f.randomAccess?QStringLiteral("@random %1").arg(f.displayType):f.displayType;if(role==SearchTextRole)return QStringLiteral("%1 %2 %3 %4").arg(f.index).arg(f.displayType,f.structure).arg(f.offset,0,16);
        if(role==Qt::SizeHintRole&&idx.column()==1)return QSize(104,58);
        if(role==Qt::DecorationRole&&idx.column()==1){const auto image=m_thumbnails.value(f.index);if(!image.isNull())return QPixmap::fromImage(image.scaled(96,54,Qt::KeepAspectRatio,Qt::SmoothTransformation));return{};}
        if(role==Qt::TextAlignmentRole)return idx.column()==0||idx.column()==6||idx.column()==7?Qt::AlignCenter:Qt::AlignVCenter;
        if(role==Qt::ForegroundRole&&idx.column()==2){if(f.randomAccess)return QBrush(QColor("#42d392"));if(f.displayType.contains(QStringLiteral("INTRA"))||(m_stream->codec!=Codec::Av1&&f.displayType.contains('I')))return QBrush(QColor("#f4a261"));if(f.displayType.contains('B'))return QBrush(QColor("#9b87f5"));return QBrush(QColor("#58a6ff"));}
        if(role==Qt::FontRole&&(idx.column()==0||idx.column()==5)){QFont font(QStringLiteral("Menlo"));font.setStyleHint(QFont::Monospace);font.setPointSize(9);return font;}
        if(role!=Qt::DisplayRole&&role!=Qt::ToolTipRole)return{};
        switch(idx.column()){case 0:return QStringLiteral("%1").arg(f.index,4,10,QChar('0'));case 1:return m_thumbnails.contains(f.index)?QString():QStringLiteral("按需加载");case 2:return f.displayType;case 3:return f.structure;case 4:return timeText(f.pts);case 5:return QStringLiteral("0x%1").arg(f.offset,8,16,QChar('0')).toUpper();case 6:return bytesText(f.size);case 7:return f.poc>=0?QVariant(f.poc):QVariant(QStringLiteral("—"));case 8:return f.references.isEmpty()?(f.index==0&&f.displayType.contains('P')?QStringLiteral("⚠ 文件外"):QStringLiteral("—")):QStringLiteral("L0 #%1").arg(f.references.first());default:return{};}
    }
    const auto &n=m_stream->nals.at(idx.row());if(role==ItemIndexRole)return n.index;if(role==TypeRole)return n.category;if(role==SearchTextRole)return QStringLiteral("%1 %2 %3 %4").arg(n.index).arg(n.name,n.category).arg(n.offset,0,16);if(role==Qt::ForegroundRole&&idx.column()==1)return QBrush(n.category=="PARAM"?QColor("#f4a261"):n.category=="SEI"?QColor("#f4a261"):QColor("#8fa1b5"));if(role==Qt::FontRole&&(idx.column()==0||idx.column()==3)){QFont font(QStringLiteral("Menlo"));font.setStyleHint(QFont::Monospace);font.setPointSize(9);return font;}if(role!=Qt::DisplayRole&&role!=Qt::ToolTipRole)return{};
    switch(idx.column()){case 0:return QStringLiteral("%1").arg(n.index,4,10,QChar('0'));case 1:return n.category;case 2:return m_stream->codec==Codec::Mpeg4Visual?(QStringLiteral("0x%1").arg(n.nalType,2,16,QChar('0')).toUpper()+QStringLiteral(" · ")+n.name):QStringLiteral("%1 · %2").arg(n.nalType).arg(n.name);case 3:return QStringLiteral("0x%1").arg(n.offset,8,16,QChar('0')).toUpper();case 4:return bytesText(n.size);case 5:return n.temporalId>=0?QVariant(n.temporalId):QVariant(QStringLiteral("—"));default:return{};}
}

QVariant FrameTableModel::headerData(int section,Qt::Orientation orientation,int role) const {
    if(orientation!=Qt::Horizontal||role!=Qt::DisplayRole)return{};static const QStringList frameHeaders{QStringLiteral("#"),QStringLiteral("帧预览"),QStringLiteral("类型"),QStringLiteral("结构"),QStringLiteral("PTS"),QStringLiteral("偏移"),QStringLiteral("大小"),QStringLiteral("POC"),QStringLiteral("参考")};static const QStringList av1FrameHeaders{QStringLiteral("#"),QStringLiteral("帧预览"),QStringLiteral("类型"),QStringLiteral("结构"),QStringLiteral("PTS"),QStringLiteral("偏移"),QStringLiteral("大小"),QStringLiteral("Order Hint"),QStringLiteral("参考")};static const QStringList nalHeaders{QStringLiteral("#"),QStringLiteral("分类"),QStringLiteral("NAL 单元类型"),QStringLiteral("偏移"),QStringLiteral("大小"),QStringLiteral("TID")};static const QStringList mpeg4Headers{QStringLiteral("#"),QStringLiteral("分类"),QStringLiteral("MPEG-4 起始码单元"),QStringLiteral("偏移"),QStringLiteral("大小"),QStringLiteral("层")};static const QStringList av1Headers{QStringLiteral("#"),QStringLiteral("分类"),QStringLiteral("AV1 OBU 类型"),QStringLiteral("偏移"),QStringLiteral("大小"),QStringLiteral("TID")};if(m_mode==Mode::Frames)return (m_stream&&m_stream->codec==Codec::Av1?av1FrameHeaders:frameHeaders).value(section);return (m_stream&&m_stream->codec==Codec::Mpeg4Visual?mpeg4Headers:(m_stream&&m_stream->codec==Codec::Av1?av1Headers:nalHeaders)).value(section);
}

} // namespace bitscope
