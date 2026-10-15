#pragma once

#include <cstdint>
#include <string>

namespace gateway::protocol {

/**
 * @brief Standard Modbus Exception Codes as defined in Modbus Application Protocol V1.1b3.
 */
enum class ModbusException : uint8_t {
    NONE = 0x00,
    ILLEGAL_FUNCTION = 0x01,
    ILLEGAL_DATA_ADDRESS = 0x02,
    ILLEGAL_DATA_VALUE = 0x03,
    SLAVE_DEVICE_FAILURE = 0x04,
    ACKNOWLEDGE = 0x05,
    SLAVE_DEVICE_BUSY = 0x06,
    NEGATIVE_ACKNOWLEDGE = 0x07,
    MEMORY_PARITY_ERROR = 0x08,
    GATEWAY_PATH_UNAVAILABLE = 0x0A,
    GATEWAY_TARGET_DEVICE_FAILED_TO_RESPOND = 0x0B
};

/**
 * @brief Convert Modbus exception code to human-readable string.
 */
inline std::string exceptionToString(ModbusException ex) {
    switch (ex) {
        case ModbusException::NONE:
            return "No Exception";
        case ModbusException::ILLEGAL_FUNCTION:
            return "Illegal Function (0x01)";
        case ModbusException::ILLEGAL_DATA_ADDRESS:
            return "Illegal Data Address (0x02)";
        case ModbusException::ILLEGAL_DATA_VALUE:
            return "Illegal Data Value (0x03)";
        case ModbusException::SLAVE_DEVICE_FAILURE:
            return "Slave Device Failure (0x04)";
        case ModbusException::ACKNOWLEDGE:
            return "Acknowledge (0x05)";
        case ModbusException::SLAVE_DEVICE_BUSY:
            return "Slave Device Busy (0x06)";
        case ModbusException::NEGATIVE_ACKNOWLEDGE:
            return "Negative Acknowledge (0x07)";
        case ModbusException::MEMORY_PARITY_ERROR:
            return "Memory Parity Error (0x08)";
        case ModbusException::GATEWAY_PATH_UNAVAILABLE:
            return "Gateway Path Unavailable (0x0A)";
        case ModbusException::GATEWAY_TARGET_DEVICE_FAILED_TO_RESPOND:
            return "Gateway Target Device Failed to Respond (0x0B)";
        default:
            return "Unknown Exception (" + std::to_string(static_cast<uint8_t>(ex)) + ")";
    }
}

} // namespace gateway::protocol
