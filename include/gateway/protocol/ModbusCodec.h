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
// 位读取（0x01/0x02）在阶段 3 补齐：从站模拟器需要它们来验证位区的打包顺序。
std::vector<uint8_t> buildReadCoils(uint16_t startAddress, uint16_t count);
std::vector<uint8_t> buildReadDiscreteInputs(uint16_t startAddress, uint16_t count);
std::vector<uint8_t> buildWriteSingleCoil(uint16_t address, bool value);
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
//
// [与项目一对应] 项目一的 HttpParser 也依赖报文自带的长度信息切分消息体
// （有 Content-Length 就按它读，没有就按 chunked 或读到连接关闭）；
// 这里换成 MBAP 的 2 字节长度字段。差别在于 Modbus TCP 的长度字段是强制的，
// 所以不存在「读到连接关闭」这种兜底，非法长度必须直接判错。
// 注意：解帧结果里含 std::vector / std::string（PDU 与错误文案），
// 因此这三个函数并非不抛异常 —— 早期版本给它们加了 noexcept，
// 等于把「内存不足」从可处理的异常变成 std::terminate，这里去掉该误标。
std::size_t decodeTcp(const uint8_t* buffer, std::size_t len, DecodeResult& out);
// RTU：帧里没有长度字段，必须先知道方向才能推断帧长，因此拆成两个函数。
std::size_t decodeRtuResponse(const uint8_t* buffer, std::size_t len, DecodeResult& out);
std::size_t decodeRtuRequest(const uint8_t* buffer, std::size_t len, DecodeResult& out);

// ---------------- 响应解析 ----------------
struct ReadRegistersResult {
    bool valid = false;
    uint8_t functionCode = 0;
    std::vector<uint16_t> values;
    ExceptionCode exception = ExceptionCode::None;
    std::string error;
};

ReadRegistersResult parseReadRegistersResponse(const Frame& frame, uint16_t expectedCount);

// 位读取（0x01/0x02）响应的解析结果。位在字节里低位在前，与从站的打包方式一致。
struct ReadBitsResult {
    bool valid = false;
    uint8_t functionCode = 0;
    std::vector<bool> bits;
    ExceptionCode exception = ExceptionCode::None;
    std::string error;
};

ReadBitsResult parseReadBitsResponse(const Frame& frame, uint16_t expectedCount);

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

// 一条响应是否属于某个已发出的请求。
// - 单元号必须一致（两种链路都比）；
// - TCP 还要比事务号；
// - 功能码必须与请求一致，异常响应（功能码 | 0x80）同样放行。
//   早期版本不比功能码，RTU 上一条「别人的帧」会被当成自己的响应消费掉，
//   导致本该重传的事务被提前判失败。
bool matchesRequest(const Frame& response, Transport transport, uint16_t transactionId,
                    uint8_t unitId, uint8_t functionCode) noexcept;

// ---------------- 工具 ----------------
std::string toHex(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> fromHex(const std::string& text);

}  // namespace gateway::protocol
