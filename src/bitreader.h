#pragma once

#include <QByteArray>
#include <QtGlobal>

#include <stdexcept>

namespace bitscope {

class BitReader {
public:
    explicit BitReader(QByteArray data) : m_data(std::move(data)) {}

    int bitsLeft() const { return m_data.size() * 8 - m_bit; }

    quint32 readBits(int count) {
        if (count < 0 || count > 32 || bitsLeft() < count)
            throw std::runtime_error("RBSP data exhausted");
        quint32 value = 0;
        for (int i = 0; i < count; ++i) {
            value = (value << 1U) | ((quint8(m_data.at(m_bit >> 3)) >> (7 - (m_bit & 7))) & 1U);
            ++m_bit;
        }
        return value;
    }

    bool flag() { return readBits(1) != 0; }

    quint32 ue() {
        int zeros = 0;
        while (bitsLeft() > 0 && readBits(1) == 0) {
            if (++zeros > 31)
                throw std::runtime_error("invalid Exp-Golomb code");
        }
        return zeros == 0 ? 0U : ((1U << zeros) - 1U + readBits(zeros));
    }

    qint32 se() {
        const quint32 code = ue();
        return (code & 1U) ? qint32((code + 1U) / 2U) : -qint32(code / 2U);
    }

    void skip(int count) {
        if (bitsLeft() < count)
            throw std::runtime_error("RBSP data exhausted");
        m_bit += count;
    }

private:
    QByteArray m_data;
    int m_bit = 0;
};

inline QByteArray rbsp(const QByteArray &payload, int headerBytes) {
    QByteArray out;
    out.reserve(payload.size());
    int zeroCount = 0;
    for (int i = headerBytes; i < payload.size(); ++i) {
        const quint8 value = quint8(payload.at(i));
        if (zeroCount >= 2 && value == 0x03) {
            zeroCount = 0;
            continue;
        }
        out.append(char(value));
        zeroCount = value == 0 ? zeroCount + 1 : 0;
    }
    return out;
}

} // namespace bitscope
