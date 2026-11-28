#pragma once

#include "bitstreamtypes.h"

namespace bitscope {

// H.265 8.3.1: only an eligible TemporalId=0 picture updates prevTid0Pic.
// Track validity separately: -1 is a legitimate POC for leading pictures.
struct HevcPocState {
    int previous = 0;
    bool first = true;
    bool anchored = false;

    int derive(int type, int temporalId, int lsb, int bits) {
        const bool idr = type == 19 || type == 20;
        const bool bla = type >= 16 && type <= 18;
        const bool reset = idr || bla || (type == 21 && first);
        const int modulus = 1 << bits;
        // Positive modulo also works when the previous POC was negative.
        const int prevLsb = ((previous % modulus) + modulus) % modulus;
        int msb = previous - prevLsb;
        if (reset || first) msb = 0;
        else if (lsb < prevLsb && prevLsb - lsb >= modulus / 2) msb += modulus;
        else if (lsb > prevLsb && lsb - prevLsb > modulus / 2) msb -= modulus;
        const int poc = idr ? 0 : msb + lsb;
        anchored |= reset;
        first = false;
        if (temporalId == 0 && type != 0 && type != 2 && type != 4
            && !(type >= 6 && type <= 9)) previous = poc;
        return poc;
    }
};

inline void deriveHevcPocs(StreamInfo &stream) {
    HevcPocState state;
    for (auto &frame : stream.frames) {
        // EOS/EOB are kept with the following access unit by the parser.
        for (int ni : frame.nalIndices) {
            if (stream.nals.at(ni).hasSlice) break;
            if (stream.nals.at(ni).nalType == 36 || stream.nals.at(ni).nalType == 37) state = {};
        }
        SliceInfo *independent = nullptr;
        const NalUnit *picture = nullptr;
        bool consistent = true;
        for (int ni : frame.sliceNalIndices) {
            auto &nal = stream.nals[ni];
            auto &s = nal.slice;
            if (s.dependentSlice) continue;
            if (s.pocLsb < 0 || s.pocBits < 4 || s.pocBits > 16) { consistent = false; continue; }
            if (!independent) { independent = &s; picture = &nal; }
            else if (s.pocLsb != independent->pocLsb || s.pocBits != independent->pocBits
                     || nal.nalType != picture->nalType || nal.temporalId != picture->temporalId)
                consistent = false;
        }
        frame.poc = -1;
        frame.pocValid = false;
        if (!consistent || !independent) {
            frame.pocDescription = QStringLiteral("POC 未获取：Slice 头缺失、参数集缺失或分片信息不一致");
            // Do not silently carry an outdated MSB through an unparsed picture.
            state = {};
            continue;
        }
        frame.poc = state.derive(picture->nalType, picture->temporalId,
                                 independent->pocLsb, independent->pocBits);
        frame.type = independent->type;
        frame.pocValid = true;
        frame.pocDescription = QStringLiteral("PicOrderCntVal = %1；POC LSB = %2，%3 bits。%4")
            .arg(frame.poc).arg(independent->pocLsb).arg(independent->pocBits)
            .arg(state.anchored ? QStringLiteral("按解码顺序及 prevTid0Pic 推导，非显示序号。")
                               : QStringLiteral("起点缺少随机访问上下文，MSB 以当前片段为基准。"));
        for (int ni : frame.sliceNalIndices) {
            auto &nal = stream.nals[ni];
            auto &s = nal.slice;
            if (s.dependentSlice) {
                s.type = independent->type;
                s.rawType = independent->rawType;
                s.numRefL0 = independent->numRefL0;
                s.numRefL1 = independent->numRefL1;
            }
            s.poc = frame.poc;
            s.pocValid = true;
            s.fields.push_back({QStringLiteral("PicOrderCntVal"), QString::number(s.poc),
                                QStringLiteral("derived"), frame.pocDescription});
            nal.fields = s.fields;
        }
    }
}
} // namespace bitscope
