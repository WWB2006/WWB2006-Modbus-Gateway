#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gateway/protocol/ExceptionCode.h"

namespace gateway::protocol {

enum class Transport {
    Tcp,
    Rtu,
};

enum class FunctionCode : uint8_t {
    ReadCoils = 0x01,
    ReadDiscreteInputs = 0x02,
    ReadHoldingRegisters = 0x03,
    ReadInputRegisters = 0x04,
    WriteSingleCoil = 0x05,
    WriteSingleRegister = 0x06,
    WriteMultipleCoils = 0x0F,
    WriteMultipleRegisters = 0x10,
};

const char* toString(FunctionCode code) noexcept;

// 已解出的一条报文。pdu 是功能码之后的数据域，不含功能码本身。
struct Frame {
    Transport transport = Transport::Tcp;
    uint16_t transactionId = 0;  // 仅 TCP 有效，用于匹配请求与响应
    uint16_t protocolId = 0;     // TCP MBAP 保留字段，必须为 0
    uint8_t unitId = 0;
    uint8_t functionCode = 0;
    std::vector<uint8_t> pdu;

    bool isException() const noexcept { return isExceptionFunction(functionCode); }
    ExceptionCode exceptionCode() const noexcept;
    std::string describe() const;
};

enum class DecodeStatus {
    Ok,
    Incomplete,     // 数据不足，继续等待
    BadCrc,         // RTU 帧 CRC 校验失败，帧同步已丢失
    BadProtocolId,  // MBAP 协议标识不为 0
    BadLength,      // MBAP 长度字段非法或与实际不符
    BadFunction,    // 功能码未知或 PDU 长度与功能码不符
    TooShort,
};

const char* toString(DecodeStatus status) noexcept;

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Incomplete;
    Frame frame;
    std::size_t consumed = 0;  // 仅在 status == Ok 时有效
    std::string message;
};

}  // namespace gateway::protocol
