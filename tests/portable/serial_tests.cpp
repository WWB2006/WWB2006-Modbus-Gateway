// 串口链路测试：验证 RTU 侧的分帧行为、链路抽象一致性，
// 以及 SerialTransport 里「帧间静默间隔」的估算逻辑。
//
// 为什么不用真实串口：com0com（Windows）/ socat（Linux）都需要安装与管理员权限，
// 无法在 CI 里自动化。因此这里用与 SerialTransport 同一套语义的假链路
// （FakeSlaveTransport + TransportKind::Serial）验证**协议与分帧**，
// 而 SerialTransport 自身只保留「参数映射 + 静默间隔估算」这些可纯函数验证的部分。
// 真实串口的读写路径在 docs 里记录为手工验证项（需插线）。

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "fake_slave_transport.h"
#include "gateway/device/DeviceSession.h"
#include "gateway/protocol/ModbusCodec.h"
#include "test_support.h"

using test_support::check;
using test_support::checkBytes;
using test_support::checkU16;
using test_support::checkU64;
using test_support::section;
using test_support::valueAt;

using gateway::device::DeviceConfig;
using gateway::device::DeviceSession;
using gateway::device::TransactionResult;
using gateway::protocol::ExceptionCode;
using gateway::transport::TransportKind;

namespace {

DeviceConfig makeRtuConfig(uint8_t unitId, uint16_t timeoutMs = 100, uint8_t retry = 2) {
    DeviceConfig config;
    config.name = "rtu-device";
    config.transport = TransportKind::Serial;
    config.unitId = unitId;
    config.timeoutMs = timeoutMs;
    config.retry = retry;
    config.pollIntervalMs = 50;
    config.serialPort = "COM11";
    config.baudRate = 9600;
    return config;
}

struct Collector {
    int count = 0;
    TransactionResult last;
    void operator()(const TransactionResult& result) {
        last = result;
        ++count;
    }
};

// SerialTransport 的帧间静默估算逻辑（与 SerialTransport.cpp 中的实现保持一致）。
// 这里重写一份用于验证公式边界：3.5 字符时间在常见波特率下的取值范围。
int estimateFrameGapMs(int baudRate, int dataBits, char parity, int stopBits) {
    if (baudRate <= 0) {
        return 10;
    }
    const int bitsPerChar = 1 + dataBits + (parity == 'N' || parity == 'n' ? 0 : 1) + stopBits;
    const double ms = (bitsPerChar * 3.5) * 1000.0 / static_cast<double>(baudRate);
    int gap = static_cast<int>(ms) + 1;
    if (gap < 1) {
        gap = 1;
    }
    if (gap > 50) {
        gap = 50;
    }
    return gap;
}

void testRtuFrameEncoding() {
    section("RTU 组帧：单元号 + PDU + CRC（低字节在前）");

    // 读保持寄存器 0 起 10 个：01 03 00 00 00 0A + CRC C5 CD
    const auto pdu = gateway::protocol::buildReadHoldingRegisters(0, 10);
    const auto frame = gateway::protocol::encodeRtuRequest(1, pdu);
    checkBytes(frame, "01 03 00 00 00 0A C5 CD", "RTU 请求帧字节序");

    // CRC 低字节在前：报文里是 C5 CD，而不是 CD C5。
    checkU16(frame[frame.size() - 2], 0xC5, "CRC 低字节 C5 在前");
    checkU16(frame[frame.size() - 1], 0xCD, "CRC 高字节 CD 在后");

    // RTU 帧 = 单元号 1 + PDU 5 + CRC 2 = 8 字节；
    // TCP 帧 = MBAP 7 + PDU 5 = 12 字节。差值 = 12 - 8 = 4。
    const auto tcpFrame = gateway::protocol::encodeTcpRequest(1, 1, pdu);
    checkU64(tcpFrame.size(), frame.size() + 4, "TCP 帧比 RTU 帧长 4 字节（MBAP 7 - 单元号 1 - CRC 2）");
}

void testRtuDecodeRoundTrip() {
    section("RTU 解帧：请求帧与响应帧方向不同");

    const auto pdu = gateway::protocol::buildReadHoldingRegisters(0, 2);
    const auto request = gateway::protocol::encodeRtuRequest(1, pdu);

    gateway::protocol::DecodeResult decoded;
    const std::size_t consumed =
        gateway::protocol::decodeRtuRequest(request.data(), request.size(), decoded);
    checkU64(consumed, request.size(), "RTU 请求帧完整解出");
    checkU16(decoded.frame.unitId, 1, "解出单元号");
    checkU16(decoded.frame.functionCode, 0x03, "解出功能码");

    // 响应帧（0x03，4 字节数据）走 decodeRtuResponse 才认。
    const std::vector<uint8_t> responsePdu = {0x03, 0x04, 0x00, 0x0A, 0x00, 0x14};
    const auto response = gateway::protocol::encodeRtuRequest(1, responsePdu);
    gateway::protocol::DecodeResult responseDecoded;
    const std::size_t responseConsumed =
        gateway::protocol::decodeRtuResponse(response.data(), response.size(), responseDecoded);
    checkU64(responseConsumed, response.size(), "RTU 响应帧完整解出");
    checkU16(responseDecoded.frame.functionCode, 0x03, "解出响应功能码");
}

void testRtuSessionRead() {
    section("RTU 会话：通过串口链路读保持寄存器");

    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();
    slave->setRegister(0, 0x1234);
    slave->setRegister(1, 0x5678);

    DeviceSession session(makeRtuConfig(1));
    session.attach(*slave);
    Collector collector;
    session.setResultCallback(std::ref(collector));

    check(session.requestReadHoldingRegisters(0, 2), "RTU 请求已受理");
    slave->deliverAll();

    checkU64(collector.count, 1, "收到一次回调");
    check(collector.last.valid, "结果有效");
    checkU16(valueAt(collector.last.values, 0), 0x1234, "第 0 个寄存器");
    checkU16(valueAt(collector.last.values, 1), 0x5678, "第 1 个寄存器");
}

void testRtuFragmentAndCrc() {
    section("RTU 分帧：半包拼齐、坏 CRC 丢弃");

    // 半包：响应被拆成两段到达，只有凑齐后才解出。
    {
        auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
        slave->open();
        slave->setFragment(true);

        DeviceSession session(makeRtuConfig(1));
        session.attach(*slave);
        Collector collector;
        session.setResultCallback(std::ref(collector));

        session.requestReadHoldingRegisters(0, 2);
        slave->deliverNext();  // 只送达前半段
        checkU64(collector.count, 0, "半包不产生回调");
        slave->deliverNext();  // 送达后半段
        checkU64(collector.count, 1, "拼齐后产生一次回调");
        check(collector.last.valid, "半包拼齐后结果有效");
    }

    // 坏 CRC：整帧丢弃、不回调、继续等待（直到超时重传）。
    {
        auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
        slave->open();
        slave->setCorruptCrc(true);

        DeviceSession session(makeRtuConfig(1, 100, 0));
        session.attach(*slave);
        Collector collector;
        session.setResultCallback(std::ref(collector));

        session.requestReadHoldingRegisters(0, 2);
        slave->deliverAll();
        checkU64(collector.count, 0, "坏 CRC 帧被丢弃，无回调");
        checkU64(session.stats().crcErrors, 1, "CRC 错误计数为 1");

        session.tick(1000);
        checkU64(collector.count, 1, "超时后回调一次失败");
        check(collector.last.timedOut, "标记为超时");
    }
}

void testRtuException() {
    section("RTU 异常响应：不重传，直接给出中文提示");

    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();
    slave->setExceptionCode(0x02);  // 非法数据地址

    DeviceSession session(makeRtuConfig(1));
    session.attach(*slave);
    Collector collector;
    session.setResultCallback(std::ref(collector));

    session.requestReadHoldingRegisters(0, 2);
    slave->deliverAll();

    checkU64(collector.count, 1, "异常响应产生一次回调");
    check(collector.last.exception == ExceptionCode::IllegalDataAddress, "异常码 0x02");
    check(!collector.last.timedOut, "异常响应不算超时");
    checkU64(session.stats().retries, 0, "异常响应不触发重传");
}

void testRtuWrongUnitIdIgnored() {
    section("RTU 总线：单元号不匹配的响应被丢弃");

    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();
    slave->setResponseUnitId(7);  // 总线上别的从站的响应

    DeviceSession session(makeRtuConfig(1, 100, 0));
    session.attach(*slave);
    Collector collector;
    session.setResultCallback(std::ref(collector));

    session.requestReadHoldingRegisters(0, 2);
    slave->deliverAll();

    checkU64(collector.count, 0, "他人响应不触发回调");
    checkU64(session.stats().droppedResponses, 1, "计入丢弃响应");

    session.tick(1000);
    check(collector.last.timedOut, "最终以超时结束");
}

void testFrameGapEstimation() {
    section("串口帧间静默估算：3.5 字符时间");

    // 9600bps / 8N1 → 10 bit/字符 → 35ms/1000 ≈ 3.65ms → 向上取 4ms
    checkU64(static_cast<uint64_t>(estimateFrameGapMs(9600, 8, 'N', 1)), 4, "9600 8N1");
    // 19200bps → 约 1.8ms → 2ms
    checkU64(static_cast<uint64_t>(estimateFrameGapMs(19200, 8, 'N', 1)), 2, "19200 8N1");
    // 115200bps → 约 0.3ms → 至少 1ms
    checkU64(static_cast<uint64_t>(estimateFrameGapMs(115200, 8, 'N', 1)), 1, "115200 8N1 下限 1ms");
    // 1200bps → 约 29ms → 30ms
    checkU64(static_cast<uint64_t>(estimateFrameGapMs(1200, 8, 'N', 1)), 30, "1200 8N1");
    // 带校验位时每字符多 1 位：9600 8E1 → 11 bit/字符 → 38.5×1000/9600 ≈ 4.01ms → int 4 + 1 = 5ms
    checkU64(static_cast<uint64_t>(estimateFrameGapMs(9600, 8, 'E', 1)), 5, "9600 8E1（含校验位，每字符 11 位）");
    // 非法波特率回落为保守值
    checkU64(static_cast<uint64_t>(estimateFrameGapMs(0, 8, 'N', 1)), 10, "波特率为 0 时回落 10ms");
    // 极低波特率被上限截断，避免静默等待过久
    checkU64(static_cast<uint64_t>(estimateFrameGapMs(300, 8, 'N', 1)), 50, "300bps 触顶 50ms");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Modbus 串口链路测试（RTU 分帧与帧间静默，不依赖真实串口）\n");

    testRtuFrameEncoding();
    testRtuDecodeRoundTrip();
    testRtuSessionRead();
    testRtuFragmentAndCrc();
    testRtuException();
    testRtuWrongUnitIdIgnored();
    testFrameGapEstimation();

    return test_support::summary();
}
