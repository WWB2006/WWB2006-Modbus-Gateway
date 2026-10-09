#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gateway/protocol/ModbusFrame.h"

namespace gateway::protocol {

// 协议上限（来自 Modbus 应用协议规范）。
constexpr uint16_t kMaxReadRegisters = 125;
constexpr uint16_t kMaxWriteRegisters = 123;
constexpr uint16_t kMaxReadBits = 2000;
constexpr uint16_t kMaxWriteBits = 1968;
constexpr std::size_t kMbapHeaderSize = 7;
constexpr std::size_t kMaxPduSize = 253;

// ---------------- PDU 构造（不含链路帧头，TCP 与 RTU 复用） ----------------
std::vector<uint8_t> buildReadHoldingRegisters(uint16_t startAddress, uint16_t count);
std::vector<uint8_t> buildReadInputRegisters(uint16_t startAddress, uint16_t count);
std::vector<uint8_t> buildWriteSingleRegister(uint16_t address, uint16_t value);
std::vector<uint8_t> buildWriteMultipleRegisters(uint16_t startAddress,
                                                 const std::vector<uint16_t>& values);

// ---------------- 组帧 ----------------
// TCP：MBAP 头 7 字节（事务号 2 + 协议标识 2 + 长度 2 + 单元号 1）+ PDU
std::vector<uint8_t> encodeTcpRequest(uint16_t transactionId, uint8_t unitId,
                                      const std::vector<uint8_t>& pdu);
// RTU：单元号 1 字节 + PDU + CRC16 2 字节（低字节在前）
std::vector<uint8_t> encodeRtuRequest(uint8_t unitId, const std::vector<uint8_t>& pdu);

// ---------------- 解帧 ----------------
// 返回已消费的字节数；返回 0 表示还需要更多数据，具体原因看 out.status。
// TCP：MBAP 的「长度」字段已经给出帧长，因此请求与响应用同一个解码函数。
std::size_t decodeTcp(const uint8_t* buffer, std::size_t len, DecodeResult& out) noexcept;
// RTU：帧里没有长度字段，必须先知道方向才能推断帧长，因此拆成两个函数。
std::size_t decodeRtuResponse(const uint8_t* buffer, std::size_t len, DecodeResult& out) noexcept;
std::size_t decodeRtuRequest(const uint8_t* buffer, std::size_t len, DecodeResult& out) noexcept;

// ---------------- 响应解析 ----------------
struct ReadRegistersResult {
    bool valid = false;
    uint8_t functionCode = 0;
    std::vector<uint16_t> values;
    ExceptionCode exception = ExceptionCode::None;
    std::string error;
};

ReadRegistersResult parseReadRegistersResponse(const Frame& frame, uint16_t expectedCount);

struct WriteSingleResult {
    bool valid = false;
    uint16_t address = 0;
    uint16_t value = 0;
    ExceptionCode exception = ExceptionCode::None;
    std::string error;
};

WriteSingleResult parseWriteSingleResponse(const Frame& frame);

struct WriteMultipleResult {
    bool valid = false;
    uint16_t address = 0;
    uint16_t count = 0;
    ExceptionCode exception = ExceptionCode::None;
    std::string error;
};

WriteMultipleResult parseWriteMultipleResponse(const Frame& frame);

// 一条响应是否属于某个已发出的请求。事务号只在 TCP 上存在，RTU 只用单元号。
bool matchesRequest(const Frame& response, Transport transport, uint16_t transactionId,
                    uint8_t unitId) noexcept;

// ---------------- 工具 ----------------
std::string toHex(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> fromHex(const std::string& text);

}  // namespace gateway::protocol
