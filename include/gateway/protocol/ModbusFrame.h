#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include "gateway/protocol/ExceptionCode.h"

namespace gateway::protocol {

enum class FunctionCode : uint8_t {
    READ_COILS = 0x01,
    READ_DISCRETE_INPUTS = 0x02,
    READ_HOLDING_REGISTERS = 0x03,
    READ_INPUT_REGISTERS = 0x04,
    WRITE_SINGLE_COIL = 0x05,
    WRITE_SINGLE_REGISTER = 0x06,
    WRITE_MULTIPLE_COILS = 0x0F,
    WRITE_MULTIPLE_REGISTERS = 0x10
};

inline std::string functionCodeToString(uint8_t func) {
    switch (static_cast<FunctionCode>(func)) {
        case FunctionCode::READ_COILS: return "01 Read Coils";
        case FunctionCode::READ_DISCRETE_INPUTS: return "02 Read Discrete Inputs";
        case FunctionCode::READ_HOLDING_REGISTERS: return "03 Read Holding Registers";
        case FunctionCode::READ_INPUT_REGISTERS: return "04 Read Input Registers";
        case FunctionCode::WRITE_SINGLE_COIL: return "05 Write Single Coil";
        case FunctionCode::WRITE_SINGLE_REGISTER: return "06 Write Single Register";
        case FunctionCode::WRITE_MULTIPLE_COILS: return "0F Write Multiple Coils";
        case FunctionCode::WRITE_MULTIPLE_REGISTERS: return "10 Write Multiple Registers";
        default: return "Unknown (" + std::to_string(func) + ")";
    }
}

/**
 * @brief Structured representation of a Modbus request.
 */
struct ModbusRequest {
    uint16_t transactionId = 0;   // Used in TCP MBAP header (echoed back in response)
    uint8_t unitId = 1;           // Slave / unit address
    uint8_t functionCode = 0x03;  // Modbus function code
    uint16_t address = 0;         // Starting address (0-based)
    uint16_t count = 1;           // Number of coils or registers
    std::vector<uint16_t> values; // Values for writing registers
    std::vector<bool> coilValues; // Values for writing coils

    std::string summary() const {
        std::ostringstream oss;
        oss << "Tx=" << transactionId << " Unit=" << static_cast<int>(unitId)
            << " Func=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(functionCode)
            << " Addr=" << std::dec << address << " Count=" << count;
        return oss.str();
    }
};

/**
 * @brief Structured representation of a Modbus response.
 */
struct ModbusResponse {
    uint16_t transactionId = 0;
    uint8_t unitId = 1;
    uint8_t functionCode = 0;
    bool isException = false;
    ModbusException exceptionCode = ModbusException::NONE;

    std::vector<uint16_t> registerValues;
    std::vector<bool> coilValues;
    std::vector<uint8_t> rawPdu;
    double latencyMs = 0.0;

    bool isSuccess() const {
        return !isException && exceptionCode == ModbusException::NONE;
    }

    std::string summary() const {
        std::ostringstream oss;
        oss << "Tx=" << transactionId << " Unit=" << static_cast<int>(unitId)
            << " Func=0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(functionCode)
            << std::dec;
        if (isException) {
            oss << " [EXCEPTION: " << exceptionToString(exceptionCode) << "]";
        } else {
            if (!registerValues.empty()) {
                oss << " Regs=[";
                for (size_t i = 0; i < registerValues.size(); ++i) {
                    if (i > 0) oss << ", ";
                    oss << registerValues[i];
                }
                oss << "]";
            }
            if (!coilValues.empty()) {
                oss << " Coils=[";
                for (size_t i = 0; i < coilValues.size(); ++i) {
                    if (i > 0) oss << ", ";
                    oss << (coilValues[i] ? "1" : "0");
                }
                oss << "]";
            }
        }
        oss << " (" << std::fixed << std::setprecision(1) << latencyMs << "ms)";
        return oss.str();
    }
};

} // namespace gateway::protocol
