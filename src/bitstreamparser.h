#pragma once

#include "bitstreamtypes.h"

#include <QString>

namespace bitscope {

class BitstreamParser {
public:
    static StreamPtr parseFile(const QString &path);
};

} // namespace bitscope

