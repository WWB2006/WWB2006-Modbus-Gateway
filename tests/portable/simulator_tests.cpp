// 阶段 3 从站模拟器测试（不依赖 Qt）。
//
// 测试策略：**闭环**。用主站侧的 ModbusCodec 造请求 → 交给从站 RequestHandler →
// 用主站侧的解析函数解响应。两侧互为参照，而不是拿一串手写字节去对答案 ——
// 手写字节只能验证「我认为对的」，闭环能验证「两边对同一个规范的理解一致」。
//
// 位区（0x01/0x02/0x05/0x0F）的字节序、异常码的选择、越界与数量的判定顺序，
// 都靠这组用例钉住。

#include <cstdio>
#include <cstring>

#include "gateway/protocol/ModbusCodec.h"
#include "gateway/sim/RegisterMap.h"
#include "gateway/sim/RequestHandler.h"
#include "gateway/sim/SimulatorConfig.h"
#include "test_support.h"

using gateway::protocol::DecodeResult;
using gateway::protocol::ExceptionCode;
using gateway::protocol::Frame;
using gateway::protocol::FunctionCode;
using gateway::protocol::Transport;
using gateway::sim::RegisterMap;
using gateway::sim::RequestHandler;
using gateway::sim::ResponsePdu;

namespace {

constexpr uint8_t kUnit = 1;

// 用主站侧的编解码闭环：请求 PDU -> TCP 帧 -> 解帧 -> 交给从站 -> 响应解帧。
// 返回从站的响应 PDU（异常响应时长度为 2）。
// functionCode 只用于在帧解不出时给出可定位的报错，不参与逻辑。
ResponsePdu roundTrip(RegisterMap& map, uint8_t functionCode, const std::vector<uint8_t>& pdu,
                      uint16_t transactionId = 1, uint8_t unitId = kUnit) {
    RequestHandler handler(map);
    handler.setUnitId(kUnit);

    const std::vector<uint8_t> frame =
        gateway::protocol::encodeTcpRequest(transactionId, unitId, pdu);

    DecodeResult decoded;
    const std::size_t consumed =
        gateway::protocol::decodeTcp(frame.data(), frame.size(), decoded);
    if (consumed == 0) {
        ResponsePdu broken;
        char text[128];
        std::snprintf(text, sizeof(text), "功能码 0x%02X 的请求帧无法解出，测试用例本身有问题",
                      static_cast<unsigned>(functionCode));
        broken.summary = text;
        return broken;
    }
    return handler.handleRequest(decoded.frame);
}

// 从站响应 PDU -> TCP 帧 -> 解帧，便于复用主站解析函数验证。
bool decodeResponse(const ResponsePdu& response, uint16_t transactionId, DecodeResult& out) {
    if (response.pdu.empty()) {
        return false;
    }
    const std::vector<uint8_t> frame =
        gateway::protocol::encodeTcpRequest(transactionId, kUnit, response.pdu);
    const std::size_t consumed =
        gateway::protocol::decodeTcp(frame.data(), frame.size(), out);
    return consumed > 0;
}

RegisterMap makeMap() {
    RegisterMap map;
    map.resizeHoldingRegisters(128);
    map.resizeInputRegisters(64);
    map.resizeCoils(64);
    map.resizeDiscreteInputs(32);
    map.setHoldingRegister(0, 100);
    map.setHoldingRegister(1, 250);
    map.setHoldingRegister(2, 380);
    map.setHoldingRegister(99, 42);
    map.setInputRegister(0, 220);
    map.setInputRegister(1, 50);
    map.setCoil(0, true);
    map.setCoil(1, false);
    map.setCoil(2, true);
    map.setDiscreteInput(0, true);
    map.setDiscreteInput(3, true);
    return map;
}

// --------------------------------------------------------------- 读寄存器

void testReadHoldingRegisters() {
    test_support::section("阶段 3 · 读保持寄存器（0x03）");
    RegisterMap map = makeMap();

    const auto response = roundTrip(map, 0x03, gateway::protocol::buildReadHoldingRegisters(0, 3));
    test_support::check(!response.pdu.empty(), "读保持寄存器应有响应");
    test_support::check(response.exception == ExceptionCode::None, "读保持寄存器不应返回异常");
    // 功能码 + 字节数 6 + 三个大端寄存器值
    test_support::checkBytes(response.pdu, "03 06 00 64 00 FA 01 7C", "读 3 个保持寄存器");

    DecodeResult decoded;
    test_support::check(decodeResponse(response, 1, decoded), "响应可被主站解码");
    const auto parsed = gateway::protocol::parseReadRegistersResponse(decoded.frame, 3);
    test_support::check(parsed.valid, "主站解析应成功");
    test_support::checkU16(test_support::valueAt(parsed.values, 0), 100, "寄存器 0");
    test_support::checkU16(test_support::valueAt(parsed.values, 1), 250, "寄存器 1");
    test_support::checkU16(test_support::valueAt(parsed.values, 2), 380, "寄存器 2");
}

void testReadInputRegisters() {
    test_support::section("阶段 3 · 读输入寄存器（0x04）");
    RegisterMap map = makeMap();

    const auto response = roundTrip(map, 0x04, gateway::protocol::buildReadInputRegisters(0, 2));
    test_support::checkBytes(response.pdu, "04 04 00 DC 00 32", "读 2 个输入寄存器");

    // 输入寄存器只能通过 0x04 读，没有对应的写功能码。
    // 用 0x10 写「输入寄存器区」在协议上不存在这个概念：
    // 0x10 写的永远是保持寄存器，所以这里验证的是「两个区确实是分开的」——
    // 写保持寄存器地址 0 不应改动输入寄存器地址 0。
    const std::vector<uint16_t> values{999};
    const auto write =
        roundTrip(map, 0x10, gateway::protocol::buildWriteMultipleRegisters(0, values));
    test_support::check(write.exception == ExceptionCode::None, "写保持寄存器应成功");
    test_support::checkU16(map.holdingRegister(0), 999, "保持寄存器 0 已写入");
    test_support::checkU16(map.inputRegister(0), 220, "输入寄存器 0 未被改动（两区独立）");
}

// ----------------------------------------------------------------- 位读取

void testReadCoilsAndDiscreteInputs() {
    test_support::section("阶段 3 · 读线圈与离散输入（0x01 / 0x02）");

    {
        RegisterMap map = makeMap();
        // 线圈 0=true, 1=false, 2=true -> 位模式 101 -> 0x05
        const auto response = roundTrip(map, 0x01, gateway::protocol::buildReadCoils(0, 3));
        test_support::checkBytes(response.pdu, "01 01 05", "读线圈 0-2：位模式 101 = 0x05");

        // 闭环：用主站侧的位解析函数解回来
        DecodeResult decoded;
        test_support::check(decodeResponse(response, 1, decoded), "位读响应可解码");
        const auto parsed = gateway::protocol::parseReadBitsResponse(decoded.frame, 3);
        test_support::check(parsed.valid, "主站位解析应成功");
        test_support::check(parsed.bits.size() == 3, "应解出 3 个位");
        test_support::check(parsed.bits[0] && !parsed.bits[1] && parsed.bits[2],
                            "位序列应为 1,0,1");
    }
    {
        RegisterMap map = makeMap();
        const auto response =
            roundTrip(map, 0x02, gateway::protocol::buildReadDiscreteInputs(0, 4));
        // 位 0 = 1，位 3 = 1 -> 二进制 1001 -> 0x09
        test_support::checkBytes(response.pdu, "02 01 09", "读离散输入 0-3：位模式 1001 = 0x09");
    }
    {
        // 位区跨字节：读 12 个位，位 0 与位 9 为 1 -> 两个字节 0x01 0x02
        RegisterMap map;
        map.resizeCoils(16);
        map.setCoil(0, true);
        map.setCoil(9, true);
        const auto response = roundTrip(map, 0x01, gateway::protocol::buildReadCoils(0, 12));
        test_support::checkBytes(response.pdu, "01 02 01 02", "位区跨字节打包（LSB first）");
    }
    {
        // 单个位：LSB 必须在字节最低位，写成 MSB 会得到 0x80
        RegisterMap map;
        map.resizeCoils(8);
        map.setCoil(0, true);
        const auto response = roundTrip(map, 0x01, gateway::protocol::buildReadCoils(0, 1));
        test_support::checkBytes(response.pdu, "01 01 01", "单个位取最低位，不是最高位");
    }
    {
        // 9 个位占 2 字节：末字节只有 1 位有效，其余必须补零
        RegisterMap map;
        map.resizeCoils(16);
        for (int i = 0; i < 9; ++i) {
            map.setCoil(static_cast<uint16_t>(i), true);
        }
        const auto response = roundTrip(map, 0x01, gateway::protocol::buildReadCoils(0, 9));
        test_support::checkBytes(response.pdu, "01 02 FF 01", "9 个位占 2 字节，末字节补零");
    }
}

// ----------------------------------------------------------------- 写操作

void testWriteSingleRegister() {
    test_support::section("阶段 3 · 写单个寄存器（0x06）");
    RegisterMap map = makeMap();

    const auto response = roundTrip(map, 0x06, gateway::protocol::buildWriteSingleRegister(5, 777));
    test_support::checkBytes(response.pdu, "06 00 05 03 09", "写单个寄存器应回显地址与值");
    test_support::checkU16(map.holdingRegister(5), 777, "寄存器 5 已写入");

    const auto parsed = gateway::protocol::parseWriteSingleResponse(
        [&] {
            DecodeResult decoded;
            decodeResponse(response, 1, decoded);
            return decoded.frame;
        }());
    test_support::check(parsed.valid, "主站可解析写单个寄存器响应");
    test_support::checkU16(parsed.address, 5, "回显地址");
    test_support::checkU16(parsed.value, 777, "回显值");

    // 越界写：地址 127 存在（size 128），地址 128 不存在
    const auto ok = roundTrip(map, 0x06, gateway::protocol::buildWriteSingleRegister(127, 1));
    test_support::check(ok.exception == ExceptionCode::None, "写最后一个合法地址应成功");
    const auto bad = roundTrip(map, 0x06, gateway::protocol::buildWriteSingleRegister(128, 1));
    test_support::check(bad.exception == ExceptionCode::IllegalDataAddress,
                        "写越界地址应返回 0x02");
}

void testWriteMultipleRegisters() {
    test_support::section("阶段 3 · 写多个寄存器（0x10）");
    RegisterMap map = makeMap();

    const std::vector<uint16_t> values{11, 22, 33};
    const auto response =
        roundTrip(map, 0x10, gateway::protocol::buildWriteMultipleRegisters(10, values));
    test_support::checkBytes(response.pdu, "10 00 0A 00 03", "写多个寄存器应回显起始地址与数量");
    test_support::checkU16(map.holdingRegister(10), 11, "寄存器 10 已写入");
    test_support::checkU16(map.holdingRegister(12), 33, "寄存器 12 已写入");

    // 字节数写错（数量 3 但字节数 8）：应报 0x03 而不是勉强解析
    {
        std::vector<uint8_t> pdu{0x10, 0x00, 0x0A, 0x00, 0x03, 0x08,
                                 0x00, 0x0B, 0x00, 0x16, 0x00, 0x21, 0x00, 0x00};
        const auto bad = roundTrip(map, 0x10, pdu);
        test_support::check(bad.exception == ExceptionCode::IllegalDataValue,
                            "字节数与数量不一致应返回 0x03");
    }
    // 数量字段超规范（124 > 123）：应报 0x03，而不是因为「地址够放」就成功。
    // 这条钉住的是判定顺序：数量本身越规范时先报 0x03，与地址落在哪里无关。
    //
    // 关键：数量字段并不需要真的带 248 字节数据。Modbus PDU 上限是 253 字节，
    // 「数量 124 + 完整数据」会超长，主站侧的构造函数因此直接拒绝，
    // 构造不出这条非法请求。所以这里**故意让字节数字段与实际数据不符**：
    // 数量说 124，数据只给 1 个寄存器 —— 这正是现场坏主站会发出的帧，
    // 从站必须按「先将数量与字节数一致性校验」的原则拒绝掉。
    {
        std::vector<uint8_t> pdu{0x10, 0x00, 0x00, 0x00, 0x7C, 0x02, 0x00, 0x07};  // 数量 124，字节数 2
        const auto bad = roundTrip(map, 0x10, pdu);
        test_support::checkBytes(bad.pdu, "90 03", "数量字段超过 123 应返回 0x03");
    }
    // 数量合法（123）但字节数与数量不符：同样应报 0x03
    {
        std::vector<uint8_t> pdu{0x10, 0x00, 0x00, 0x00, 0x03, 0x04, 0x00, 0x07, 0x00, 0x08};
        // 数量 3 需要 6 字节数据，只给了 4 字节
        const auto bad = roundTrip(map, 0x10, pdu);
        test_support::checkBytes(bad.pdu, "90 03", "字节数与数量不符应返回 0x03");
    }
}

void testWriteCoils() {
    test_support::section("阶段 3 · 写线圈（0x05 / 0x0F）");
    RegisterMap map = makeMap();

    // 写单个线圈：0xFF00 = ON
    {
        std::vector<uint8_t> pdu{0x05, 0x00, 0x03, 0xFF, 0x00};
        const auto response = roundTrip(map, 0x05, pdu);
        test_support::checkBytes(response.pdu, "05 00 03 FF 00", "写单个线圈 ON 应回显");
        test_support::check(map.coil(3), "线圈 3 已置位");
    }
    // 0x0000 = OFF
    {
        std::vector<uint8_t> pdu{0x05, 0x00, 0x00, 0x00, 0x00};
        const auto response = roundTrip(map, 0x05, pdu);
        test_support::checkBytes(response.pdu, "05 00 00 00 00", "写单个线圈 OFF 应回显");
        test_support::check(!map.coil(0), "线圈 0 已复位");
    }
    // 非法值 0x1234：必须报 0x03，不能按「非零即 ON」猜
    {
        std::vector<uint8_t> pdu{0x05, 0x00, 0x00, 0x12, 0x34};
        const auto bad = roundTrip(map, 0x05, pdu);
        test_support::check(bad.exception == ExceptionCode::IllegalDataValue,
                            "写单个线圈的非法值应返回 0x03");
    }
    // 写多个线圈：位 0、2、4 置位 -> 0x15
    {
        std::vector<uint8_t> pdu{0x0F, 0x00, 0x10, 0x00, 0x05, 0x01, 0x15};
        const auto response = roundTrip(map, 0x0F, pdu);
        test_support::checkBytes(response.pdu, "0F 00 10 00 05", "写多个线圈应回显地址与数量");
        test_support::check(map.coil(16), "线圈 16 已置位");
        test_support::check(!map.coil(17), "线圈 17 应保持复位");
        test_support::check(map.coil(18), "线圈 18 已置位");
        test_support::check(map.coil(20), "线圈 20 已置位");
    }
}

// ----------------------------------------------------------------- 异常路径

void testExceptionPaths() {
    test_support::section("阶段 3 · 异常路径");

    // 1) 地址越界 -> 0x02
    {
        RegisterMap map = makeMap();
        const auto response =
            roundTrip(map, 0x03, gateway::protocol::buildReadHoldingRegisters(127, 5));
        test_support::checkBytes(response.pdu, "83 02", "读越界范围应返回 0x02");
        test_support::check(response.exception == ExceptionCode::IllegalDataAddress,
                            "异常码应被识别");
    }
    // 2) 数量为 0 -> 0x03（主站侧构造会拒绝，这里直接手写 PDU）
    {
        RegisterMap map = makeMap();
        std::vector<uint8_t> pdu{0x03, 0x00, 0x00, 0x00, 0x00};
        const auto response = roundTrip(map, 0x03, pdu);
        test_support::checkBytes(response.pdu, "83 03", "数量为 0 应返回 0x03");
    }
    // 3) 数量超规范上限（126 > 125）-> 0x03
    {
        RegisterMap map;
        map.resizeHoldingRegisters(200);
        std::vector<uint8_t> pdu{0x03, 0x00, 0x00, 0x00, 0x7E};  // 126
        const auto response = roundTrip(map, 0x03, pdu);
        test_support::checkBytes(response.pdu, "83 03", "读数量超过 125 应返回 0x03");
    }
    // 4) 未实现的功能码 0x17 -> 0x01
    {
        RegisterMap map = makeMap();
        std::vector<uint8_t> pdu{0x17, 0x00, 0x00};
        const auto response = roundTrip(map, 0x17, pdu);
        test_support::checkBytes(response.pdu, "97 01", "未实现功能码应返回 0x01");
    }
    // 5) 数据域长度不对 -> 0x03
    {
        RegisterMap map = makeMap();
        std::vector<uint8_t> pdu{0x03, 0x00};  // 少了数量字段
        const auto response = roundTrip(map, 0x03, pdu);
        test_support::checkBytes(response.pdu, "83 03", "数据域长度不对应返回 0x03");
    }
    // 6) 异常响应的功能码最高位必须是 1
    {
        RegisterMap map = makeMap();
        const auto response =
            roundTrip(map, 0x03, gateway::protocol::buildReadHoldingRegisters(200, 1));
        test_support::check(response.pdu.size() == 2, "异常响应应固定 2 字节");
        test_support::check((response.pdu[0] & 0x80u) != 0, "异常响应的功能码最高位应为 1");
        test_support::check(response.pdu[0] == (0x03 | 0x80u), "0x03 的异常功能码应为 0x83");
    }
}

// ------------------------------------------------------------- 单元号过滤

void testUnitIdFiltering() {
    test_support::section("阶段 3 · 单元号过滤");
    RegisterMap map = makeMap();

    // 请求发给 2 号从站，本模拟器是 1 号：必须不应答（空 PDU）
    const auto other = roundTrip(map, 0x03, gateway::protocol::buildReadHoldingRegisters(0, 1),
                                 1, 2);
    test_support::check(other.pdu.empty(), "单元号不匹配时不应答");
    test_support::check(other.summary.find("不是本从站") != std::string::npos,
                        "应给出「不是本从站」的说明");

    // 广播地址 0 在默认配置下不应答
    const auto broadcast = roundTrip(map, 0x03,
                                     gateway::protocol::buildReadHoldingRegisters(0, 1), 1, 0);
    test_support::check(broadcast.pdu.empty(), "广播请求默认不应答");

    // 本机地址 1 正常应答
    const auto own = roundTrip(map, 0x03, gateway::protocol::buildReadHoldingRegisters(0, 1), 1, 1);
    test_support::check(!own.pdu.empty(), "本从站地址应正常应答");
}

// ----------------------------------------------------------- 事务号与粘包

void testTransactionIdEcho() {
    test_support::section("阶段 3 · 事务号回显");
    RegisterMap map = makeMap();

    // 事务号必须原样回送：主站靠它匹配请求与响应，模拟器自己生成一个会让主站全部超时。
    const auto response =
        roundTrip(map, 0x03, gateway::protocol::buildReadHoldingRegisters(0, 1), 0xABCD);
    DecodeResult decoded;
    // decodeResponse 里固定用事务号 1，这里改用真实事务号自己组帧
    const std::vector<uint8_t> frame =
        gateway::protocol::encodeTcpRequest(0xABCD, kUnit, response.pdu);
    const std::size_t consumed =
        gateway::protocol::decodeTcp(frame.data(), frame.size(), decoded);
    test_support::check(consumed > 0, "响应可解码");
    test_support::checkU16(decoded.frame.transactionId, 0xABCD, "事务号应原样回送");
    test_support::check(decoded.frame.unitId == kUnit, "单元号应为从站地址");
}

// ------------------------------------------------------------- 配置加载

void testConfigParsing() {
    test_support::section("阶段 3 · 配置解析");
    gateway::sim::SimulatorConfig config;

    // 用一个临时文件验证：配置文件是本机真实路径，测试里不依赖工作目录
    const std::string path = "build_portable/test_slave_config.json";
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        test_support::check(file != nullptr, "可以创建临时配置文件");
        if (file != nullptr) {
            const char* text =
                "{\n"
                "  \"simulator\": { \"port\": 15020, \"unitId\": 7, \"verbose\": false },\n"
                "  \"registers\": {\n"
                "    \"holding\":  { \"size\": 16, \"values\": { \"0\": 111, \"15\": 222 } },\n"
                "    \"input\":    { \"size\": 8 },\n"
                "    \"coils\":    { \"size\": 8, \"values\": { \"0\": true, \"7\": true } },\n"
                "    \"discreteInputs\": { \"size\": 4, \"values\": { \"1\": true } }\n"
                "  }\n"
                "}\n";
            std::fwrite(text, 1, std::strlen(text), file);
            std::fclose(file);
        }
    }

    std::string error;
    const bool ok = gateway::sim::loadSimulatorConfig(path, config, error);
    test_support::check(ok, std::string("配置应解析成功（") + error + "）");
    if (ok) {
        test_support::checkU16(config.port, 15020, "端口");
        test_support::check(config.unitId == 7, "从站地址");
        test_support::check(!config.verbose, "verbose");
        test_support::checkU64(config.holdingSize, 16, "保持寄存器数量");
        test_support::checkU64(config.holdingValues.size(), 2, "保持寄存器初值条数");
        test_support::checkU64(config.holdingValues.at(0), 111, "地址 0 初值");
        test_support::checkU64(config.holdingValues.at(15), 222, "地址 15 初值");
        test_support::checkU64(config.coilValues.size(), 2, "线圈初值条数");
        test_support::check(config.discreteInputValues.at(1), "离散输入 1 应为 true");

        RegisterMap map;
        gateway::sim::applyTo(map, config);
        test_support::checkU16(map.holdingRegister(0), 111, "applyTo 写入地址 0");
        test_support::checkU16(map.holdingRegister(15), 222, "applyTo 写入地址 15");
        test_support::check(map.coil(7), "applyTo 写入线圈 7");
        test_support::check(!map.coil(1), "未配置的线圈应为 false");
    }

    // 初值地址超出 size：必须报错而不是静默丢弃
    {
        const std::string badPath = "build_portable/test_slave_config_bad.json";
        std::FILE* file = std::fopen(badPath.c_str(), "wb");
        if (file != nullptr) {
            const char* text =
                "{ \"registers\": { \"holding\": { \"size\": 4, \"values\": { \"9\": 1 } } } }";
            std::fwrite(text, 1, std::strlen(text), file);
            std::fclose(file);
        }
        gateway::sim::SimulatorConfig bad;
        std::string badError;
        const bool badOk = gateway::sim::loadSimulatorConfig(badPath, bad, badError);
        test_support::check(!badOk, "初值地址超出 size 应解析失败");
        test_support::check(badError.find("超出了 size") != std::string::npos,
                            std::string("错误信息应说明越界（实际：") + badError + "）");
    }

    // 语法错误：必须报出定位信息
    {
        const std::string syntaxPath = "build_portable/test_slave_config_syntax.json";
        std::FILE* file = std::fopen(syntaxPath.c_str(), "wb");
        if (file != nullptr) {
            const char* text = "{ \"registers\": { \"holding\": { \"size\": 4 } ";
            std::fwrite(text, 1, std::strlen(text), file);
            std::fclose(file);
        }
        gateway::sim::SimulatorConfig broken;
        std::string brokenError;
        const bool brokenOk = gateway::sim::loadSimulatorConfig(syntaxPath, broken, brokenError);
        test_support::check(!brokenOk, "缺少闭合括号应解析失败");
        test_support::check(brokenError.find("行") != std::string::npos,
                            std::string("错误信息应含行列定位（实际：") + brokenError + "）");
    }
}

// --------------------------------------------------------------- 寄存器映射

void testRegisterMapLimits() {
    test_support::section("阶段 3 · 寄存器映射边界");
    RegisterMap map;
    map.resizeHoldingRegisters(10);
    map.resizeCoils(10);

    ExceptionCode exception = ExceptionCode::None;
    std::vector<uint16_t> values;
    std::vector<bool> bits;

    // 整段刚好在末尾：合法
    test_support::check(map.readHoldingRegisters(0, 10, values, exception), "0 起 10 个合法");
    test_support::check(map.readHoldingRegisters(5, 5, values, exception), "5 起 5 个合法");
    // 超出末尾一个：非法
    test_support::check(!map.readHoldingRegisters(5, 6, values, exception), "5 起 6 个越界");
    test_support::check(exception == ExceptionCode::IllegalDataAddress, "越界应报 0x02");
    // 地址 + 数量在 16 位边界回绕：不能因为回绕而被判合法
    test_support::check(!map.readHoldingRegisters(0xFFFF, 2, values, exception),
                        "0xFFFF + 2 回绕不应被判合法");
    // 数量 0：非法
    test_support::check(!map.readHoldingRegisters(0, 0, values, exception), "数量 0 非法");

    // 位区的地址判定顺序：先地址（0x02）后数量（0x03）。
    // 这里构造「地址越界 + 数量也是 0」的组合，应按地址报 0x02。
    test_support::check(!map.readCoils(20, 0, bits, exception), "越界地址 + 数量 0 应失败");
    test_support::check(exception == ExceptionCode::IllegalDataAddress,
                        "先判地址：应报 0x02 而不是 0x03");
}

}  // namespace

int main() {
    std::printf("阶段 3 从站模拟器测试\n");

    testReadHoldingRegisters();
    testReadInputRegisters();
    testReadCoilsAndDiscreteInputs();
    testWriteSingleRegister();
    testWriteMultipleRegisters();
    testWriteCoils();
    testExceptionPaths();
    testUnitIdFiltering();
    testTransactionIdEcho();
    testConfigParsing();
    testRegisterMapLimits();

    return test_support::summary();
}
