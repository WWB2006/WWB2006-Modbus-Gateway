// 设备层测试：用假链路驱动完整的「请求 → 超时/重传 → 响应/异常 → 回调」流程。
// 不依赖 Qt，也不需要真实设备或网络。

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "fake_slave_transport.h"
#include "gateway/device/DeviceManager.h"
#include "gateway/device/DeviceSession.h"
#include "gateway/transport/ByteAccumulator.h"
#include "test_support.h"

using test_support::check;
using test_support::checkU16;
using test_support::checkU64;
using test_support::section;
using test_support::valueAt;

using gateway::device::DeviceConfig;
using gateway::device::DeviceManager;
using gateway::device::DeviceSession;
using gateway::device::TransactionResult;
using gateway::protocol::ExceptionCode;
using gateway::transport::ByteAccumulator;
using gateway::transport::TransportKind;
using gateway::transport::TransportState;

namespace {

DeviceConfig makeConfig(const std::string& name, TransportKind kind, uint8_t unitId) {
    DeviceConfig config;
    config.name = name;
    config.transport = kind;
    config.unitId = unitId;
    config.timeoutMs = 100;
    config.retry = 2;
    config.pollIntervalMs = 50;
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

void testRtuRead() {
    section("RTU 读保持寄存器：正常响应");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    test_support::FakeSlaveTransport* raw = slave.get();
    raw->open();

    DeviceSession session(makeConfig("meter-01", TransportKind::Serial, 1));
    session.attach(*raw);
    Collector collector;
    session.setResultCallback([&collector](const TransactionResult& r) { collector(r); });

    session.tick(0);
    check(session.requestReadHoldingRegisters(0, 4), "请求被接受");
    check(session.busy(), "进入等待响应状态");
    checkU64(session.stats().requests, 1, "请求计数为 1");

    raw->deliverAll();
    checkU64(collector.count, 1, "收到一次结果回调");
    check(collector.last.valid, "结果有效");
    checkU64(collector.last.values.size(), 4, "取回 4 个寄存器");
    checkU16(valueAt(collector.last.values, 0), 0, "寄存器 0");
    checkU16(valueAt(collector.last.values, 1), 10, "寄存器 1");
    checkU16(valueAt(collector.last.values, 3), 30, "寄存器 3");
    check(!session.busy(), "事务结束");
    checkU64(session.stats().responses, 1, "响应计数为 1");
}

void testTimeoutAndRetry() {
    section("超时与有限重传");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();
    slave->setResponding(false);  // 设备离线：收到请求但不回应

    DeviceSession session(makeConfig("meter-02", TransportKind::Serial, 1));
    session.attach(*slave);
    Collector collector;
    session.setResultCallback([&collector](const TransactionResult& r) { collector(r); });

    session.tick(0);
    session.requestReadHoldingRegisters(0, 2);
    checkU64(slave->sentFrames, 1, "首次发送");

    session.tick(99);
    checkU64(slave->sentFrames, 1, "未到超时时间不重传");
    session.tick(100);
    checkU64(slave->sentFrames, 2, "第一次超时后重传");
    session.tick(200);
    checkU64(slave->sentFrames, 3, "第二次超时后重传");
    session.tick(300);
    checkU64(slave->sentFrames, 3, "达到重传上限后不再发送");

    check(collector.count == 1, "只上报一次失败结果");
    check(collector.last.timedOut, "标记为超时");
    check(!collector.last.valid, "超时结果不是有效数据");
    checkU64(session.stats().timeouts, 3, "超时计数为 3");
    checkU64(session.stats().retries, 2, "重传次数等于 retry 配置");
    checkU64(session.stats().requests, 1, "逻辑请求数仍为 1");
    check(!session.busy(), "失败后释放在途事务");
}

void testBadCrc() {
    section("坏 CRC：整帧丢弃并继续等待");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();
    slave->setCorruptCrc(true);

    DeviceSession session(makeConfig("meter-03", TransportKind::Serial, 1));
    session.attach(*slave);
    Collector collector;
    session.setResultCallback([&collector](const TransactionResult& r) { collector(r); });

    session.tick(0);
    session.requestReadHoldingRegisters(0, 2);
    slave->deliverAll();

    checkU64(session.stats().crcErrors, 1, "CRC 错误计数为 1（整帧丢弃，不重复计数）");
    checkU64(session.stats().responses, 0, "没有产生有效响应");
    check(collector.count == 0, "不回调错误结果，继续等正确的响应");
    check(session.busy(), "事务仍在进行");
    checkU64(session.bufferedBytes(), 0, "误码帧已从缓冲区清除");

    session.tick(100);
    checkU64(slave->sentFrames, 2, "超时后重传");
}

void testFragmentArrival() {
    section("半包：分两次到达");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();
    slave->setFragment(true);

    DeviceSession session(makeConfig("meter-04", TransportKind::Serial, 1));
    session.attach(*slave);
    Collector collector;
    session.setResultCallback([&collector](const TransactionResult& r) { collector(r); });

    session.tick(0);
    session.requestReadHoldingRegisters(0, 4);

    slave->deliverNext();
    check(session.busy(), "只有半帧时不结束事务");
    check(session.bufferedBytes() > 0, "剩余字节留在缓冲区");
    check(collector.count == 0, "未触发结果回调");

    slave->deliverNext();
    check(collector.last.valid, "凑齐后解析成功");
    checkU64(collector.last.values.size(), 4, "取回 4 个寄存器");
    checkU64(session.bufferedBytes(), 0, "缓冲区已清空");
}

void testStickyFrames() {
    section("粘包：一次到达两帧，第二帧按迟到响应处理");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();
    slave->setResponding(false);  // 本用例手工构造到达数据

    DeviceSession session(makeConfig("meter-05", TransportKind::Serial, 1));
    session.attach(*slave);
    Collector collector;
    session.setResultCallback([&collector](const TransactionResult& r) { collector(r); });

    session.tick(0);
    session.requestReadHoldingRegisters(0, 1);

    const auto oneResponse =
        gateway::protocol::encodeRtuRequest(1, gateway::protocol::fromHex("03 02 00 0A"));
    std::vector<uint8_t> sticky = oneResponse;
    sticky.insert(sticky.end(), oneResponse.begin(), oneResponse.end());
    session.onBytes(sticky.data(), sticky.size());

    check(collector.last.valid, "第一帧被解出");
    check(!collector.last.values.empty() && collector.last.values[0] == 10, "寄存器值为 10");
    checkU64(session.stats().droppedResponses, 1, "第二帧因无在途事务被计为迟到响应");
    checkU64(session.bufferedBytes(), 0, "两帧都被消费，缓冲区无残留");
}

void testLateResponseTcp() {
    section("TCP：事务号错配的响应被丢弃，重传后拿到正确结果");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Tcp);
    slave->open();
    slave->setResponseTransactionId(99);  // 故意回一个错误的事务号

    DeviceSession session(makeConfig("plc-01", TransportKind::Tcp, 1));
    session.attach(*slave);
    Collector collector;
    session.setResultCallback([&collector](const TransactionResult& r) { collector(r); });

    session.tick(0);
    session.requestReadHoldingRegisters(0, 2);
    slave->deliverAll();

    check(collector.count == 0, "错配的响应不产生结果");
    checkU64(session.stats().droppedResponses, 1, "计入丢弃响应");
    check(session.busy(), "仍在等待正确的响应");

    slave->clearResponseTransactionId();
    session.tick(100);  // 超时后重传
    slave->deliverAll();
    check(collector.last.valid, "重传后拿到有效结果");
    checkU64(session.stats().retries, 1, "发生一次重传");
}

void testExceptionResponse() {
    section("异常响应：不重传，直接上报中文提示");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();
    slave->setExceptionCode(0x02);

    DeviceSession session(makeConfig("meter-06", TransportKind::Serial, 1));
    session.attach(*slave);
    Collector collector;
    session.setResultCallback([&collector](const TransactionResult& r) { collector(r); });

    session.tick(0);
    session.requestReadHoldingRegisters(0, 2);
    slave->deliverAll();

    check(collector.count == 1, "异常响应会回调一次");
    check(!collector.last.valid, "异常响应不是有效数据");
    check(collector.last.exception == ExceptionCode::IllegalDataAddress, "异常码为 0x02");
    check(!collector.last.error.empty(), "带有可读的错误说明");
    checkU64(session.stats().exceptions, 1, "异常计数为 1");
    checkU64(session.stats().responses, 1, "异常响应也计入响应");
    checkU64(slave->sentFrames, 1, "异常响应不触发重传");
    check(!session.busy(), "事务结束");
}

void testWriteSingleRegister() {
    section("写单个寄存器");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();

    DeviceSession session(makeConfig("meter-07", TransportKind::Serial, 1));
    session.attach(*slave);
    Collector collector;
    session.setResultCallback([&collector](const TransactionResult& r) { collector(r); });

    session.tick(0);
    check(session.requestWriteSingleRegister(5, 1234), "写请求被接受");
    slave->deliverAll();

    check(collector.last.valid, "写结果有效");
    checkU16(collector.last.value, 1234, "回显写入值");
    checkU16(slave->registerAt(5), 1234, "从站寄存器已被写入");
}

void testSingleInFlight() {
    section("同一时刻只允许一个在途事务");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();

    DeviceSession session(makeConfig("meter-08", TransportKind::Serial, 1));
    session.attach(*slave);

    session.tick(0);
    check(session.requestReadHoldingRegisters(0, 2), "第一个请求被接受");
    check(!session.requestReadHoldingRegisters(2, 2), "在途期间的第二个请求被拒绝");
    checkU64(session.stats().requests, 1, "只记录一个逻辑请求");
}

void testAutoPoll() {
    section("自动轮询：按间隔发起，不重叠");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();

    DeviceConfig config = makeConfig("meter-09", TransportKind::Serial, 1);
    config.points = {{0x03, 0, 2}};
    DeviceSession session(config);
    session.attach(*slave);
    session.setAutoPoll(true);

    session.tick(0);
    checkU64(session.stats().requests, 1, "第一轮发起请求");
    slave->deliverAll();

    session.tick(10);
    checkU64(session.stats().requests, 1, "未到轮询间隔不重复发起");
    session.tick(50);
    checkU64(session.stats().requests, 2, "到达轮询间隔后再发起");
}

// 回归测试：轮询点写单寄存器（0x06）写入的必须是 PollPoint::value，而不是 count。
// 早期实现把 count 当写入值传下去，配置里「数量」会被误写进设备，
// 这里故意让 count 与 value 取不同的数，确保二者不会被混淆。
void testAutoPollWritePoint() {
    section("自动轮询：0x06 写入的是 value 而不是 count");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();

    DeviceConfig config = makeConfig("meter-10", TransportKind::Serial, 1);
    config.points = {{0x06, 5, 2, 1234}};  // 地址 5，count=2，value=1234
    DeviceSession session(config);
    session.attach(*slave);
    session.setAutoPoll(true);

    Collector collector;
    session.setResultCallback([&collector](const TransactionResult& r) { collector(r); });

    session.tick(0);
    slave->deliverAll();

    check(collector.last.valid, "写结果有效");
    checkU16(collector.last.value, 1234, "回显写入值为 value");
    checkU16(slave->registerAt(5), 1234, "从站寄存器被写入 value");
    check(slave->registerAt(5) != 2, "从站寄存器没有被误写成 count");
}

// 回归测试：未支持的轮询功能码不能打断轮询序列。
void testAutoPollUnknownFunction() {
    section("自动轮询：未支持的功能码被跳过，不阻塞其它点位");
    auto slave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Serial);
    slave->open();

    DeviceConfig config = makeConfig("meter-11", TransportKind::Serial, 1);
    // 0x45 不在支持列表内：本轮应被跳过，而不是发出一个注定失败的请求
    config.points = {{0x45, 0, 2}, {0x03, 0, 2}};
    DeviceSession session(config);
    session.attach(*slave);
    session.setAutoPoll(true);

    session.tick(0);
    checkU64(session.stats().requests, 0, "未知功能码不产生请求");
    slave->deliverAll();

    session.tick(50);
    checkU64(session.stats().requests, 1, "下一轮轮到 0x03，正常发起请求");
    slave->deliverAll();
    checkU64(session.stats().responses, 1, "0x03 请求正常拿到响应");
}

void testDeviceManager() {
    section("DeviceManager：多设备注册、启动与结果分发");
    DeviceManager manager;

    auto tcpSlave = std::make_unique<test_support::FakeSlaveTransport>(1, TransportKind::Tcp);
    auto rtuSlave = std::make_unique<test_support::FakeSlaveTransport>(2, TransportKind::Serial);
    test_support::FakeSlaveTransport* tcpRaw = tcpSlave.get();
    test_support::FakeSlaveTransport* rtuRaw = rtuSlave.get();

    std::vector<std::string> names;
    int resultCount = 0;
    manager.setResultCallback(
        [&names, &resultCount](const std::string& name, const TransactionResult&) {
            names.push_back(name);
            ++resultCount;
        });

    DeviceConfig tcpConfig = makeConfig("plc-01", TransportKind::Tcp, 1);
    tcpConfig.points = {{0x03, 0, 2}};
    DeviceConfig rtuConfig = makeConfig("meter-02", TransportKind::Serial, 2);
    rtuConfig.points = {{0x04, 0, 2}};

    check(manager.addDevice(tcpConfig, std::move(tcpSlave)) != nullptr, "TCP 设备注册成功");
    check(manager.addDevice(rtuConfig, std::move(rtuSlave)) != nullptr, "RTU 设备注册成功");
    checkU64(manager.deviceCount(), 2, "设备数为 2");

    check(manager.startAll(), "两条链路都成功打开");
    check(manager.state("plc-01") == TransportState::Open, "TCP 链路状态为 open");

    manager.tick(0);
    checkU64(tcpRaw->sentFrames, 1, "TCP 设备发起一次请求");
    checkU64(rtuRaw->sentFrames, 1, "RTU 设备发起一次请求");

    tcpRaw->deliverAll();
    check(resultCount == 1, "TCP 结果已回调");
    check(!names.empty() && names.back() == "plc-01", "回调带正确的设备名");

    rtuRaw->deliverAll();
    check(resultCount == 2, "RTU 结果已回调");
    check(names.size() == 2 && names.back() == "meter-02", "第二个回调来自 RTU 设备");

    manager.stopAll();
    check(manager.state("plc-01") == TransportState::Closed, "停止后链路状态为 closed");
    check(manager.session("nope") == nullptr, "查询不存在的设备返回空指针");
}

void testByteAccumulator() {
    section("ByteAccumulator：追加、消费与越界清理");
    ByteAccumulator accumulator;
    accumulator.append(gateway::protocol::fromHex("01 02 03"));
    checkU64(accumulator.size(), 3, "追加 3 字节");
    accumulator.consume(1);
    checkU64(accumulator.size(), 2, "消费 1 字节后剩 2 字节");
    check(accumulator.data()[0] == 0x02, "读指针已前移");

    accumulator.append(gateway::protocol::fromHex("04 05"));
    checkU64(accumulator.size(), 4, "再次追加后共 4 字节");
    check(accumulator.data()[0] == 0x02, "追加不会打乱未消费的数据");

    accumulator.consume(100);
    check(accumulator.empty(), "消费超过长度时直接清空");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // 无缓冲：定位卡死点，CI 里也更容易看进度
    std::printf("Modbus 设备层测试（假链路驱动，不依赖 Qt）\n");

    testRtuRead();
    testTimeoutAndRetry();
    testBadCrc();
    testFragmentArrival();
    testStickyFrames();
    testLateResponseTcp();
    testExceptionResponse();
    testWriteSingleRegister();
    testSingleInFlight();
    testAutoPoll();
    testAutoPollWritePoint();
    testAutoPollUnknownFunction();
    testDeviceManager();
    testByteAccumulator();

    return test_support::summary();
}
