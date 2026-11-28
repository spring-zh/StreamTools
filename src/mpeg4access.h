#pragma once

#include "bitstreamtypes.h"

namespace bitscope {

// The B-VOPs immediately following an I-VOP in coded order may precede it
// in presentation order. The MPEG-4 decoder skips them when its previous
// reference is absent. A plain I-VOP is not an H.264-style IDR boundary.
inline bool mpeg4LeadingBFrame(const StreamPtr &stream, int start, int target)
{
    if (!stream || target <= start || target >= stream->frames.size()
        || stream->frames.at(target).type != QStringLiteral("B"))
        return false;
    for (int i = start + 1; i < target; ++i)
        if (stream->frames.at(i).referencePicture) return false;
    return true;
}

inline int nearestMpeg4DecodeStart(const StreamPtr &stream, int target)
{
    if (!stream || target <= 0) return 0;
    for (int i = target; i >= 0; --i) {
        if (!stream->frames.at(i).randomAccess) continue;
        // Only a requested leading B needs the preceding GOP's reference.
        // Ordinary I/P/B requests retain the nearest-I fast path.
        if (mpeg4LeadingBFrame(stream, i, target)) continue;
        return i;
    }
    return 0;
}

inline QVector<int> mpeg4PreviewOutputOrder(const StreamPtr &stream, int start, int end)
{
    QVector<int> result;
    bool havePreviousReference = false;
    for (int i = start; i <= end; ++i) {
        const FrameInfo &frame = stream->frames.at(i);
        if (i > start && frame.referencePicture) havePreviousReference = true;
        if (frame.type == QStringLiteral("B") && !havePreviousReference) continue;
        result.push_back(i);
    }
    return result;
}

} // namespace bitscope
