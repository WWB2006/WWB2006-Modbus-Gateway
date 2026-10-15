#include "gateway/protocol/ModbusCodec.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace gateway::protocol;

int main() {
    std::cout << "[TEST] Running tst_codec..." << std::endl;

    // Test 1: Build & Parse TCP Read Holding Registers Request
    {
        ModbusRequest req;
        req.transactionId = 42;
        req.unitId = 1;
        req.functionCode = static_cast<uint8_t>(FunctionCode::READ_HOLDING_REGISTERS);
        req.address = 100;
        req.count = 5;

        std::vector<uint8_t> frame = ModbusCodec::buildTcpFrame(req);
        // Header: 00 2A 00 00 00 06 01 03 00 64 00 05 (12 bytes)
        assert(frame.size() == 12);
        assert(frame[0] == 0x00 && frame[1] == 0x2A); // Tx 42
        assert(frame[2] == 0x00 && frame[3] == 0x00); // Protocol 0
        assert(frame[4] == 0x00 && frame[5] == 0x06); // Length 6
        assert(frame[6] == 0x01);                     // Unit ID 1
        assert(frame[7] == 0x03);                     // Func 3
        assert(frame[8] == 0x00 && frame[9] == 0x64); // Addr 100
        assert(frame[10] == 0x00 && frame[11] == 0x05); // Count 5

        // Parse TCP Request
        ModbusRequest parsedReq;
        size_t consumed = 0;
        CodecStatus status = ModbusCodec::parseTcpRequest(frame.data(), frame.size(), parsedReq, consumed);
        assert(status == CodecStatus::OK);
        assert(consumed == 12);
        assert(parsedReq.transactionId == 42);
        assert(parsedReq.unitId == 1);
        assert(parsedReq.functionCode == 3);
        assert(parsedReq.address == 100);
        assert(parsedReq.count == 5);
    }

    // Test 2: Parse TCP Read Holding Registers Response
    {
        // 5 registers: 10, 20, 30, 40, 50
        // PDU: 03 (func) 0A (byte count = 10) 00 0A  00 14  00 1E  00 28  00 32
        std::vector<uint16_t> regs = {10, 20, 30, 40, 50};
        std::vector<uint8_t> pdu = ModbusCodec::buildReadHoldingRegistersResponsePdu(regs);
        std::vector<uint8_t> tcpResp = ModbusCodec::buildTcpResponseFrame(42, 1, pdu);

        ModbusResponse resp;
        size_t consumed = 0;
        CodecStatus status = ModbusCodec::parseTcpResponse(tcpResp.data(), tcpResp.size(), resp, consumed);
        assert(status == CodecStatus::OK);
        assert(consumed == tcpResp.size());
        assert(resp.transactionId == 42);
        assert(resp.unitId == 1);
        assert(resp.functionCode == 3);
        assert(!resp.isException);
        assert(resp.registerValues.size() == 5);
        assert(resp.registerValues[0] == 10);
        assert(resp.registerValues[1] == 20);
        assert(resp.registerValues[2] == 30);
        assert(resp.registerValues[3] == 40);
        assert(resp.registerValues[4] == 50);
    }

    // Test 3: Parse Exception Response (e.g. 0x83 0x02 Illegal Address)
    {
        std::vector<uint8_t> exPdu = ModbusCodec::buildExceptionPdu(0x03, ModbusException::ILLEGAL_DATA_ADDRESS);
        std::vector<uint8_t> tcpExResp = ModbusCodec::buildTcpResponseFrame(100, 2, exPdu);

        ModbusResponse resp;
        size_t consumed = 0;
        CodecStatus status = ModbusCodec::parseTcpResponse(tcpExResp.data(), tcpExResp.size(), resp, consumed);
        assert(status == CodecStatus::OK);
        assert(resp.transactionId == 100);
        assert(resp.unitId == 2);
        assert(resp.isException == true);
        assert(resp.exceptionCode == ModbusException::ILLEGAL_DATA_ADDRESS);
    }

    // Test 4: RTU Frame Build & Parse
    {
        ModbusRequest req;
        req.unitId = 3;
        req.functionCode = 0x03;
        req.address = 0;
        req.count = 2;

        std::vector<uint8_t> rtuFrame = ModbusCodec::buildRtuFrame(req);
        // Unit(1) + Func(1) + Addr(2) + Count(2) + CRC(2) = 8 bytes
        assert(rtuFrame.size() == 8);
        assert(Crc16::verify(rtuFrame));

        ModbusRequest parsedReq;
        size_t consumed = 0;
        CodecStatus status = ModbusCodec::parseRtuRequest(rtuFrame.data(), rtuFrame.size(), parsedReq, consumed);
        assert(status == CodecStatus::OK);
        assert(consumed == 8);
        assert(parsedReq.unitId == 3);
        assert(parsedReq.functionCode == 3);
        assert(parsedReq.address == 0);
        assert(parsedReq.count == 2);
    }

    // Test 5: RTU Response Parse and CRC Error handling
    {
        std::vector<uint16_t> vals = {1234, 5678};
        std::vector<uint8_t> pdu = ModbusCodec::buildReadHoldingRegistersResponsePdu(vals);
        std::vector<uint8_t> rtuResp = ModbusCodec::buildRtuResponseFrame(3, pdu);

        ModbusResponse resp;
        size_t consumed = 0;
        CodecStatus status = ModbusCodec::parseRtuResponse(rtuResp.data(), rtuResp.size(), 3, 3, resp, consumed);
        assert(status == CodecStatus::OK);
        assert(resp.registerValues.size() == 2);
        assert(resp.registerValues[0] == 1234);
        assert(resp.registerValues[1] == 5678);

        // Corrupt CRC
        rtuResp[rtuResp.size() - 1] ^= 0xFF;
        status = ModbusCodec::parseRtuResponse(rtuResp.data(), rtuResp.size(), 3, 3, resp, consumed);
        assert(status == CodecStatus::CRC_ERROR);
    }

    // Test 6: Incomplete / fragmented data handling
    {
        std::vector<uint8_t> incomplete = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06}; // 6 bytes, need >= 7
        ModbusResponse resp;
        size_t consumed = 0;
        CodecStatus status = ModbusCodec::parseTcpResponse(incomplete.data(), incomplete.size(), resp, consumed);
        assert(status == CodecStatus::NEED_MORE_DATA);
    }

    // Test 7: Write Multiple Registers PDU
    {
        ModbusRequest writeReq;
        writeReq.unitId = 1;
        writeReq.functionCode = static_cast<uint8_t>(FunctionCode::WRITE_MULTIPLE_REGISTERS);
        writeReq.address = 10;
        writeReq.count = 3;
        writeReq.values = {100, 200, 300};

        std::vector<uint8_t> pdu = ModbusCodec::buildPdu(writeReq);
        // Func(1) + Addr(2) + Count(2) + ByteCount(1) + Values(6) = 12 bytes
        assert(pdu.size() == 12);
        assert(pdu[0] == 0x10);
        assert(pdu[5] == 6); // 3 * 2

        ModbusRequest parsedWriteReq;
        CodecStatus st = ModbusCodec::parseRequestPdu(pdu.data(), pdu.size(), parsedWriteReq);
        assert(st == CodecStatus::OK);
        assert(parsedWriteReq.functionCode == 0x10);
        assert(parsedWriteReq.values.size() == 3);
        assert(parsedWriteReq.values[0] == 100);
        assert(parsedWriteReq.values[1] == 200);
        assert(parsedWriteReq.values[2] == 300);
    }

    std::cout << "[PASS] tst_codec passed all 7 test scenarios!" << std::endl;
    return 0;
}
