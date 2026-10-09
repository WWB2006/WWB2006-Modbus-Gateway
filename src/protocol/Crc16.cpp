#include "gateway/protocol/Crc16.h"

#include <array>

namespace gateway::protocol {
namespace {

constexpr uint16_t kPolynomial = 0xA001;  // 0x8005 的反射形式
constexpr uint16_t kInitial = 0xFFFF;

// 编译期生成 256 项查表，避免运行期初始化顺序问题。
constexpr std::array<uint16_t, 256> makeTable() {
    std::array<uint16_t, 256> table{};
    for (uint32_t index = 0; index < 256; ++index) {
        uint16_t value = static_cast<uint16_t>(index);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 0x0001u) != 0
                        ? static_cast<uint16_t>((value >> 1) ^ kPolynomial)
                        : static_cast<uint16_t>(value >> 1);
        }
        table[index] = value;
    }
    return table;
}

constexpr std::array<uint16_t, 256> kTable = makeTable();

}  // namespace

uint16_t crc16(const uint8_t* data, std::size_t len) noexcept {
    return crc16(data, len, kInitial);
}

uint16_t crc16(const uint8_t* data, std::size_t len, uint16_t init) noexcept {
    uint16_t crc = init;
    for (std::size_t i = 0; i < len; ++i) {
        const uint8_t index = static_cast<uint8_t>(crc ^ data[i]);
        crc = static_cast<uint16_t>((crc >> 8) ^ kTable[index]);
    }
    return crc;
}

uint16_t crc16Bitwise(const uint8_t* data, std::size_t len, uint16_t init) noexcept {
    uint16_t crc = init;
    for (std::size_t i = 0; i < len; ++i) {
        crc = static_cast<uint16_t>(crc ^ data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x0001u) != 0 ? static_cast<uint16_t>((crc >> 1) ^ kPolynomial)
                                       : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

void appendCrc16(std::vector<uint8_t>& buffer) noexcept {
    const uint16_t crc = crc16(buffer.data(), buffer.size());
    buffer.push_back(static_cast<uint8_t>(crc & 0x00FFu));         // 低字节在前
    buffer.push_back(static_cast<uint8_t>((crc >> 8) & 0x00FFu));  // 高字节在后
}

bool verifyCrc16(const uint8_t* frame, std::size_t len) noexcept {
    if (frame == nullptr || len < 3) {
        return false;
    }
    const uint16_t expect = crc16(frame, len - 2);
    const uint16_t actual =
        static_cast<uint16_t>(frame[len - 2] | (static_cast<uint16_t>(frame[len - 1]) << 8));
    return expect == actual;
}

}  // namespace gateway::protocol
