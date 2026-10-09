#pragma once

#include <cstdint>
#include <string>

namespace gateway::protocol {

// Modbus 异常码（从站以「功能码 | 0x80 + 异常码」的 2 字节 PDU 返回）。
enum class ExceptionCode : uint8_t {
    None = 0x00,
    IllegalFunction = 0x01,
    IllegalDataAddress = 0x02,
    IllegalDataValue = 0x03,
    SlaveDeviceFailure = 0x04,
    Acknowledge = 0x05,
    SlaveDeviceBusy = 0x06,
    MemoryParityError = 0x08,
    GatewayPathUnavailable = 0x0A,
    GatewayTargetFailedToRespond = 0x0B,
    Unknown = 0xFF,
};

const char* toString(ExceptionCode code) noexcept;

// 给界面与日志用的中文说明：异常码本身不直观，界面上必须翻译成人话。
//
// [与项目一对应] 项目一把 HTTP 状态码映射成可读文案（404 页面、503 过载提示）；
// 这里是同一件事的协议版本：把 0x02 变成「地址越界或地址口径不一致」。
// 两个项目都不在界面里直接显示裸数字。
std::string toChineseHint(ExceptionCode code);

ExceptionCode toExceptionCode(uint8_t raw) noexcept;

// 异常响应的功能码最高位为 1。
bool isExceptionFunction(uint8_t functionCode) noexcept;

}  // namespace gateway::protocol
