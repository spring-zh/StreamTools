#pragma once

#include "bitstreamtypes.h"

namespace bitscope {

// FFmpeg libavcodec/mpegutils.c: get_type_mv_char / get_segmentation_char.
// These are debug flags, not the numeric mb_type syntax element. In particular,
// the generic 'i' flag does not distinguish H.264 Intra_4x4 from Intra_8x8.
inline QString avcPredictionName(const QString &code)
{
    if (code.isEmpty()) return QStringLiteral("未获取");
    switch (code.front().toLatin1()) {
    case 'P': return QStringLiteral("I_PCM（原始样本）");
    case 'i': return QStringLiteral("帧内 NxN（4×4 / 8×8 未区分）");
    case 'I': return QStringLiteral("Intra 16×16");
    case 'd': return QStringLiteral("B Skip / Direct");
    case 'D': return QStringLiteral("Direct");
    case 'S': return QStringLiteral("Skip");
    case '>': return QStringLiteral("L0 预测");
    case '<': return QStringLiteral("L1 预测");
    case 'X': return QStringLiteral("L0 + L1 使用标记");
    default: return QStringLiteral("未识别（保留原始标记）");
    }
}

inline QString avcPartitionName(const QString &code)
{
    if (code.isEmpty()) return QStringLiteral("未获取");
    if (!QStringLiteral("PiIdDS><X").contains(code.front())) return QStringLiteral("未获取");
    if (QStringLiteral("PiI").contains(code.front()))
        return QStringLiteral("帧内 / PCM（非帧间分区）");
    if (code.contains('+')) return QStringLiteral("8×8（子分区未导出）");
    if (code.contains('-')) return QStringLiteral("16×8");
    if (code.contains('|')) return QStringLiteral("8×16");
    if (code.contains('?')) return QStringLiteral("未获取");
    return QStringLiteral("16×16");
}

inline QString avcMacroblockExplanation(const QString &code)
{
    return QStringLiteral("%1；%2。来源：FFmpeg 解码日志标记，不是码流中的 mb_type 数值。%3"
                          "L0/L1 是参考列表，不等于时间上的前/后；两列表使用标记不保证每个子分区都双向预测。"
                          "Skip/Direct 的预测参数可由规则推导，不代表复制上一帧同位置。"
                          "MV 由独立提取器按需读取；未获取：具体参考索引、MVP/MVD、帧内方向、变换系数与残差。")
        .arg(avcPredictionName(code), avcPartitionName(code),
             code.contains('=') ? QStringLiteral("含场编码标记；网格位置不是完整的场/MBAFF 地址映射。") : QString());
}

} // namespace bitscope
