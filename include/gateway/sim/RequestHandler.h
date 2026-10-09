#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gateway/protocol/ExceptionCode.h"
#include "gateway/protocol/ModbusFrame.h"
#include "gateway/sim/RegisterMap.h"

namespace gateway::sim {

// 请求处理的结果。response 是完整的「功能码 + 数据域」PDU；
// 异常响应时 response 为「功能码 | 0x80 + 异常码」，长度恒为 2。
struct ResponsePdu {
    std::vector<uint8_t> pdu;
    protocol::ExceptionCode exception = protocol::ExceptionCode::None;
    // 为日志保留：这条请求做了什么（\"读保持寄存器 0 起 10 个\" 之类）。
    std::string summary;
};

// 从站请求处理器：把一条请求 PDU 变成一条响应 PDU。
//
// 这是模拟器里唯一有判断逻辑的部分，因此被刻意做成**不依赖网络、不依赖 Qt** 的纯函数，
// 可以直接用字节数组喂进去、用字节数组断言出来（见 tests/portable/simulator_tests.cpp）。
// TCP 与 RTU 两种链路只是外面套的帧头不同，处理逻辑完全复用这一份。
//
// 与主站侧的 ModbusCodec 是一对：主站 buildReadHoldingRegisters 造请求，
// 从站 handleRequest 消费它；主站 parseReadRegistersResponse 解响应，
// 从站造出来的响应必须能被它解开 —— 测试里就是这么闭环验证的。
class RequestHandler {
public:
    // 从站地址。TCP 上通常还要比 MBAP 的单元号，RTU 上则必须比，
    // 因为串口总线上会有多个从站，地址不匹配的请求不能应答。
    explicit RequestHandler(RegisterMap& map) : map_(&map) {}

    // 允许先构造、后绑定数据区：SlaveSimulator 里数据区在构造参数中，
    // 而处理器又是成员，成员初始化顺序不允许互相引用，因此留一个空构造再 setMap。
    RequestHandler() = default;

    void setMap(RegisterMap& map) { map_ = &map; }

    void setUnitId(uint8_t unitId) { unitId_ = unitId; }
    uint8_t unitId() const noexcept { return unitId_; }

    // 仅 TCP 使用：把「非法协议标识」这类链路层错误也计入统计。
    void setSupportBroadcast(bool enabled) { broadcast_ = enabled; }

    // 处理一条已解帧的请求。
    //
    // 返回的 ResponsePdu::pdu 为空表示「不应答」：单元号不匹配（总线上是别的从站）、
    // 或者功能码不属于本项目实现的范围。主站此时只会等到超时，这正是现场的真实表现。
    ResponsePdu handleRequest(const protocol::Frame& request);

    // 已处理请求与异常响应的计数，供日志与阶段 7 的指标使用。
    uint64_t handledRequests() const noexcept { return handled_; }
    uint64_t exceptionResponses() const noexcept { return exceptions_; }

private:
    ResponsePdu handleReadBits(const protocol::Frame& request, uint8_t functionCode);
    ResponsePdu handleReadRegisters(const protocol::Frame& request, uint8_t functionCode);
    ResponsePdu handleWriteSingleCoil(const protocol::Frame& request);
    ResponsePdu handleWriteSingleRegister(const protocol::Frame& request);
    ResponsePdu handleWriteMultipleCoils(const protocol::Frame& request);
    ResponsePdu handleWriteMultipleRegisters(const protocol::Frame& request);

    // 构造异常响应：功能码最高位置 1 + 异常码。
    ResponsePdu makeException(uint8_t functionCode, protocol::ExceptionCode code,
                              const std::string& why);

    RegisterMap* map_ = nullptr;
    uint8_t unitId_ = 1;
    bool broadcast_ = false;
    uint64_t handled_ = 0;
    uint64_t exceptions_ = 0;
};

// 位区打包 / 解包：位在字节里是低位在前（第一个寄存器对应字节的最低位）。
// 规范里这条最容易写反，因此单独提出来并配了对照用例。
std::vector<uint8_t> packBits(const std::vector<bool>& bits);
bool unpackBits(const uint8_t* data, std::size_t byteCount, std::size_t bitCount,
                std::vector<bool>& out);

}  // namespace gateway::sim
