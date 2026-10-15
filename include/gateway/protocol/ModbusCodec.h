#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include "gateway/protocol/ModbusFrame.h"
#include "gateway/protocol/ExceptionCode.h"
#include "gateway/protocol/Crc16.h"

namespace gateway::protocol {

enum class CodecStatus {
    OK,
    NEED_MORE_DATA,
    CRC_ERROR,
    INVALID_HEADER,
    INVALID_LENGTH,
    UNSUPPORTED_FUNCTION,
    MALFORMED_PDU
};

inline std::string codecStatusToString(CodecStatus st) {
    switch (st) {
        case CodecStatus::OK: return "OK";
        case CodecStatus::NEED_MORE_DATA: return "Need More Data";
        case CodecStatus::CRC_ERROR: return "CRC Error";
        case CodecStatus::INVALID_HEADER: return "Invalid Header";
        case CodecStatus::INVALID_LENGTH: return "Invalid Length";
        case CodecStatus::UNSUPPORTED_FUNCTION: return "Unsupported Function";
        case CodecStatus::MALFORMED_PDU: return "Malformed PDU";
        default: return "Unknown";
    }
}

class ModbusCodec {
public:
    // PDU Construction
    static std::vector<uint8_t> buildPdu(const ModbusRequest& req);
    static std::vector<uint8_t> buildExceptionPdu(uint8_t functionCode, ModbusException ex);
    static std::vector<uint8_t> buildReadHoldingRegistersResponsePdu(const std::vector<uint16_t>& registers);
    static std::vector<uint8_t> buildReadInputRegistersResponsePdu(const std::vector<uint16_t>& registers);
    static std::vector<uint8_t> buildReadCoilsResponsePdu(const std::vector<bool>& coils);
    static std::vector<uint8_t> buildReadDiscreteInputsResponsePdu(const std::vector<bool>& inputs);
    static std::vector<uint8_t> buildWriteSingleCoilResponsePdu(uint16_t address, bool value);
    static std::vector<uint8_t> buildWriteSingleRegisterResponsePdu(uint16_t address, uint16_t value);
    static std::vector<uint8_t> buildWriteMultipleCoilsResponsePdu(uint16_t address, uint16_t count);
    static std::vector<uint8_t> buildWriteMultipleRegistersResponsePdu(uint16_t address, uint16_t count);

    // Frame Building
    static std::vector<uint8_t> buildTcpFrame(const ModbusRequest& req);
    static std::vector<uint8_t> buildTcpResponseFrame(uint16_t txId, uint8_t unitId, const std::vector<uint8_t>& pdu);
    static std::vector<uint8_t> buildRtuFrame(const ModbusRequest& req);
    static std::vector<uint8_t> buildRtuResponseFrame(uint8_t unitId, const std::vector<uint8_t>& pdu);

    // Frame Parsing
    static CodecStatus parseTcpResponse(const uint8_t* data, size_t len, ModbusResponse& outResponse, size_t& bytesConsumed);
    static CodecStatus parseRtuResponse(const uint8_t* data, size_t len, uint8_t expectedUnitId, uint8_t expectedFunc, ModbusResponse& outResponse, size_t& bytesConsumed);

    // Slave Side Request Parsing
    static CodecStatus parseTcpRequest(const uint8_t* data, size_t len, ModbusRequest& outRequest, size_t& bytesConsumed);
    static CodecStatus parseRtuRequest(const uint8_t* data, size_t len, ModbusRequest& outRequest, size_t& bytesConsumed);

    // PDU Parser
    static CodecStatus parseResponsePdu(const uint8_t* pdu, size_t len, ModbusResponse& outResponse);
    static CodecStatus parseRequestPdu(const uint8_t* pdu, size_t len, ModbusRequest& outRequest);
};

} // namespace gateway::protocol
