#include "bitstreamparser.h"
#include "frametablemodel.h"
#include "hevcorder.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QDebug>
#include <cstdlib>

using namespace bitscope;
static void check(bool ok, const char *message) {
    if (!ok) { qCritical() << message; std::exit(1); }
}
struct Bits {
    QByteArray data;
    int count = 0;
    void put(quint32 value, int n) {
        for (int i=n-1;i>=0;--i) {
            if (!(count%8)) data.append(char(0));
            data[count/8] = char(quint8(data.at(count/8)) | (((value>>i)&1)<<(7-count%8)));
            ++count;
        }
    }
    void ue(unsigned value) {
        const unsigned code=value+1; int width=0;
        for(unsigned n=code;n;n>>=1) ++width;
        put(0,width-1);put(code,width);
    }
    QByteArray nal(int type, int tid=0) {
        put(1,1);while(count%8)put(0,1);
        QByteArray out("\0\0\0\1",4);out.append(char(type<<1));out.append(char(tid+1));
        int zeros=0;
        for(char c:data){if(zeros>=2&&quint8(c)<=3){out.append(char(3));zeros=0;}out.append(c);zeros=c==0?zeros+1:0;}
        return out;
    }
};
static QByteArray sps(bool colour=false) {
    Bits b;b.put(0,4);b.put(0,3);b.put(1,1);
    b.put(1,8);b.put(0,32);b.put(0,32);b.put(0,16);b.put(120,8);
    b.ue(0);b.ue(colour?3:1);if(colour)b.put(1,1);
    b.ue(192);b.ue(64);b.put(1,1);
    b.ue(0);b.ue(colour?128:64);b.ue(0);b.ue(0); // crop to 64, but 3 coded CTUs
    b.ue(0);b.ue(0);b.ue(0); // 4 POC bits
    b.put(0,1);b.ue(3);b.ue(2);b.ue(0);
    b.ue(0);b.ue(3);b.ue(0);b.ue(0);b.ue(0);b.ue(0);
    return b.nal(33);
}
static QByteArray pps() {
    Bits b;b.ue(0);b.ue(0);b.put(1,1);b.put(1,1);b.put(3,3);return b.nal(34);
}
static QByteArray slice(int type,int lsb,int raw=1,int tid=0,bool first=true,bool dep=false,bool colour=false) {
    Bits b;b.put(first,1);if(type>=16&&type<=23)b.put(0,1);b.ue(0);
    if(!first){b.put(dep,1);b.put(2,2);}
    if(!dep){b.put(5,3);b.ue(raw);b.put(1,1);if(colour)b.put(2,2);if(type!=19&&type!=20)b.put(lsb,4);}
    return b.nal(type,tid);
}
static StreamPtr parse(const QByteArray &bytes) {
    QTemporaryDir dir;check(dir.isValid(),"temporary directory");
    QFile f(dir.filePath("sample.h265"));check(f.open(QIODevice::WriteOnly),"create fixture");
    check(f.write(bytes)==bytes.size(),"write fixture");f.close();
    auto stream=BitstreamParser::parseFile(f.fileName());
    check(stream->error.isEmpty(),qPrintable(stream->error));return stream;
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    HevcPocState state;
    check(state.derive(19,0,0,4)==0,"IDR zero");
    check(state.derive(1,0,7,4)==7,"T0 reference");
    check(state.derive(1,0,14,4)==14,"T0 reference before wrap");
    check(state.derive(3,1,1,4)==17,"temporal layer wrap");
    check(state.previous==14,"T1 must not update prevTid0");
    check(state.derive(1,0,2,4)==18,"forward LSB wrap");
    check(state.derive(9,0,15,4)==15&&state.previous==18,"RASL does not update prevTid0");
    check(state.derive(6,0,0,4)==16&&state.previous==18,"RADL does not update prevTid0");
    check(state.derive(0,0,3,4)==19&&state.previous==18,"nonreference T0 exclusion");
    check(state.derive(21,0,5,4)==21,"midstream CRA keeps MSB");
    check(state.derive(16,0,3,4)==3,"BLA resets MSB");
    check(state.derive(20,0,0,4)==0,"second IDR resets POC");
    state={};check(state.derive(21,0,2,4)==2,"initial CRA resets MSB");
    check(state.derive(9,0,15,4)==-1,"negative leading POC");

    auto stream=parse(sps()+pps()+slice(19,0,2)+slice(1,7)+slice(1,14)
        +slice(1,2)+slice(1,2,1,0,false,true)+slice(1,2,1,0,false,false)
        +slice(0,1,0,1)+slice(21,5,2)+slice(20,0,2));
    const QVector<int> expected{0,7,14,18,17,21,0};
    check(stream->frames.size()==expected.size(),"multi-slice AU grouping");
    for(int i=0;i<expected.size();++i)check(stream->frames.at(i).pocValid&&stream->frames.at(i).poc==expected[i],"full reconstructed POC");
    check(stream->frames[1].type=="P"&&stream->frames[4].type=="B","HEVC type numbering (not AVC)");
    check(stream->frames[3].sliceNalIndices.size()==3,"dependent and independent slices");
    for(int ni:stream->frames[3].sliceNalIndices) {
        const auto &s=stream->nals[ni].slice;
        check(s.pocValid&&s.poc==18&&s.type=="P","slice POC inheritance");
    }
    check(stream->nals[stream->frames[3].sliceNalIndices[1]].slice.address==2,"address uses coded dimensions before cropping");
    auto colour=parse(sps(true)+pps()+slice(21,3,2,0,true,false,true));
    check(colour->frames.first().poc==3,"extra header/output/colour-plane bits before POC");
    auto leading=parse(sps()+pps()+slice(21,2,2)+slice(9,15,0));
    check(leading->frames[1].pocValid&&leading->frames[1].poc==-1,"negative POC validity");
    FrameTableModel model;model.setStream(leading);
    check(model.columnCount()==8,"type and structure share one column");
    check(model.data(model.index(1,2)).toString()==QStringLiteral("B（完整帧）"),"structure in parentheses");
    check(model.data(model.index(1,2),Qt::ToolTipRole)==model.data(model.index(1,2)),"full combined tooltip");
    check(model.data(model.index(1,2),FrameTableModel::TypeRole).toString()=="B","type filtering remains unchanged");
    check(model.headerData(4,Qt::Horizontal,Qt::DisplayRole).toString()=="POC","POC column moved");
    check(model.headerData(6,Qt::Horizontal,Qt::DisplayRole).toString()==QStringLiteral("偏移"),"offset column moved");
    check(model.data(model.index(1,4)).toInt()==-1,"negative POC displayed instead of dash");
    check(model.data(model.index(1,4),Qt::ToolTipRole).toString()==leading->frames[1].pocDescription,"POC tooltip moved");
    check(model.data(model.index(1,6)).toString().startsWith("0X"),"offset value moved");
    leading->frames[1].structure.clear();check(model.data(model.index(1,2)).toString()=="B","omit empty parentheses");
    leading->codec=Codec::Av1;check(model.headerData(4,Qt::Horizontal,Qt::DisplayRole).toString()=="Order Hint","AV1 column moved");
    leading->frames[1].displayType="INTER";leading->frames[1].structure="Frame OBU";
    check(model.data(model.index(1,2)).toString()==QStringLiteral("INTER（Frame OBU）"),"AV1 combined type");
    model.setMode(FrameTableModel::Mode::Nals);
    check(model.columnCount()==6&&model.headerData(3,Qt::Horizontal,Qt::DisplayRole).toString()==QStringLiteral("偏移"),"NAL columns unchanged");
    auto missing=parse(slice(1,4));check(!missing->frames.first().pocValid,"missing PPS must not fabricate POC");
    Bits eos;
    auto reset=parse(sps()+pps()+slice(19,0,2)+slice(1,7)+slice(1,14)+slice(1,2)+eos.nal(36)+slice(21,5,2));
    check(reset->frames.last().poc==5,"EOS resets CRA context");
    auto conflict=parse(sps()+pps()+slice(19,0,2)+slice(1,2)+slice(1,3,1,0,false));
    check(!conflict->frames.last().pocValid,"inconsistent slice POC rejected");
    if(argc>1){
        auto real=BitstreamParser::parseFile(QString::fromLocal8Bit(argv[1]));
        check(real->error.isEmpty()&&!real->frames.isEmpty(),"real HEVC fixture");
        for(const auto &f:real->frames){
            check(f.pocValid,"real frame has POC");
            if(argc>2&&QString::fromLocal8Bit(argv[2])=="--timeline")
                check(f.poc==qRound(f.pts*real->fps),"reconstructed POC matches independent MP4 display timeline");
            if(f.index<10||f.index>=real->frames.size()-5)qInfo()<<f.index<<f.type<<f.poc;
        }
        qInfo()<<"Validated POC for"<<real->frames.size()<<"real frames";
    }
    qInfo()<<"PASS: HEVC slice headers, signed POC, wrap/reset/TID, dependent slices and table columns";
}
