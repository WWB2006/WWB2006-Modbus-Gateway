#include "gateway/protocol/ModbusCodec.h"
#include <cstring>

namespace gateway::protocol {

std::vector<uint8_t> ModbusCodec::buildPdu(const ModbusRequest& req) {
    std::vector<uint8_t> pdu;
    pdu.push_back(req.functionCode);

    switch (static_cast<FunctionCode>(req.functionCode)) {
        case FunctionCode::READ_COILS:
        case FunctionCode::READ_DISCRETE_INPUTS:
        case FunctionCode::READ_HOLDING_REGISTERS:
        case FunctionCode::READ_INPUT_REGISTERS: {
            pdu.push_back(static_cast<uint8_t>((req.address >> 8) & 0xFF));
            pdu.push_back(static_cast<uint8_t>(req.address & 0xFF));
            pdu.push_back(static_cast<uint8_t>((req.count >> 8) & 0xFF));
            pdu.push_back(static_cast<uint8_t>(req.count & 0xFF));
            break;
        }
        case FunctionCode::WRITE_SINGLE_COIL: {
            pdu.push_back(static_cast<uint8_t>((req.address >> 8) & 0xFF));
            pdu.push_back(static_cast<uint8_t>(req.address & 0xFF));
            bool val = !req.coilValues.empty() ? req.coilValues[0] : false;
            pdu.push_back(val ? 0xFF : 0x00);
            pdu.push_back(0x00);
            break;
        }
        case FunctionCode::WRITE_SINGLE_REGISTER: {
            pdu.push_back(static_cast<uint8_t>((req.address >> 8) & 0xFF));
            pdu.push_back(static_cast<uint8_t>(req.address & 0xFF));
            uint16_t val = !req.values.empty() ? req.values[0] : 0;
            pdu.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
            pdu.push_back(static_cast<uint8_t>(val & 0xFF));
            break;
        }
        case FunctionCode::WRITE_MULTIPLE_COILS: {
            pdu.push_back(static_cast<uint8_t>((req.address >> 8) & 0xFF));
            pdu.push_back(static_cast<uint8_t>(req.address & 0xFF));
            pdu.push_back(static_cast<uint8_t>((req.count >> 8) & 0xFF));
            pdu.push_back(static_cast<uint8_t>(req.count & 0xFF));
            uint8_t byteCount = static_cast<uint8_t>((req.count + 7) / 8);
            pdu.push_back(byteCount);
            pdu.resize(pdu.size() + byteCount, 0);
            for (size_t i = 0; i < req.coilValues.size() && i < req.count; ++i) {
                if (req.coilValues[i]) {
                    pdu[6 + (i / 8)] |= (1 << (i % 8));
                }
            }
            break;
        }
        case FunctionCode::WRITE_MULTIPLE_REGISTERS: {
            pdu.push_back(static_cast<uint8_t>((req.address >> 8) & 0xFF));
            pdu.push_back(static_cast<uint8_t>(req.address & 0xFF));
            pdu.push_back(static_cast<uint8_t>((req.count >> 8) & 0xFF));
            pdu.push_back(static_cast<uint8_t>(req.count & 0xFF));
            uint8_t byteCount = static_cast<uint8_t>(req.count * 2);
            pdu.push_back(byteCount);
            for (size_t i = 0; i < req.count; ++i) {
                uint16_t v = (i < req.values.size()) ? req.values[i] : 0;
                pdu.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                pdu.push_back(static_cast<uint8_t>(v & 0xFF));
            }
            break;
        }
        default:
            break;
    }
    return pdu;
}

std::vector<uint8_t> ModbusCodec::buildExceptionPdu(uint8_t functionCode, ModbusException ex) {
    std::vector<uint8_t> pdu;
    pdu.push_back(static_cast<uint8_t>(functionCode | 0x80));
    pdu.push_back(static_cast<uint8_t>(ex));
    return pdu;
}

std::vector<uint8_t> ModbusCodec::buildReadHoldingRegistersResponsePdu(const std::vector<uint16_t>& registers) {
    std::vector<uint8_t> pdu;
    pdu.push_back(static_cast<uint8_t>(FunctionCode::READ_HOLDING_REGISTERS));
    uint8_t byteCount = static_cast<uint8_t>(registers.size() * 2);
    pdu.push_back(byteCount);
    for (uint16_t val : registers) {
        pdu.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        pdu.push_back(static_cast<uint8_t>(val & 0xFF));
    }
    return pdu;
}

std::vector<uint8_t> ModbusCodec::buildReadInputRegistersResponsePdu(const std::vector<uint16_t>& registers) {
    std::vector<uint8_t> pdu;
    pdu.push_back(static_cast<uint8_t>(FunctionCode::READ_INPUT_REGISTERS));
    uint8_t byteCount = static_cast<uint8_t>(registers.size() * 2);
    pdu.push_back(byteCount);
    for (uint16_t val : registers) {
        pdu.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        pdu.push_back(static_cast<uint8_t>(val & 0xFF));
    }
    return pdu;
}

std::vector<uint8_t> ModbusCodec::buildReadCoilsResponsePdu(const std::vector<bool>& coils) {
    std::vector<uint8_t> pdu;
    pdu.push_back(static_cast<uint8_t>(FunctionCode::READ_COILS));
    uint8_t byteCount = static_cast<uint8_t>((coils.size() + 7) / 8);
    pdu.push_back(byteCount);
    pdu.resize(pdu.size() + byteCount, 0);
    for (size_t i = 0; i < coils.size(); ++i) {
        if (coils[i]) {
            pdu[2 + (i / 8)] |= (1 << (i % 8));
        }
    }
    return pdu;
}

std::vector<uint8_t> ModbusCodec::buildReadDiscreteInputsResponsePdu(const std::vector<bool>& inputs) {
    std::vector<uint8_t> pdu;
    pdu.push_back(static_cast<uint8_t>(FunctionCode::READ_DISCRETE_INPUTS));
    uint8_t byteCount = static_cast<uint8_t>((inputs.size() + 7) / 8);
    pdu.push_back(byteCount);
    pdu.resize(pdu.size() + byteCount, 0);
    for (size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i]) {
            pdu[2 + (i / 8)] |= (1 << (i % 8));
        }
    }
    return pdu;
}

std::vector<uint8_t> ModbusCodec::buildWriteSingleCoilResponsePdu(uint16_t address, bool value) {
    std::vector<uint8_t> pdu;
    pdu.push_back(static_cast<uint8_t>(FunctionCode::WRITE_SINGLE_COIL));
    pdu.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
    pdu.push_back(static_cast<uint8_t>(address & 0xFF));
    pdu.push_back(value ? 0xFF : 0x00);
    pdu.push_back(0x00);
    return pdu;
}

std::vector<uint8_t> ModbusCodec::buildWriteSingleRegisterResponsePdu(uint16_t address, uint16_t value) {
    std::vector<uint8_t> pdu;
    pdu.push_back(static_cast<uint8_t>(FunctionCode::WRITE_SINGLE_REGISTER));
    pdu.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
    pdu.push_back(static_cast<uint8_t>(address & 0xFF));
    pdu.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    pdu.push_back(static_cast<uint8_t>(value & 0xFF));
    return pdu;
}

std::vector<uint8_t> ModbusCodec::buildWriteMultipleCoilsResponsePdu(uint16_t address, uint16_t count) {
    std::vector<uint8_t> pdu;
    pdu.push_back(static_cast<uint8_t>(FunctionCode::WRITE_MULTIPLE_COILS));
    pdu.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
    pdu.push_back(static_cast<uint8_t>(address & 0xFF));
    pdu.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
    pdu.push_back(static_cast<uint8_t>(count & 0xFF));
    return pdu;
}

std::vector<uint8_t> ModbusCodec::buildWriteMultipleRegistersResponsePdu(uint16_t address, uint16_t count) {
    std::vector<uint8_t> pdu;
    pdu.push_back(static_cast<uint8_t>(FunctionCode::WRITE_MULTIPLE_REGISTERS));
    pdu.push_back(static_cast<uint8_t>((address >> 8) & 0xFF));
    pdu.push_back(static_cast<uint8_t>(address & 0xFF));
    pdu.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
    pdu.push_back(static_cast<uint8_t>(count & 0xFF));
    return pdu;
}

std::vector<uint8_t> ModbusCodec::buildTcpFrame(const ModbusRequest& req) {
    std::vector<uint8_t> pdu = buildPdu(req);
    return buildTcpResponseFrame(req.transactionId, req.unitId, pdu);
}

std::vector<uint8_t> ModbusCodec::buildTcpResponseFrame(uint16_t txId, uint8_t unitId, const std::vector<uint8_t>& pdu) {
    std::vector<uint8_t> frame;
    frame.reserve(7 + pdu.size());
    // Transaction ID (2 bytes)
    frame.push_back(static_cast<uint8_t>((txId >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(txId & 0xFF));
    // Protocol ID = 0 (2 bytes)
    frame.push_back(0x00);
    frame.push_back(0x00);
    // Length = 1 (unitId) + PDU size (2 bytes)
    uint16_t len = static_cast<uint16_t>(1 + pdu.size());
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
    // Unit ID (1 byte)
    frame.push_back(unitId);
    // PDU
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    return frame;
}

std::vector<uint8_t> ModbusCodec::buildRtuFrame(const ModbusRequest& req) {
    std::vector<uint8_t> pdu = buildPdu(req);
    return buildRtuResponseFrame(req.unitId, pdu);
}

std::vector<uint8_t> ModbusCodec::buildRtuResponseFrame(uint8_t unitId, const std::vector<uint8_t>& pdu) {
    std::vector<uint8_t> frame;
    frame.reserve(1 + pdu.size() + 2);
    frame.push_back(unitId);
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    Crc16::append(frame);
    return frame;
}

CodecStatus ModbusCodec::parseTcpResponse(const uint8_t* data, size_t len, ModbusResponse& outResponse, size_t& bytesConsumed) {
    bytesConsumed = 0;
    if (len < 7) {
        return CodecStatus::NEED_MORE_DATA;
    }

    uint16_t txId = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    uint16_t protocolId = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    uint16_t mbapLen = (static_cast<uint16_t>(data[4]) << 8) | data[5];

    if (protocolId != 0) {
        return CodecStatus::INVALID_HEADER;
    }
    if (mbapLen < 2) {
        return CodecStatus::INVALID_LENGTH;
    }

    size_t fullFrameLen = 6 + mbapLen;
    if (len < fullFrameLen) {
        return CodecStatus::NEED_MORE_DATA;
    }

    uint8_t unitId = data[6];
    outResponse.transactionId = txId;
    outResponse.unitId = unitId;

    const uint8_t* pdu = data + 7;
    size_t pduLen = mbapLen - 1;

    CodecStatus status = parseResponsePdu(pdu, pduLen, outResponse);
    if (status == CodecStatus::OK) {
        bytesConsumed = fullFrameLen;
    }
    return status;
}

CodecStatus ModbusCodec::parseRtuResponse(const uint8_t* data, size_t len, uint8_t expectedUnitId, uint8_t expectedFunc, ModbusResponse& outResponse, size_t& bytesConsumed) {
    bytesConsumed = 0;
    if (len < 5) { // Minimum RTU exception response: Unit(1) + Func|0x80(1) + ExCode(1) + CRC(2) = 5 bytes
        return CodecStatus::NEED_MORE_DATA;
    }

    uint8_t unitId = data[0];
    if (expectedUnitId != 0 && unitId != expectedUnitId) {
        // Mismatched unit ID
        return CodecStatus::INVALID_HEADER;
    }

    uint8_t func = data[1];
    bool isEx = (func & 0x80) != 0;

    size_t expectedLen = 0;
    if (isEx) {
        expectedLen = 5;
    } else {
        switch (static_cast<FunctionCode>(func)) {
            case FunctionCode::READ_COILS:
            case FunctionCode::READ_DISCRETE_INPUTS:
            case FunctionCode::READ_HOLDING_REGISTERS:
            case FunctionCode::READ_INPUT_REGISTERS: {
                if (len < 3) return CodecStatus::NEED_MORE_DATA;
                uint8_t byteCount = data[2];
                expectedLen = 1 + 1 + 1 + byteCount + 2; // Unit + Func + ByteCount + Data + CRC
                break;
            }
            case FunctionCode::WRITE_SINGLE_COIL:
            case FunctionCode::WRITE_SINGLE_REGISTER:
            case FunctionCode::WRITE_MULTIPLE_COILS:
            case FunctionCode::WRITE_MULTIPLE_REGISTERS:
                expectedLen = 1 + 1 + 2 + 2 + 2; // Unit + Func + Addr(2) + Val/Count(2) + CRC(2) = 8 bytes
                break;
            default:
                return CodecStatus::UNSUPPORTED_FUNCTION;
        }
    }

    if (len < expectedLen) {
        return CodecStatus::NEED_MORE_DATA;
    }

    if (!Crc16::verify(data, expectedLen)) {
        return CodecStatus::CRC_ERROR;
    }

    outResponse.transactionId = 0;
    outResponse.unitId = unitId;

    const uint8_t* pdu = data + 1;
    size_t pduLen = expectedLen - 3; // Subtract UnitId and 2-byte CRC

    CodecStatus status = parseResponsePdu(pdu, pduLen, outResponse);
    if (status == CodecStatus::OK) {
        bytesConsumed = expectedLen;
    }
    return status;
}

CodecStatus ModbusCodec::parseResponsePdu(const uint8_t* pdu, size_t len, ModbusResponse& outResponse) {
    if (len < 1) {
        return CodecStatus::MALFORMED_PDU;
    }

    outResponse.rawPdu.assign(pdu, pdu + len);
    uint8_t func = pdu[0];
    outResponse.functionCode = func;

    // Check for exception response (bit 7 set)
    if (func & 0x80) {
        outResponse.isException = true;
        if (len < 2) {
            return CodecStatus::MALFORMED_PDU;
        }
        outResponse.exceptionCode = static_cast<ModbusException>(pdu[1]);
        return CodecStatus::OK;
    }

    outResponse.isException = false;
    outResponse.exceptionCode = ModbusException::NONE;

    switch (static_cast<FunctionCode>(func)) {
        case FunctionCode::READ_COILS:
        case FunctionCode::READ_DISCRETE_INPUTS: {
            if (len < 2) return CodecStatus::MALFORMED_PDU;
            uint8_t byteCount = pdu[1];
            if (len < static_cast<size_t>(2 + byteCount)) return CodecStatus::MALFORMED_PDU;
            outResponse.coilValues.clear();
            for (size_t i = 0; i < byteCount; ++i) {
                uint8_t b = pdu[2 + i];
                for (int bit = 0; bit < 8; ++bit) {
                    outResponse.coilValues.push_back((b & (1 << bit)) != 0);
                }
            }
            break;
        }
        case FunctionCode::READ_HOLDING_REGISTERS:
        case FunctionCode::READ_INPUT_REGISTERS: {
            if (len < 2) return CodecStatus::MALFORMED_PDU;
            uint8_t byteCount = pdu[1];
            if (byteCount % 2 != 0 || len < static_cast<size_t>(2 + byteCount)) {
                return CodecStatus::MALFORMED_PDU;
            }
            outResponse.registerValues.clear();
            size_t regCount = byteCount / 2;
            outResponse.registerValues.reserve(regCount);
            for (size_t i = 0; i < regCount; ++i) {
                uint16_t regVal = (static_cast<uint16_t>(pdu[2 + i * 2]) << 8) |
                                   static_cast<uint16_t>(pdu[2 + i * 2 + 1]);
                outResponse.registerValues.push_back(regVal);
            }
            break;
        }
        case FunctionCode::WRITE_SINGLE_COIL: {
            if (len < 5) return CodecStatus::MALFORMED_PDU;
            uint16_t val = (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4];
            outResponse.coilValues.push_back(val == 0xFF00);
            break;
        }
        case FunctionCode::WRITE_SINGLE_REGISTER: {
            if (len < 5) return CodecStatus::MALFORMED_PDU;
            uint16_t val = (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4];
            outResponse.registerValues.push_back(val);
            break;
        }
        case FunctionCode::WRITE_MULTIPLE_COILS:
        case FunctionCode::WRITE_MULTIPLE_REGISTERS: {
            if (len < 5) return CodecStatus::MALFORMED_PDU;
            // Response echoes starting address and quantity
            break;
        }
        default:
            return CodecStatus::UNSUPPORTED_FUNCTION;
    }

    return CodecStatus::OK;
}

CodecStatus ModbusCodec::parseTcpRequest(const uint8_t* data, size_t len, ModbusRequest& outRequest, size_t& bytesConsumed) {
    bytesConsumed = 0;
    if (len < 7) {
        return CodecStatus::NEED_MORE_DATA;
    }

    uint16_t txId = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    uint16_t protocolId = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    uint16_t mbapLen = (static_cast<uint16_t>(data[4]) << 8) | data[5];

    if (protocolId != 0) return CodecStatus::INVALID_HEADER;
    if (mbapLen < 2) return CodecStatus::INVALID_LENGTH;

    size_t fullFrameLen = 6 + mbapLen;
    if (len < fullFrameLen) return CodecStatus::NEED_MORE_DATA;

    outRequest.transactionId = txId;
    outRequest.unitId = data[6];

    const uint8_t* pdu = data + 7;
    size_t pduLen = mbapLen - 1;

    CodecStatus status = parseRequestPdu(pdu, pduLen, outRequest);
    if (status == CodecStatus::OK) {
        bytesConsumed = fullFrameLen;
    }
    return status;
}

CodecStatus ModbusCodec::parseRtuRequest(const uint8_t* data, size_t len, ModbusRequest& outRequest, size_t& bytesConsumed) {
    bytesConsumed = 0;
    if (len < 6) {
        return CodecStatus::NEED_MORE_DATA;
    }

    uint8_t func = data[1];
    size_t expectedLen = 0;
    switch (static_cast<FunctionCode>(func)) {
        case FunctionCode::READ_COILS:
        case FunctionCode::READ_DISCRETE_INPUTS:
        case FunctionCode::READ_HOLDING_REGISTERS:
        case FunctionCode::READ_INPUT_REGISTERS:
        case FunctionCode::WRITE_SINGLE_COIL:
        case FunctionCode::WRITE_SINGLE_REGISTER:
            expectedLen = 8; // Unit(1) + Func(1) + Addr(2) + Count/Val(2) + CRC(2)
            break;
        case FunctionCode::WRITE_MULTIPLE_COILS:
        case FunctionCode::WRITE_MULTIPLE_REGISTERS: {
            if (len < 7) return CodecStatus::NEED_MORE_DATA;
            uint8_t byteCount = data[6];
            expectedLen = 7 + byteCount + 2; // Unit(1)+Func(1)+Addr(2)+Count(2)+ByteCount(1)+Data(N)+CRC(2)
            break;
        }
        default:
            return CodecStatus::UNSUPPORTED_FUNCTION;
    }

    if (len < expectedLen) {
        return CodecStatus::NEED_MORE_DATA;
    }

    if (!Crc16::verify(data, expectedLen)) {
        return CodecStatus::CRC_ERROR;
    }

    outRequest.transactionId = 0;
    outRequest.unitId = data[0];

    const uint8_t* pdu = data + 1;
    size_t pduLen = expectedLen - 3;

    CodecStatus status = parseRequestPdu(pdu, pduLen, outRequest);
    if (status == CodecStatus::OK) {
        bytesConsumed = expectedLen;
    }
    return status;
}

CodecStatus ModbusCodec::parseRequestPdu(const uint8_t* pdu, size_t len, ModbusRequest& outRequest) {
    if (len < 1) return CodecStatus::MALFORMED_PDU;

    outRequest.functionCode = pdu[0];
    switch (static_cast<FunctionCode>(outRequest.functionCode)) {
        case FunctionCode::READ_COILS:
        case FunctionCode::READ_DISCRETE_INPUTS:
        case FunctionCode::READ_HOLDING_REGISTERS:
        case FunctionCode::READ_INPUT_REGISTERS: {
            if (len < 5) return CodecStatus::MALFORMED_PDU;
            outRequest.address = (static_cast<uint16_t>(pdu[1]) << 8) | pdu[2];
            outRequest.count = (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4];
            break;
        }
        case FunctionCode::WRITE_SINGLE_COIL: {
            if (len < 5) return CodecStatus::MALFORMED_PDU;
            outRequest.address = (static_cast<uint16_t>(pdu[1]) << 8) | pdu[2];
            outRequest.count = 1;
            uint16_t val = (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4];
            outRequest.coilValues = { val == 0xFF00 };
            break;
        }
        case FunctionCode::WRITE_SINGLE_REGISTER: {
            if (len < 5) return CodecStatus::MALFORMED_PDU;
            outRequest.address = (static_cast<uint16_t>(pdu[1]) << 8) | pdu[2];
            outRequest.count = 1;
            uint16_t val = (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4];
            outRequest.values = { val };
            break;
        }
        case FunctionCode::WRITE_MULTIPLE_COILS: {
            if (len < 6) return CodecStatus::MALFORMED_PDU;
            outRequest.address = (static_cast<uint16_t>(pdu[1]) << 8) | pdu[2];
            outRequest.count = (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4];
            uint8_t byteCount = pdu[5];
            if (len < static_cast<size_t>(6 + byteCount)) return CodecStatus::MALFORMED_PDU;
            outRequest.coilValues.clear();
            for (size_t i = 0; i < outRequest.count; ++i) {
                uint8_t b = pdu[6 + (i / 8)];
                outRequest.coilValues.push_back((b & (1 << (i % 8))) != 0);
            }
            break;
        }
        case FunctionCode::WRITE_MULTIPLE_REGISTERS: {
            if (len < 6) return CodecStatus::MALFORMED_PDU;
            outRequest.address = (static_cast<uint16_t>(pdu[1]) << 8) | pdu[2];
            outRequest.count = (static_cast<uint16_t>(pdu[3]) << 8) | pdu[4];
            uint8_t byteCount = pdu[5];
            if (byteCount != outRequest.count * 2 || len < static_cast<size_t>(6 + byteCount)) {
                return CodecStatus::MALFORMED_PDU;
            }
            outRequest.values.clear();
            outRequest.values.reserve(outRequest.count);
            for (size_t i = 0; i < outRequest.count; ++i) {
                uint16_t v = (static_cast<uint16_t>(pdu[6 + i * 2]) << 8) |
                              static_cast<uint16_t>(pdu[6 + i * 2 + 1]);
                outRequest.values.push_back(v);
            }
            break;
        }
        default:
            return CodecStatus::UNSUPPORTED_FUNCTION;
    }
    return CodecStatus::OK;
}

} // namespace gateway::protocol
