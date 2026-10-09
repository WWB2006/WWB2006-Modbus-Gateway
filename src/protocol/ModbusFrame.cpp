#include "gateway/protocol/ModbusFrame.h"

#include <sstream>

namespace gateway::protocol {

const char* toString(FunctionCode code) noexcept {
    switch (code) {
        case FunctionCode::ReadCoils:
            return "ReadCoils";
        case FunctionCode::ReadDiscreteInputs:
            return "ReadDiscreteInputs";
        case FunctionCode::ReadHoldingRegisters:
            return "ReadHoldingRegisters";
        case FunctionCode::ReadInputRegisters:
            return "ReadInputRegisters";
        case FunctionCode::WriteSingleCoil:
            return "WriteSingleCoil";
        case FunctionCode::WriteSingleRegister:
            return "WriteSingleRegister";
        case FunctionCode::WriteMultipleCoils:
            return "WriteMultipleCoils";
        case FunctionCode::WriteMultipleRegisters:
            return "WriteMultipleRegisters";
    }
    return "Unknown";
}

const char* toString(DecodeStatus status) noexcept {
    switch (status) {
        case DecodeStatus::Ok:
            return "Ok";
        case DecodeStatus::Incomplete:
            return "Incomplete";
        case DecodeStatus::BadCrc:
            return "BadCrc";
        case DecodeStatus::BadProtocolId:
            return "BadProtocolId";
        case DecodeStatus::BadLength:
            return "BadLength";
        case DecodeStatus::BadFunction:
            return "BadFunction";
        case DecodeStatus::TooShort:
            return "TooShort";
    }
    return "Unknown";
}

ExceptionCode Frame::exceptionCode() const noexcept {
    if (!isException() || pdu.empty()) {
        return ExceptionCode::None;
    }
    return toExceptionCode(pdu[0]);
}

std::string Frame::describe() const {
    std::ostringstream oss;
    oss << (transport == Transport::Tcp ? "TCP" : "RTU") << " unit=" << static_cast<int>(unitId)
        << " func=0x";
    const char* hex = "0123456789ABCDEF";
    oss << hex[(functionCode >> 4) & 0x0F] << hex[functionCode & 0x0F];
    if (transport == Transport::Tcp) {
        oss << " tx=" << transactionId;
    }
    if (isException()) {
        oss << " exception=" << toString(exceptionCode());
    }
    oss << " pdu=" << pdu.size() << "B";
    return oss.str();
}

}  // namespace gateway::protocol
