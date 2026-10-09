#include "gateway/protocol/ExceptionCode.h"

namespace gateway::protocol {

const char* toString(ExceptionCode code) noexcept {
    switch (code) {
        case ExceptionCode::None:
            return "None";
        case ExceptionCode::IllegalFunction:
            return "IllegalFunction";
        case ExceptionCode::IllegalDataAddress:
            return "IllegalDataAddress";
        case ExceptionCode::IllegalDataValue:
            return "IllegalDataValue";
        case ExceptionCode::SlaveDeviceFailure:
            return "SlaveDeviceFailure";
        case ExceptionCode::Acknowledge:
            return "Acknowledge";
        case ExceptionCode::SlaveDeviceBusy:
            return "SlaveDeviceBusy";
        case ExceptionCode::MemoryParityError:
            return "MemoryParityError";
        case ExceptionCode::GatewayPathUnavailable:
            return "GatewayPathUnavailable";
        case ExceptionCode::GatewayTargetFailedToRespond:
            return "GatewayTargetFailedToRespond";
        default:
            return "Unknown";
    }
}

std::string toChineseHint(ExceptionCode code) {
    switch (code) {
        case ExceptionCode::None:
            return "正常";
        case ExceptionCode::IllegalFunction:
            return "从站不支持该功能码，检查主站是否用了设备未实现的功能";
        case ExceptionCode::IllegalDataAddress:
            return "寄存器地址越界或地址口径不一致（40001 与 0-based 差一位）";
        case ExceptionCode::IllegalDataValue:
            return "数据值非法：请求数量或写入值超出从站允许范围";
        case ExceptionCode::SlaveDeviceFailure:
            return "从站内部故障，通常是设备自身异常而非通信问题";
        case ExceptionCode::Acknowledge:
            return "从站已受理但需要较长时间，稍后重新轮询";
        case ExceptionCode::SlaveDeviceBusy:
            return "从站忙，降低轮询频率或错开请求";
        case ExceptionCode::MemoryParityError:
            return "从站存储校验错误";
        case ExceptionCode::GatewayPathUnavailable:
            return "网关路径不可用：网关配置错误或后端设备未上线";
        case ExceptionCode::GatewayTargetFailedToRespond:
            return "网关后的目标设备无响应：设备离线或链路断开";
        default:
            return "未知异常码，需要抓包核对报文";
    }
}

ExceptionCode toExceptionCode(uint8_t raw) noexcept {
    switch (raw) {
        case 0x00:
            return ExceptionCode::None;
        case 0x01:
            return ExceptionCode::IllegalFunction;
        case 0x02:
            return ExceptionCode::IllegalDataAddress;
        case 0x03:
            return ExceptionCode::IllegalDataValue;
        case 0x04:
            return ExceptionCode::SlaveDeviceFailure;
        case 0x05:
            return ExceptionCode::Acknowledge;
        case 0x06:
            return ExceptionCode::SlaveDeviceBusy;
        case 0x08:
            return ExceptionCode::MemoryParityError;
        case 0x0A:
            return ExceptionCode::GatewayPathUnavailable;
        case 0x0B:
            return ExceptionCode::GatewayTargetFailedToRespond;
        default:
            return ExceptionCode::Unknown;
    }
}

bool isExceptionFunction(uint8_t functionCode) noexcept {
    return (functionCode & 0x80u) != 0;
}

}  // namespace gateway::protocol
