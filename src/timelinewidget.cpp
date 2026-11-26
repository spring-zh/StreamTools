#include "timelinewidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace bitscope {

TimelineWidget::TimelineWidget(QWidget *parent):QWidget(parent){setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);setMinimumHeight(125);}
void TimelineWidget::setStream(StreamPtr stream){m_stream=std::move(stream);m_selected=0;update();}
void TimelineWidget::setSelectedFrame(int frameIndex){m_selected=frameIndex;update();}
void TimelineWidget::setZoom(double zoom){m_zoom=std::clamp(zoom,.5,5.0);update();}
QColor TimelineWidget::frameColor(const FrameInfo &f) const {if(f.randomAccess)return QColor("#42d392");if(f.displayType.contains('I'))return QColor("#f4a261");if(f.displayType.contains('B'))return QColor("#9b87f5");return QColor("#58a6ff");}
QPair<int,int> TimelineWidget::visibleRange() const {if(!m_stream||m_stream->frames.isEmpty())return{0,0};const int frameCount=int(m_stream->frames.size()),count=std::min(frameCount,std::max(24,int(140/m_zoom))),start=std::clamp(m_selected-count/2,0,std::max(0,frameCount-count));return{start,start+count};}

void TimelineWidget::paintEvent(QPaintEvent *){QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor("#0d131c"));p.setPen(QColor("#dbe5ef"));p.drawText(QRectF(14,8,width()-28,18),Qt::AlignLeft|Qt::AlignVCenter,QStringLiteral("帧时间线"));if(!m_stream||m_stream->frames.isEmpty())return;const auto range=visibleRange();const int start=range.first,end=range.second;m_trackRect=QRectF(14,38,width()-28,height()-52);p.setPen(QColor("#243343"));p.drawLine(m_trackRect.topLeft(),m_trackRect.topRight());qint64 maxSize=1;for(int i=start;i<end;++i)maxSize=std::max(maxSize,m_stream->frames.at(i).size);const double barW=m_trackRect.width()/std::max(1,end-start);
    auto centerX=[&](int index){return m_trackRect.left()+(index-start+.5)*barW;};
    if(m_selected>=start&&m_selected<end){const auto &selected=m_stream->frames.at(m_selected);p.setPen(QPen(QColor("#19c2c9"),1.4));for(int ref:selected.references)if(ref>=start&&ref<end){QPainterPath path;path.moveTo(centerX(ref),m_trackRect.top()+18);path.quadTo((centerX(ref)+centerX(m_selected))/2,m_trackRect.top()-18,centerX(m_selected),m_trackRect.top()+18);p.drawPath(path);}p.setPen(QPen(QColor("#9b87f5"),1));for(int next:selected.referencedBy)if(next>=start&&next<end){QPainterPath path;path.moveTo(centerX(m_selected),m_trackRect.top()+20);path.quadTo((centerX(next)+centerX(m_selected))/2,m_trackRect.top()-10,centerX(next),m_trackRect.top()+20);p.drawPath(path);}}
    for(int i=start;i<end;++i){const auto &f=m_stream->frames.at(i);const double normalized=std::sqrt(double(f.size)/maxSize),h=18+normalized*(m_trackRect.height()-25);QRectF bar(m_trackRect.left()+(i-start)*barW+1,m_trackRect.bottom()-h,std::max(2.0,barW-2),h);QColor color=frameColor(f);color.setAlpha(i==m_selected?255:195);p.fillRect(bar,color);if(f.recoveryPoint)p.fillRect(QRectF(bar.left(),bar.top(),bar.width(),3),QColor("#f4a261"));if(f.sliceNalIndices.size()>1){p.setPen(QPen(QColor(255,255,255,100),.7));for(double y=bar.top();y<bar.bottom();y+=5)p.drawLine(QPointF(bar.left(),y),QPointF(bar.right(),std::min(bar.bottom(),y+5)));}if(i==m_selected){p.setPen(QPen(Qt::white,1));p.setBrush(Qt::NoBrush);p.drawRect(bar.adjusted(-1,-1,1,1));}}
    p.setPen(QColor("#607184"));for(int tick=0;tick<=8;++tick){const int frame=start+(end-start-1)*tick/8;const double x=m_trackRect.left()+m_trackRect.width()*tick/8.0;p.drawLine(QPointF(x,25),QPointF(x,31));p.drawText(QRectF(x-42,13,84,12),Qt::AlignCenter,QStringLiteral("F%1").arg(frame));}
}

void TimelineWidget::mousePressEvent(QMouseEvent *event){if(!m_stream||!m_trackRect.contains(event->position()))return;const auto [start,end]=visibleRange();const int index=std::clamp(start+int((event->position().x()-m_trackRect.left())/m_trackRect.width()*(end-start)),start,end-1);Q_EMIT frameActivated(index);}

} // namespace bitscope
