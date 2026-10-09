#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gateway::protocol {

// Modbus CRC-16。
// 多项式 0x8005 的反射形式为 0xA001，初值 0xFFFF，不做最终异或。
// 返回值为 16 位数值：低字节先发送（RTU 帧尾的 CRC 字段是低字节在前）。
uint16_t crc16(const uint8_t* data, std::size_t len) noexcept;
uint16_t crc16(const uint8_t* data, std::size_t len, uint16_t init) noexcept;

// 逐位实现，只用于对照测试与教学：查表版本必须与它结果一致。
uint16_t crc16Bitwise(const uint8_t* data, std::size_t len, uint16_t init = 0xFFFF) noexcept;

// 按 RTU 的字节序（低字节在前）把 CRC 追加到缓冲区末尾。
void appendCrc16(std::vector<uint8_t>& buffer) noexcept;

// 校验整帧：最后两个字节是 CRC，其余参与计算。
bool verifyCrc16(const uint8_t* frame, std::size_t len) noexcept;

}  // namespace gateway::protocol
