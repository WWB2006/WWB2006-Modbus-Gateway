#include "gateway/protocol/ModbusCodec.h"

#include <algorithm>

#include "gateway/protocol/Crc16.h"

namespace gateway::protocol {
namespace {

constexpr std::size_t kNeedMore = static_cast<std::size_t>(-1);

enum class Role {
    Request,
    Response,
};

void appendU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0x00FFu));  // 大端
    out.push_back(static_cast<uint8_t>(value & 0x00FFu));
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                 static_cast<uint16_t>(data[1]));
}

// RTU 帧没有长度字段，只能用「方向 + 功能码」推断整帧字节数。
// 返回 kNeedMore 表示缓冲区还不足以判断，返回 0 表示功能码未知。
std::size_t rtuFrameLength(const uint8_t* buffer, std::size_t len, Role role) {
    const uint8_t functionCode = buffer[1];

    // 异常响应固定为：单元号 1 + 功能码 1 + 异常码 1 + CRC 2
    if (isExceptionFunction(functionCode)) {
        return 5;
    }

    if (role == Role::Response) {
        switch (functionCode) {
            case 0x01:
            case 0x02:
            case 0x03:
            case 0x04:
                // 单元号 1 + 功能码 1 + 字节数 1 + 数据 N + CRC 2
                if (len < 3) {
                    return kNeedMore;
                }
                return 1 + 2 + static_cast<std::size_t>(buffer[2]) + 2;
            case 0x05:
            case 0x06:
            case 0x0F:
            case 0x10:
                // 单元号 1 + 功能码 1 + 地址 2 + 值/数量 2 + CRC 2
                return 8;
            default:
                return 0;
        }
    }

    switch (functionCode) {
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
            // 单元号 1 + 功能码 1 + 起始地址 2 + 数量/值 2 + CRC 2
            return 8;
        case 0x0F:
        case 0x10:
            // 单元号 1 + 功能码 1 + 地址 2 + 数量 2 + 字节数 1 + 数据 N + CRC 2
            if (len < 7) {
                return kNeedMore;
            }
            return 1 + 6 + static_cast<std::size_t>(buffer[6]) + 2;
        default:
            return 0;
    }
}

std::size_t decodeRtuImpl(const uint8_t* buffer, std::size_t len, DecodeResult& out,
                          Role role) noexcept {
    out = DecodeResult{};
    if (buffer == nullptr) {
        out.status = DecodeStatus::Incomplete;
        return 0;
    }
    if (len < 2) {
        out.status = DecodeStatus::Incomplete;
        return 0;
    }

    const std::size_t frameLength = rtuFrameLength(buffer, len, role);
    if (frameLength == kNeedMore) {
        out.status = DecodeStatus::Incomplete;
        return 0;
    }
    if (frameLength == 0) {
        out.status = DecodeStatus::BadFunction;
        out.message = "未知功能码，无法推断 RTU 帧长";
        return 0;
    }
    if (len < frameLength) {
        out.status = DecodeStatus::Incomplete;
        return 0;
    }
    if (!verifyCrc16(buffer, frameLength)) {
        out.status = DecodeStatus::BadCrc;
        out.message = "CRC 校验失败，本帧已丢弃并重新同步";
        return 0;
    }

    out.frame.transport = Transport::Rtu;
    out.frame.unitId = buffer[0];
    out.frame.functionCode = buffer[1];
    // PDU = 功能码之后的数据域
    out.frame.pdu.assign(buffer + 2, buffer + frameLength - 2);
    out.status = DecodeStatus::Ok;
    out.consumed = frameLength;
    return frameLength;
}

bool isRegisterFunction(uint8_t functionCode) {
    return functionCode == static_cast<uint8_t>(FunctionCode::ReadHoldingRegisters) ||
           functionCode == static_cast<uint8_t>(FunctionCode::ReadInputRegisters);
}

}  // namespace

// ---------------------------------------------------------------- PDU 构造

std::vector<uint8_t> buildReadHoldingRegisters(uint16_t startAddress, uint16_t count) {
    if (count == 0 || count > kMaxReadRegisters) {
        return {};  // 参数非法，交由调用方报错，不产生一条注定被拒绝的报文
    }
    std::vector<uint8_t> pdu;
    pdu.reserve(5);
    pdu.push_back(static_cast<uint8_t>(FunctionCode::ReadHoldingRegisters));
    appendU16(pdu, startAddress);
    appendU16(pdu, count);
    return pdu;
}

std::vector<uint8_t> buildReadInputRegisters(uint16_t startAddress, uint16_t count) {
    if (count == 0 || count > kMaxReadRegisters) {
        return {};
    }
    std::vector<uint8_t> pdu;
    pdu.reserve(5);
    pdu.push_back(static_cast<uint8_t>(FunctionCode::ReadInputRegisters));
    appendU16(pdu, startAddress);
    appendU16(pdu, count);
    return pdu;
}

std::vector<uint8_t> buildWriteSingleRegister(uint16_t address, uint16_t value) {
    std::vector<uint8_t> pdu;
    pdu.reserve(5);
    pdu.push_back(static_cast<uint8_t>(FunctionCode::WriteSingleRegister));
    appendU16(pdu, address);
    appendU16(pdu, value);
    return pdu;
}

std::vector<uint8_t> buildWriteMultipleRegisters(uint16_t startAddress,
                                                 const std::vector<uint16_t>& values) {
    if (values.empty() || values.size() > kMaxWriteRegisters) {
        return {};
    }
    std::vector<uint8_t> pdu;
    pdu.reserve(6 + values.size() * 2);
    pdu.push_back(static_cast<uint8_t>(FunctionCode::WriteMultipleRegisters));
    appendU16(pdu, startAddress);
    appendU16(pdu, static_cast<uint16_t>(values.size()));
    pdu.push_back(static_cast<uint8_t>(values.size() * 2));
    for (uint16_t value : values) {
        appendU16(pdu, value);
    }
    return pdu;
}

// ------------------------------------------------------------------ 组帧

std::vector<uint8_t> encodeTcpRequest(uint16_t transactionId, uint8_t unitId,
                                      const std::vector<uint8_t>& pdu) {
    if (pdu.empty() || pdu.size() > kMaxPduSize) {
        return {};
    }
    std::vector<uint8_t> frame;
    frame.reserve(kMbapHeaderSize + pdu.size());
    appendU16(frame, transactionId);
    appendU16(frame, 0);  // 协议标识固定为 0
    // 长度字段 = 单元号 1 字节 + PDU 长度，不是整帧长度
    appendU16(frame, static_cast<uint16_t>(pdu.size() + 1));
    frame.push_back(unitId);
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    return frame;
}

std::vector<uint8_t> encodeRtuRequest(uint8_t unitId, const std::vector<uint8_t>& pdu) {
    if (pdu.empty() || pdu.size() > kMaxPduSize) {
        return {};
    }
    std::vector<uint8_t> frame;
    frame.reserve(1 + pdu.size() + 2);
    frame.push_back(unitId);
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    appendCrc16(frame);
    return frame;
}

// ------------------------------------------------------------------ 解帧

std::size_t decodeTcp(const uint8_t* buffer, std::size_t len, DecodeResult& out) noexcept {
    out = DecodeResult{};
    if (buffer == nullptr) {
        out.status = DecodeStatus::Incomplete;
        return 0;
    }
    if (len < kMbapHeaderSize) {
        out.status = DecodeStatus::Incomplete;
        return 0;
    }

    const uint16_t transactionId = readU16(buffer);
    const uint16_t protocolId = readU16(buffer + 2);
    const uint16_t lengthField = readU16(buffer + 4);

    if (protocolId != 0) {
        out.status = DecodeStatus::BadProtocolId;
        out.message = "MBAP 协议标识必须为 0，本帧按非法报文丢弃";
        return 0;
    }
    if (lengthField < 2 || lengthField > static_cast<uint16_t>(kMaxPduSize + 1)) {
        out.status = DecodeStatus::BadLength;
        out.message = "MBAP 长度字段超出合法范围";
        return 0;
    }

    const std::size_t total = 6 + static_cast<std::size_t>(lengthField);
    if (len < total) {
        out.status = DecodeStatus::Incomplete;  // TCP 靠长度字段切包：攒够再解
        return 0;
    }

    out.frame.transport = Transport::Tcp;
    out.frame.transactionId = transactionId;
    out.frame.protocolId = protocolId;
    out.frame.unitId = buffer[6];
    out.frame.functionCode = buffer[7];
    out.frame.pdu.assign(buffer + 8, buffer + total);
    out.status = DecodeStatus::Ok;
    out.consumed = total;
    return total;
}

std::size_t decodeRtuResponse(const uint8_t* buffer, std::size_t len,
                              DecodeResult& out) noexcept {
    return decodeRtuImpl(buffer, len, out, Role::Response);
}

std::size_t decodeRtuRequest(const uint8_t* buffer, std::size_t len, DecodeResult& out) noexcept {
    return decodeRtuImpl(buffer, len, out, Role::Request);
}

// -------------------------------------------------------------- 响应解析

ReadRegistersResult parseReadRegistersResponse(const Frame& frame, uint16_t expectedCount) {
    ReadRegistersResult result;
    result.functionCode = frame.functionCode;

    if (frame.isException()) {
        result.exception = frame.exceptionCode();
        result.error = toChineseHint(result.exception);
        return result;
    }
    if (!isRegisterFunction(frame.functionCode)) {
        result.error = "功能码不是读保持寄存器/输入寄存器（0x01、0x02 的位读取在阶段 3 实现）";
        return result;
    }
    if (frame.pdu.empty()) {
        result.error = "PDU 缺少字节数字段";
        return result;
    }

    const std::size_t byteCount = static_cast<std::size_t>(frame.pdu[0]);
    if (byteCount != static_cast<std::size_t>(expectedCount) * 2) {
        result.error = "响应字节数与请求的寄存器数量不一致";
        return result;
    }
    if (frame.pdu.size() != byteCount + 1) {
        result.error = "PDU 实际长度与字节数字段不一致";
        return result;
    }

    result.values.reserve(expectedCount);
    for (uint16_t i = 0; i < expectedCount; ++i) {
        result.values.push_back(readU16(&frame.pdu[1 + static_cast<std::size_t>(i) * 2]));
    }
    result.valid = true;
    return result;
}

WriteSingleResult parseWriteSingleResponse(const Frame& frame) {
    WriteSingleResult result;
    if (frame.isException()) {
        result.exception = frame.exceptionCode();
        result.error = toChineseHint(result.exception);
        return result;
    }
    if (frame.functionCode != static_cast<uint8_t>(FunctionCode::WriteSingleRegister)) {
        result.error = "功能码不是写单个寄存器";
        return result;
    }
    if (frame.pdu.size() != 4) {
        result.error = "写单个寄存器的响应应为 4 字节";
        return result;
    }
    result.address = readU16(&frame.pdu[0]);
    result.value = readU16(&frame.pdu[2]);
    result.valid = true;
    return result;
}

WriteMultipleResult parseWriteMultipleResponse(const Frame& frame) {
    WriteMultipleResult result;
    if (frame.isException()) {
        result.exception = frame.exceptionCode();
        result.error = toChineseHint(result.exception);
        return result;
    }
    if (frame.functionCode != static_cast<uint8_t>(FunctionCode::WriteMultipleRegisters)) {
        result.error = "功能码不是写多个寄存器";
        return result;
    }
    if (frame.pdu.size() != 4) {
        result.error = "写多个寄存器的响应应为 4 字节";
        return result;
    }
    result.address = readU16(&frame.pdu[0]);
    result.count = readU16(&frame.pdu[2]);
    result.valid = true;
    return result;
}

bool matchesRequest(const Frame& response, Transport transport, uint16_t transactionId,
                    uint8_t unitId) noexcept {
    if (response.unitId != unitId) {
        return false;
    }
    if (transport == Transport::Tcp) {
        return response.transactionId == transactionId;
    }
    return true;  // RTU 没有事务号，只能靠单元号 + 顺序匹配
}

// ------------------------------------------------------------------ 工具

std::string toHex(const std::vector<uint8_t>& bytes) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        out.push_back(digits[(bytes[i] >> 4) & 0x0F]);
        out.push_back(digits[bytes[i] & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> fromHex(const std::string& text) {
    std::vector<uint8_t> out;
    int high = -1;
    for (char ch : text) {
        int value = -1;
        if (ch >= '0' && ch <= '9') {
            value = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            value = ch - 'a' + 10;
        } else if (ch >= 'A' && ch <= 'F') {
            value = ch - 'A' + 10;
        } else {
            continue;  // 跳过空格、逗号等分隔符
        }
        if (high < 0) {
            high = value;
        } else {
            out.push_back(static_cast<uint8_t>((high << 4) | value));
            high = -1;
        }
    }
    return out;
}

}  // namespace gateway::protocol
