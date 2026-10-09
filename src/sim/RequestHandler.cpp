#include "gateway/sim/RequestHandler.h"

#include <cstdio>

namespace gateway::sim {

using protocol::ExceptionCode;
using protocol::FunctionCode;

namespace {

void appendU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0x00FFu));  // 大端，与主站侧一致
    out.push_back(static_cast<uint8_t>(value & 0x00FFu));
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                 static_cast<uint16_t>(data[1]));
}

const char* functionName(uint8_t code) {
    switch (code) {
        case 0x01: return "读线圈";
        case 0x02: return "读离散输入";
        case 0x03: return "读保持寄存器";
        case 0x04: return "读输入寄存器";
        case 0x05: return "写单个线圈";
        case 0x06: return "写单个寄存器";
        case 0x0F: return "写多个线圈";
        case 0x10: return "写多个寄存器";
        default: return "未知功能码";
    }
}

std::string describeRange(uint8_t functionCode, uint16_t address, uint16_t count) {
    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "%s 地址 %u 起 %u 个", functionName(functionCode),
                  static_cast<unsigned>(address), static_cast<unsigned>(count));
    return buffer;
}

}  // namespace

// ------------------------------------------------------------------- 位打包

std::vector<uint8_t> packBits(const std::vector<bool>& bits) {
    // 位区按字节打包，第一个位是字节的最低位（LSB first），不足 8 位的高位补 0。
    // 这条规则与「大端/小端」无关，是规范单独规定的，写反了主站会读出一堆错位的位。
    std::vector<uint8_t> packed((bits.size() + 7) / 8, 0);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if (bits[i]) {
            packed[i / 8] = static_cast<uint8_t>(packed[i / 8] | (1u << (i % 8)));
        }
    }
    return packed;
}

bool unpackBits(const uint8_t* data, std::size_t byteCount, std::size_t bitCount,
                std::vector<bool>& out) {
    out.clear();
    if (data == nullptr) {
        return false;
    }
    // 字节数必须刚好够装下这些位：多一个少一个都说明主站算错了，不能将就。
    if (byteCount != (bitCount + 7) / 8) {
        return false;
    }
    out.reserve(bitCount);
    for (std::size_t i = 0; i < bitCount; ++i) {
        out.push_back((data[i / 8] & (1u << (i % 8))) != 0);
    }
    return true;
}

// ------------------------------------------------------------------- 响应构造

ResponsePdu RequestHandler::makeException(uint8_t functionCode, ExceptionCode code,
                                          const std::string& why) {
    ResponsePdu result;
    result.pdu.push_back(static_cast<uint8_t>(functionCode | 0x80u));
    result.pdu.push_back(static_cast<uint8_t>(code));
    result.exception = code;
    result.summary = why + " -> 异常响应 " + std::to_string(static_cast<unsigned>(code));
    ++exceptions_;
    return result;
}

// ------------------------------------------------------------------- 读取处理

ResponsePdu RequestHandler::handleReadBits(const protocol::Frame& request, uint8_t functionCode) {
    const std::vector<uint8_t>& pdu = request.pdu;
    // 请求 PDU：起始地址 2 + 数量 2
    if (pdu.size() != 4) {
        return makeException(functionCode, ExceptionCode::IllegalDataValue,
                             "请求数据域长度不是 4 字节");
    }
    const uint16_t address = readU16(&pdu[0]);
    const uint16_t count = readU16(&pdu[2]);
    if (count == 0) {
        return makeException(functionCode, ExceptionCode::IllegalDataValue, "请求数量为 0");
    }
    // 位区读取上限 2000：先于地址判定，理由同写多个寄存器。
    if (count > 2000) {
        return makeException(functionCode, ExceptionCode::IllegalDataValue,
                             "请求数量 " + std::to_string(count) + " 超过位区读取上限 2000");
    }

    std::vector<bool> bits;
    ExceptionCode exception = ExceptionCode::None;
    const bool ok = (functionCode == static_cast<uint8_t>(FunctionCode::ReadCoils))
                        ? map_->readCoils(address, count, bits, exception)
                        : map_->readDiscreteInputs(address, count, bits, exception);
    if (!ok) {
        return makeException(functionCode, exception, describeRange(functionCode, address, count));
    }

    const std::vector<uint8_t> packed = packBits(bits);
    ResponsePdu result;
    result.pdu.reserve(2 + packed.size());
    result.pdu.push_back(functionCode);
    result.pdu.push_back(static_cast<uint8_t>(packed.size()));
    result.pdu.insert(result.pdu.end(), packed.begin(), packed.end());
    result.summary = describeRange(functionCode, address, count);
    return result;
}

ResponsePdu RequestHandler::handleReadRegisters(const protocol::Frame& request,
                                                uint8_t functionCode) {
    const std::vector<uint8_t>& pdu = request.pdu;
    if (pdu.size() != 4) {
        return makeException(functionCode, ExceptionCode::IllegalDataValue,
                             "请求数据域长度不是 4 字节");
    }
    const uint16_t address = readU16(&pdu[0]);
    const uint16_t count = readU16(&pdu[2]);
    if (count == 0) {
        return makeException(functionCode, ExceptionCode::IllegalDataValue, "请求数量为 0");
    }

    std::vector<uint16_t> values;
    ExceptionCode exception = ExceptionCode::None;
    const bool ok = (functionCode == static_cast<uint8_t>(FunctionCode::ReadHoldingRegisters))
                        ? map_->readHoldingRegisters(address, count, values, exception)
                        : map_->readInputRegisters(address, count, values, exception);
    if (!ok) {
        return makeException(functionCode, exception, describeRange(functionCode, address, count));
    }

    ResponsePdu result;
    result.pdu.reserve(2 + values.size() * 2);
    result.pdu.push_back(functionCode);
    result.pdu.push_back(static_cast<uint8_t>(values.size() * 2));
    for (uint16_t value : values) {
        appendU16(result.pdu, value);
    }
    result.summary = describeRange(functionCode, address, count);
    return result;
}

// ------------------------------------------------------------------- 写入处理

ResponsePdu RequestHandler::handleWriteSingleRegister(const protocol::Frame& request) {
    constexpr uint8_t kFunction = static_cast<uint8_t>(FunctionCode::WriteSingleRegister);
    const std::vector<uint8_t>& pdu = request.pdu;
    if (pdu.size() != 4) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue,
                             "请求数据域长度不是 4 字节");
    }
    const uint16_t address = readU16(&pdu[0]);
    const uint16_t value = readU16(&pdu[2]);

    ExceptionCode exception = ExceptionCode::None;
    if (!map_->writeSingleRegister(address, value, exception)) {
        return makeException(kFunction, exception,
                             "写单个寄存器 地址 " + std::to_string(address));
    }
    // 写单个的响应就是回显地址与值（规范要求逐字节回送请求）。
    ResponsePdu result;
    result.pdu.push_back(kFunction);
    appendU16(result.pdu, address);
    appendU16(result.pdu, value);
    result.summary = "写单个寄存器 地址 " + std::to_string(address) + " = " +
                     std::to_string(value);
    return result;
}

ResponsePdu RequestHandler::handleWriteSingleCoil(const protocol::Frame& request) {
    constexpr uint8_t kFunction = static_cast<uint8_t>(FunctionCode::WriteSingleCoil);
    const std::vector<uint8_t>& pdu = request.pdu;
    if (pdu.size() != 4) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue,
                             "请求数据域长度不是 4 字节");
    }
    const uint16_t address = readU16(&pdu[0]);
    const uint16_t value = readU16(&pdu[2]);

    // 规范只允许两个取值：0xFF00 表示 ON，0x0000 表示 OFF。
    // 其余任何值都是非法的，必须回 0x03 而不是「按非零当 ON」猜过去 ——
    // 猜过去会让一个坏帧被当成正常请求执行，现场很难查。
    if (value != 0xFF00u && value != 0x0000u) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue,
                             "写单个线圈的值必须是 0xFF00 或 0x0000");
    }

    ExceptionCode exception = ExceptionCode::None;
    if (!map_->writeSingleCoil(address, value == 0xFF00u, exception)) {
        return makeException(kFunction, exception, "写单个线圈 地址 " + std::to_string(address));
    }
    ResponsePdu result;
    result.pdu.push_back(kFunction);
    appendU16(result.pdu, address);
    appendU16(result.pdu, value);
    result.summary = std::string("写单个线圈 地址 ") + std::to_string(address) + " = " +
                     (value == 0xFF00u ? "ON" : "OFF");
    return result;
}

ResponsePdu RequestHandler::handleWriteMultipleRegisters(const protocol::Frame& request) {
    constexpr uint8_t kFunction = static_cast<uint8_t>(FunctionCode::WriteMultipleRegisters);
    const std::vector<uint8_t>& pdu = request.pdu;
    // 请求 PDU：起始地址 2 + 数量 2 + 字节数 1 + 数据 N
    if (pdu.size() < 5) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue,
                             "请求数据域长度不足 5 字节");
    }
    const uint16_t address = readU16(&pdu[0]);
    const uint16_t count = readU16(&pdu[2]);
    const uint8_t byteCount = pdu[4];

    if (count == 0) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue, "请求数量为 0");
    }
    // 数量字段本身越规范（上限 123）：先报 0x03。
    // 这一条必须独立判断，不能依赖「解析出的值个数」——
    // 一个坏帧可以声称数量 124 却只带 2 个寄存器的数据，
    // 按数据解析出来的个数是 2，看起来合法，但请求声明的数量本身已经非法。
    if (count > 123) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue,
                             "请求数量 " + std::to_string(count) + " 超过规范上限 123");
    }
    // 字节数必须等于 数量 × 2，且要与 PDU 实际长度吻合。
    // 主站算错这两个字段时立即报 0x03，而不是按其中一个「凑合」解析。
    if (byteCount != count * 2 || pdu.size() != static_cast<std::size_t>(5) + byteCount) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue,
                             "字节数字段与寄存器数量不一致");
    }

    std::vector<uint16_t> values;
    values.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        values.push_back(readU16(&pdu[5 + static_cast<std::size_t>(i) * 2]));
    }

    ExceptionCode exception = ExceptionCode::None;
    if (!map_->writeMultipleRegisters(address, values, exception)) {
        return makeException(kFunction, exception,
                             describeRange(kFunction, address, count));
    }

    ResponsePdu result;
    result.pdu.push_back(kFunction);
    appendU16(result.pdu, address);
    appendU16(result.pdu, count);
    result.summary = describeRange(kFunction, address, count);
    return result;
}

ResponsePdu RequestHandler::handleWriteMultipleCoils(const protocol::Frame& request) {
    constexpr uint8_t kFunction = static_cast<uint8_t>(FunctionCode::WriteMultipleCoils);
    const std::vector<uint8_t>& pdu = request.pdu;
    if (pdu.size() < 5) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue,
                             "请求数据域长度不足 5 字节");
    }
    const uint16_t address = readU16(&pdu[0]);
    const uint16_t count = readU16(&pdu[2]);
    const uint8_t byteCount = pdu[4];

    if (count == 0) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue, "请求数量为 0");
    }
    if (count > 1968) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue,
                             "请求数量 " + std::to_string(count) + " 超过位区写入上限 1968");
    }
    if (byteCount != (count + 7) / 8 || pdu.size() != static_cast<std::size_t>(5) + byteCount) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue,
                             "字节数字段与线圈数量不一致");
    }

    std::vector<bool> bits;
    if (!unpackBits(&pdu[5], byteCount, count, bits)) {
        return makeException(kFunction, ExceptionCode::IllegalDataValue, "位数据域长度不匹配");
    }

    ExceptionCode exception = ExceptionCode::None;
    if (!map_->writeMultipleCoils(address, bits, exception)) {
        return makeException(kFunction, exception, describeRange(kFunction, address, count));
    }

    ResponsePdu result;
    result.pdu.push_back(kFunction);
    appendU16(result.pdu, address);
    appendU16(result.pdu, count);
    result.summary = describeRange(kFunction, address, count);
    return result;
}

// ------------------------------------------------------------------- 总入口

ResponsePdu RequestHandler::handleRequest(const protocol::Frame& request) {
    ResponsePdu result;
    const uint8_t functionCode = request.functionCode;

    // 未绑定数据区时直接报 0x04（从站设备故障）。
    // 这不该发生，但相比空指针崩溃，一个明确的异常码更容易定位问题。
    if (map_ == nullptr) {
        return makeException(functionCode, ExceptionCode::SlaveDeviceFailure,
                             "处理器未绑定寄存器数据区");
    }

    // 1) 地址过滤。串口总线上会有多个从站，单元号不匹配时必须保持沉默。
    //    广播地址（0）按规范不应答（本项目用于「只写不读」的场景）。
    if (request.unitId != unitId_) {
        result.summary = "单元号 " + std::to_string(request.unitId) + " 不是本从站（" +
                         std::to_string(unitId_) + "），不应答";
        return result;
    }
    if (request.unitId == 0 && !broadcast_) {
        result.summary = "广播请求，按配置不应答";
        return result;
    }

    // 2) 功能码分派。未实现的功能码按规范回 0x01（非法功能），
    //    而不是静默丢包 —— 主站需要知道「这个从站不支持」，而不是等到超时。
    switch (functionCode) {
        case 0x01:
        case 0x02:
            result = handleReadBits(request, functionCode);
            break;
        case 0x03:
        case 0x04:
            result = handleReadRegisters(request, functionCode);
            break;
        case 0x05:
            result = handleWriteSingleCoil(request);
            break;
        case 0x06:
            result = handleWriteSingleRegister(request);
            break;
        case 0x0F:
            result = handleWriteMultipleCoils(request);
            break;
        case 0x10:
            result = handleWriteMultipleRegisters(request);
            break;
        default:
            result = makeException(functionCode, ExceptionCode::IllegalFunction,
                                   std::string("未实现的功能码 ") +
                                       std::to_string(static_cast<unsigned>(functionCode)));
            break;
    }

    if (!result.pdu.empty()) {
        ++handled_;
    }
    return result;
}

}  // namespace gateway::sim
