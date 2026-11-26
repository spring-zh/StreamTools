#pragma once

#include "bitstreamtypes.h"

#include <algorithm>

namespace bitscope {

inline bool hevcFrameHasNalType(const StreamPtr &stream, int frameIndex,
                                int firstType, int lastType)
{
    if (!stream || frameIndex < 0 || frameIndex >= stream->frames.size())
        return false;
    const FrameInfo &frame = stream->frames.at(frameIndex);
    return std::any_of(frame.sliceNalIndices.cbegin(), frame.sliceNalIndices.cend(),
                       [&stream, firstType, lastType](int nalIndex) {
        if (nalIndex < 0 || nalIndex >= stream->nals.size())
            return false;
        const int type = stream->nals.at(nalIndex).nalType;
        return type >= firstType && type <= lastType;
    });
}

inline bool hevcFrameIsCra(const StreamPtr &stream, int frameIndex)
{
    return hevcFrameHasNalType(stream, frameIndex, 21, 21);
}

inline bool hevcFrameIsRasl(const StreamPtr &stream, int frameIndex)
{
    return hevcFrameHasNalType(stream, frameIndex, 8, 9);
}

// When a decoder starts at a CRA, associated RASL pictures are present later
// in decoding order but precede the CRA in display order. They require the
// previous GOP and are deliberately not output at this random-access point.
inline bool hevcFrameSuppressedAtStart(const StreamPtr &stream, int startFrame,
                                       int frameIndex)
{
    return frameIndex > startFrame && hevcFrameIsCra(stream, startFrame)
        && hevcFrameIsRasl(stream, frameIndex);
}

inline int nearestHevcDecodeStart(const StreamPtr &stream, int frameIndex)
{
    if (!stream || frameIndex <= 0)
        return 0;
    for (int index = frameIndex; index >= 0; --index) {
        const FrameInfo &candidate = stream->frames.at(index);
        if (candidate.randomAccess) {
            // A leading RASL picture cannot be decoded by starting at its CRA;
            // continue to the preceding random-access point instead.
            if (hevcFrameIsCra(stream, index) && frameIndex > index
                && hevcFrameIsRasl(stream, frameIndex)) {
                continue;
            }
            return index;
        }
        if (candidate.recoveryPoint
            && index + std::max(0, candidate.recoveryFrameCount) <= frameIndex) {
            return index;
        }
    }
    return 0;
}

inline int hevcDecoderOutputOrdinal(const StreamPtr &stream, int startFrame,
                                    int frameIndex)
{
    int ordinal = -1;
    for (int index = startFrame; index <= frameIndex; ++index) {
        if (!hevcFrameSuppressedAtStart(stream, startFrame, index))
            ++ordinal;
    }
    return ordinal;
}

} // namespace bitscope
