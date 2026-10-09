// 不依赖 Qt 的协议层测试：任何有 C++17 编译器的地方都能跑。
// 用途有二：1) 装上 Qt 之前先验证协议层；2) 装到没有 Qt 的机器或 CI 上做快速回归。
// Qt Test 版本（tests/unit/tst_crc16.cpp、tst_codec.cpp）覆盖同一批用例。

#include <cstdio>
#include <string>
#include <vector>

#include "gateway/protocol/Crc16.h"
#include "gateway/protocol/ExceptionCode.h"
#include "gateway/protocol/ModbusCodec.h"
#include "test_support.h"

using namespace gateway::protocol;
using namespace test_support;

namespace {

// ------------------------------------------------------------------ CRC16

void testCrc16() {
    section("CRC16 固定测试向量");

    checkU16(crc16(nullptr, 0), 0xFFFF, "空数据的 CRC 为初值");
    const uint8_t zero = 0x00;
    checkU16(crc16(&zero, 1), 0x40BF, "单字节 0x00");

    const auto readReq = fromHex("01 03 00 00 00 0A");
    checkU16(crc16(readReq.data(), readReq.size()), 0xCDC5, "读保持寄存器请求");

    const auto readResp = fromHex("01 03 04 00 0A 00 14");
    checkU16(crc16(readResp.data(), readResp.size()), 0x3EDA, "读保持寄存器响应");

    const auto writeReq = fromHex("01 06 00 01 00 03");
    checkU16(crc16(writeReq.data(), writeReq.size()), 0x0B98, "写单个寄存器请求");

    const auto writeMulti = fromHex("01 10 00 00 00 02 04 00 0A 00 14");
    checkU16(crc16(writeMulti.data(), writeMulti.size()), 0xA2D3, "写多个寄存器请求");

    section("查表实现与逐位实现必须一致");
    std::vector<uint8_t> sample;
    for (int i = 0; i < 512; ++i) {
        sample.push_back(static_cast<uint8_t>((i * 37 + 11) & 0xFF));
    }
    checkU16(crc16(sample.data(), sample.size()),
             crc16Bitwise(sample.data(), sample.size()), "512 字节随机序列");

    section("CRC 追加顺序与校验");
    std::vector<uint8_t> frame = readReq;
    appendCrc16(frame);
    checkBytes(frame, "01 03 00 00 00 0A C5 CD", "RTU 帧尾 CRC 低字节在前");
    check(verifyCrc16(frame.data(), frame.size()), "完整帧校验通过");

    std::vector<uint8_t> broken = frame;
    broken[3] ^= 0x01;
    check(!verifyCrc16(broken.data(), broken.size()), "数据被篡改后校验失败");
    check(!verifyCrc16(frame.data(), 2), "长度过短时直接判失败");
}

// -------------------------------------------------------------- PDU 构造

void testPduBuilders() {
    section("PDU 构造");

    checkBytes(buildReadHoldingRegisters(0, 10), "03 00 00 00 0A", "读保持寄存器 0 起 10 个");
    checkBytes(buildReadInputRegisters(0x000A, 4), "04 00 0A 00 04", "读输入寄存器");
    checkBytes(buildWriteSingleRegister(0x0001, 0x0003), "06 00 01 00 03", "写单个寄存器");
    checkBytes(buildWriteMultipleRegisters(0, {10, 20}), "10 00 00 00 02 04 00 0A 00 14",
               "写多个寄存器（含字节数字段）");

    check(buildReadHoldingRegisters(0, 0).empty(), "数量为 0 时返回空 PDU");
    check(buildReadHoldingRegisters(0, 126).empty(), "超过 125 个寄存器时返回空 PDU");
    check(buildWriteMultipleRegisters(0, {}).empty(), "写入空数组时返回空 PDU");

    std::vector<uint16_t> tooMany(124, 1);
    check(buildWriteMultipleRegisters(0, tooMany).empty(), "一次写 124 个寄存器超出 123 上限");
}

// ------------------------------------------------------------------ 组帧

void testEncoding() {
    section("TCP 组帧");
    const auto pdu = buildReadHoldingRegisters(0, 10);
    const auto tcp = encodeTcpRequest(1, 1, pdu);
    checkBytes(tcp, "00 01 00 00 00 06 01 03 00 00 00 0A", "MBAP 头 + PDU");
    check(tcp.size() == 12, "整帧 12 字节");
    checkU16(static_cast<uint16_t>(tcp[4] << 8 | tcp[5]), 6, "长度字段是 6（单元号+PDU），不是整帧长度");

    check(encodeTcpRequest(1, 1, {}).empty(), "空 PDU 返回空帧");

    section("RTU 组帧");
    const auto rtu = encodeRtuRequest(1, pdu);
    checkBytes(rtu, "01 03 00 00 00 0A C5 CD", "单元号 + PDU + CRC");
    check(rtu.size() == 8, "整帧 8 字节");
}

// ---------------------------------------------------------------- TCP 解帧

void testDecodeTcp() {
    section("TCP 解帧");
    const auto frame = encodeTcpRequest(1, 1, buildReadHoldingRegisters(0, 10));

    DecodeResult result;
    const std::size_t consumed = decodeTcp(frame.data(), frame.size(), result);
    check(consumed == 12, "消费 12 字节");
    check(result.status == DecodeStatus::Ok, "状态为 Ok");
    check(result.frame.transactionId == 1, "事务号解析正确");
    check(result.frame.unitId == 1, "单元号解析正确");
    check(result.frame.functionCode == 0x03, "功能码解析正确");
    checkBytes(result.frame.pdu, "00 00 00 0A", "PDU 不含功能码本身");

    section("TCP 半包");
    const std::size_t partial = decodeTcp(frame.data(), frame.size() - 1, result);
    check(partial == 0, "未返回长度");
    check(result.status == DecodeStatus::Incomplete, "状态为 Incomplete，等待剩余字节");

    section("TCP 粘包");
    std::vector<uint8_t> two = frame;
    const auto second = encodeTcpRequest(2, 1, buildWriteSingleRegister(1, 3));
    two.insert(two.end(), second.begin(), second.end());
    const std::size_t firstLen = decodeTcp(two.data(), two.size(), result);
    check(firstLen == 12, "第一帧消费 12 字节");
    const std::size_t secondLen = decodeTcp(two.data() + firstLen, two.size() - firstLen, result);
    check(secondLen == 12, "第二帧消费 12 字节");
    check(result.frame.transactionId == 2, "第二帧事务号为 2，可与请求匹配");

    section("TCP 非法报文");
    std::vector<uint8_t> badProto = frame;
    badProto[3] = 0x01;
    check(decodeTcp(badProto.data(), badProto.size(), result) == 0, "协议标识非 0 时不解帧");
    check(result.status == DecodeStatus::BadProtocolId, "状态为 BadProtocolId");

    std::vector<uint8_t> badLength = frame;
    badLength[4] = 0x00;
    badLength[5] = 0x00;
    check(decodeTcp(badLength.data(), badLength.size(), result) == 0, "长度字段为 0 时不解帧");
    check(result.status == DecodeStatus::BadLength, "状态为 BadLength");

    section("TCP 异常响应");
    const auto exceptionFrame = fromHex("00 01 00 00 00 03 01 83 02");
    const std::size_t exceptionLen =
        decodeTcp(exceptionFrame.data(), exceptionFrame.size(), result);
    check(exceptionLen == 9, "异常响应消费 9 字节");
    check(result.frame.isException(), "识别为异常响应");
    check(result.frame.exceptionCode() == ExceptionCode::IllegalDataAddress, "异常码为 0x02");
}

// ---------------------------------------------------------------- RTU 解帧

void testDecodeRtu() {
    section("RTU 响应解帧");
    const auto response = fromHex("01 03 04 00 0A 00 14 DA 3E");
    DecodeResult result;
    const std::size_t consumed = decodeRtuResponse(response.data(), response.size(), result);
    check(consumed == 9, "消费 9 字节（单元号 1 + 功能码 1 + 字节数 1 + 数据 4 + CRC 2）");
    check(result.status == DecodeStatus::Ok, "状态为 Ok");
    check(result.frame.unitId == 1, "单元号解析正确");
    check(result.frame.functionCode == 0x03, "功能码解析正确");
    checkBytes(result.frame.pdu, "04 00 0A 00 14", "PDU 含字节数域");

    section("RTU 半包与坏 CRC");
    DecodeResult partial;
    check(decodeRtuResponse(response.data(), 7, partial) == 0, "7 字节不足以确定完整帧");
    check(partial.status == DecodeStatus::Incomplete, "状态为 Incomplete");

    std::vector<uint8_t> corrupted = response;
    corrupted[corrupted.size() - 1] ^= 0xFF;
    DecodeResult bad;
    check(decodeRtuResponse(corrupted.data(), corrupted.size(), bad) == 0, "坏 CRC 不解出帧");
    check(bad.status == DecodeStatus::BadCrc, "状态为 BadCrc");

    section("RTU 请求解帧（阶段 3 从站模拟器会用）");
    const auto request = encodeRtuRequest(1, buildReadHoldingRegisters(0, 10));
    DecodeResult requestResult;
    check(decodeRtuRequest(request.data(), request.size(), requestResult) == 8, "读请求 8 字节");
    check(requestResult.frame.functionCode == 0x03, "请求功能码正确");

    const auto writeRequest = encodeRtuRequest(1, buildWriteMultipleRegisters(0, {10, 20}));
    DecodeResult writeResult;
    const std::size_t writeLen =
        decodeRtuRequest(writeRequest.data(), writeRequest.size(), writeResult);
    check(writeLen == 13, "写多个寄存器请求 13 字节");
    checkBytes(writeResult.frame.pdu, "00 00 00 02 04 00 0A 00 14",
               "请求 PDU 能完整取到地址、数量、字节数与数据");

    section("RTU 异常响应");
    const auto exceptionResponse = fromHex("01 83 02 C0 F1");
    DecodeResult exceptionResult;
    check(decodeRtuResponse(exceptionResponse.data(), exceptionResponse.size(),
                            exceptionResult) == 5,
          "异常响应 5 字节");
    check(exceptionResult.frame.isException(), "识别为异常帧");
    check(exceptionResult.frame.exceptionCode() == ExceptionCode::IllegalDataAddress,
          "异常码为 0x02");

    section("RTU 未知功能码");
    const auto unknown = fromHex("01 45 00 00");
    DecodeResult unknownResult;
    check(decodeRtuResponse(unknown.data(), unknown.size(), unknownResult) == 0,
          "未知功能码不解帧");
    check(unknownResult.status == DecodeStatus::BadFunction, "状态为 BadFunction");
}

// -------------------------------------------------------------- 响应解析

void testResponseParsing() {
    section("读寄存器响应解析");
    const auto response = fromHex("01 03 04 00 0A 00 14 DA 3E");
    DecodeResult decoded;
    decodeRtuResponse(response.data(), response.size(), decoded);

    const auto parsed = parseReadRegistersResponse(decoded.frame, 2);
    check(parsed.valid, "解析成功");
    check(parsed.values.size() == 2, "取到 2 个寄存器");
    checkU16(parsed.values[0], 10, "第一个寄存器");
    checkU16(parsed.values[1], 20, "第二个寄存器（大端解析）");

    const auto mismatch = parseReadRegistersResponse(decoded.frame, 3);
    check(!mismatch.valid, "期望数量不匹配时判为非法");
    check(mismatch.error.find("字节数") != std::string::npos, "错误信息指出字节数不一致");

    section("写寄存器响应解析");
    const auto singleResponse = fromHex("01 06 00 01 00 03 98 0B");
    DecodeResult singleDecoded;
    decodeRtuResponse(singleResponse.data(), singleResponse.size(), singleDecoded);
    const auto single = parseWriteSingleResponse(singleDecoded.frame);
    check(single.valid, "写单个寄存器响应解析成功");
    checkU16(single.address, 1, "回显地址");
    checkU16(single.value, 3, "回显数值");

    const auto multiResponse = fromHex("01 10 00 00 00 02 41 C8");
    DecodeResult multiDecoded;
    decodeRtuResponse(multiResponse.data(), multiResponse.size(), multiDecoded);
    const auto multi = parseWriteMultipleResponse(multiDecoded.frame);
    check(multi.valid, "写多个寄存器响应解析成功");
    checkU16(multi.address, 0, "回显起始地址");
    checkU16(multi.count, 2, "回显寄存器数量");

    section("异常响应的中文提示");
    const auto exceptionResponse = fromHex("01 83 02 C0 F1");
    DecodeResult exceptionDecoded;
    decodeRtuResponse(exceptionResponse.data(), exceptionResponse.size(), exceptionDecoded);
    const auto exceptionParsed = parseReadRegistersResponse(exceptionDecoded.frame, 2);
    check(!exceptionParsed.valid, "异常响应不算有效结果");
    check(exceptionParsed.exception == ExceptionCode::IllegalDataAddress, "异常码正确");
    check(!exceptionParsed.error.empty(), "给出可读的错误说明");
}

// ------------------------------------------------------------ 请求响应匹配

void testMatching() {
    section("请求与响应匹配（防错配）");
    const auto frame = fromHex("00 07 00 00 00 05 01 03 02 00 0A");
    DecodeResult decoded;
    decodeTcp(frame.data(), frame.size(), decoded);

    check(matchesRequest(decoded.frame, Transport::Tcp, 7, 1, 0x03), "事务号、单元号、功能码都相符");
    check(!matchesRequest(decoded.frame, Transport::Tcp, 8, 1, 0x03), "事务号不符时判为迟到响应");
    check(!matchesRequest(decoded.frame, Transport::Tcp, 7, 2, 0x03), "单元号不符时判为他人响应");
    check(!matchesRequest(decoded.frame, Transport::Tcp, 7, 1, 0x04), "功能码不符时判为他人响应");

    // 异常响应：功能码 0x83 = 0x03 | 0x80，必须能与请求 0x03 匹配上
    const auto exceptionFrame = fromHex("00 07 00 00 00 03 01 83 02");
    DecodeResult exceptionDecoded;
    decodeTcp(exceptionFrame.data(), exceptionFrame.size(), exceptionDecoded);
    check(matchesRequest(exceptionDecoded.frame, Transport::Tcp, 7, 1, 0x03),
          "异常响应的功能码按 0x03 | 0x80 放行");

    const auto rtuFrame = fromHex("01 03 02 00 0A 38 43");
    DecodeResult rtuDecoded;
    decodeRtuResponse(rtuFrame.data(), rtuFrame.size(), rtuDecoded);
    check(matchesRequest(rtuDecoded.frame, Transport::Rtu, 0, 1, 0x03), "RTU 比对单元号与功能码");
    check(!matchesRequest(rtuDecoded.frame, Transport::Rtu, 0, 3, 0x03), "RTU 单元号不符");
    check(!matchesRequest(rtuDecoded.frame, Transport::Rtu, 0, 1, 0x06), "RTU 功能码不符");
}

// ------------------------------------------------------------ 异常码映射

// README 声称覆盖异常码 0x01-0x04，这里把 0x01-0x06 全部固定住。
// 帧尾 CRC 由独立脚本按 0xA001/0xFFFF 复算得到，不是用被测实现生成的。
void testExceptionCodes() {
    section("异常码枚举与原始值映射");
    check(toExceptionCode(0x01) == ExceptionCode::IllegalFunction, "0x01 非法功能码");
    check(toExceptionCode(0x02) == ExceptionCode::IllegalDataAddress, "0x02 非法数据地址");
    check(toExceptionCode(0x03) == ExceptionCode::IllegalDataValue, "0x03 非法数据值");
    check(toExceptionCode(0x04) == ExceptionCode::SlaveDeviceFailure, "0x04 从站设备故障");
    check(toExceptionCode(0x05) == ExceptionCode::Acknowledge, "0x05 确认");
    check(toExceptionCode(0x06) == ExceptionCode::SlaveDeviceBusy, "0x06 从站设备忙");
    check(toExceptionCode(0x7F) == ExceptionCode::Unknown, "未定义异常码归为 Unknown");

    section("异常码可读文案");
    for (const uint8_t raw : {0x01, 0x02, 0x03, 0x04, 0x05, 0x06}) {
        const ExceptionCode code = toExceptionCode(raw);
        check(std::string(toString(code)).size() > 0, "toString 非空");
        check(!toChineseHint(code).empty(), "中文提示非空");
    }

    section("异常功能码判定");
    check(isExceptionFunction(0x83), "0x83 是异常功能码");
    check(isExceptionFunction(0x84), "0x84 是异常功能码");
    check(!isExceptionFunction(0x03), "0x03 不是异常功能码");
    check(!isExceptionFunction(0x00), "0x00 不是异常功能码");

    section("RTU 异常响应逐码解析");
    struct Case {
        const char* frame;
        ExceptionCode expect;
    };
    const Case cases[] = {
        {"01 83 01 80 F0", ExceptionCode::IllegalFunction},
        {"01 83 03 01 31", ExceptionCode::IllegalDataValue},
        {"01 83 04 40 F3", ExceptionCode::SlaveDeviceFailure},
        {"01 83 06 C1 32", ExceptionCode::SlaveDeviceBusy},
        {"01 84 04 42 C3", ExceptionCode::SlaveDeviceFailure},  // 功能码 0x04 的异常响应
    };
    for (const Case& item : cases) {
        const auto raw = fromHex(item.frame);
        DecodeResult decoded;
        const std::size_t consumed = decodeRtuResponse(raw.data(), raw.size(), decoded);
        check(consumed == 5, std::string("异常帧消费 5 字节 [") + item.frame + "]");
        check(decoded.status == DecodeStatus::Ok,
              std::string("异常帧状态为 Ok [") + item.frame + "]");
        check(decoded.frame.isException(),
              std::string("识别为异常帧 [") + item.frame + "]");
        check(decoded.frame.exceptionCode() == item.expect,
              std::string("异常码解析正确 [") + item.frame + "]");
    }
}

// ------------------------------------------------------------------ 工具

void testHexHelpers() {
    section("十六进制工具");
    const auto bytes = fromHex("01 03 00 0a");
    check(bytes.size() == 4, "解析出 4 字节");
    checkBytes(bytes, "01 03 00 0A", "输出统一为大写且空格分隔");
    check(fromHex("0103000A").size() == 4, "无空格写法同样可解析");
    check(fromHex("").empty(), "空字符串解析为空数组");
}

}  // namespace

int main() {
    std::printf("Modbus 协议层可移植测试（不依赖 Qt）\n");

    testCrc16();
    testPduBuilders();
    testEncoding();
    testDecodeTcp();
    testDecodeRtu();
    testResponseParsing();
    testMatching();
    testExceptionCodes();
    testHexHelpers();

    return test_support::summary();
}
