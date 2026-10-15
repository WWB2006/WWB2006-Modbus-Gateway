// 阶段 9 健壮性测试：断线自动重连（指数退避）、主动 stop 不触发重连、
// 掉线时在途事务被中止、重连后自动恢复轮询。
//
// 用一个"可开关"的假链路：open()/close() 的行为由测试控制，
// 可以模拟"设备掉电 → 恢复"这一现场最常见的场景。

#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "gateway/device/DeviceManager.h"
#include "gateway/device/DeviceSession.h"
#include "gateway/protocol/ModbusCodec.h"
#include "gateway/transport/TransportInterface.h"
#include "test_support.h"

using test_support::check;
using test_support::checkU16;
using test_support::checkU64;
using test_support::section;

namespace {

using gateway::device::DeviceConfig;
using gateway::device::DeviceManager;
using gateway::device::DeviceSession;
using gateway::device::TransactionResult;
using gateway::transport::TransportInterface;
using gateway::transport::TransportKind;
using gateway::transport::TransportState;

// 可控假链路：
//   - online 为 false 时 open() 失败（模拟设备掉电 / 网线拔出）；
//   - dropLink() 模拟"正在用着链路突然断"，触发会话的掉线处理；
//   - 支持正常响应一个读保持寄存器请求。
class ControllableTransport : public TransportInterface {
public:
    ControllableTransport(uint8_t unitId, TransportKind kind) : unitId_(unitId), kind_(kind) {}

    bool open() override {
        ++openCalls;
        if (!online) {
            notifyState(TransportState::Error);
            return false;
        }
        open_ = true;
        notifyState(TransportState::Open);
        return true;
    }

    void close() override {
        open_ = false;
        notifyState(TransportState::Closed);
    }

    bool isOpen() const override { return open_; }
    TransportKind kind() const override { return kind_; }
    std::string describe() const override { return "controllable"; }

    bool send(const uint8_t* data, std::size_t len) override {
        if (!open_) {
            return false;
        }
        ++sentFrames;
        if (!respond_) {
            return true;  // 收到但不回，主站会超时
        }
        gateway::protocol::DecodeResult decoded;
        std::size_t consumed = 0;
        if (kind_ == TransportKind::Tcp) {
            consumed = gateway::protocol::decodeTcp(data, len, decoded);
        } else {
            consumed = gateway::protocol::decodeRtuRequest(data, len, decoded);
        }
        if (consumed == 0) {
            return true;
        }
        const auto& frame = decoded.frame;
        if (frame.functionCode != 0x03 && frame.functionCode != 0x04) {
            return true;
        }
        const uint16_t count = static_cast<uint16_t>((frame.pdu[2] << 8) | frame.pdu[3]);
        std::vector<uint8_t> pdu;
        pdu.push_back(frame.functionCode);
        pdu.push_back(static_cast<uint8_t>(count * 2));
        for (uint16_t i = 0; i < count; ++i) {
            pdu.push_back(0x00);
            pdu.push_back(static_cast<uint8_t>(i + 1));
        }
        const std::vector<uint8_t> response =
            kind_ == TransportKind::Tcp
                ? gateway::protocol::encodeTcpRequest(frame.transactionId, frame.unitId, pdu)
                : gateway::protocol::encodeRtuRequest(frame.unitId, pdu);
        notifyBytes(response.data(), response.size());
        return true;
    }

    // 模拟链路在使用中突然断开（设备掉电 / 网线拔出）。
    void dropLink() {
        open_ = false;
        notifyState(TransportState::Error);
    }

    // ---- 脚本开关 ----
    bool online = true;       // 设备是否在线（决定 open() 成败）
    bool respond_ = true;     // 是否应答请求
    int openCalls = 0;        // open() 被调用次数
    int sentFrames = 0;

private:
    uint8_t unitId_ = 1;
    TransportKind kind_;
    bool open_ = false;
};

DeviceConfig makeConfig(uint8_t unitId) {
    DeviceConfig config;
    config.name = "plc-recover";
    config.transport = TransportKind::Tcp;
    config.unitId = unitId;
    config.timeoutMs = 100;
    config.retry = 0;
    config.pollIntervalMs = 50;
    // 退避策略：初始 100ms，倍率 2，上限 800ms，前 1 次失败立即重试。
    config.reconnect.enabled = true;
    config.reconnect.initialDelayMs = 100;
    config.reconnect.maxDelayMs = 800;
    config.reconnect.multiplier = 2;
    config.reconnect.failuresBeforeBackoff = 1;
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

struct LinkLog {
    std::vector<DeviceSession::LinkEvent> events;
    std::vector<std::string> details;
    void operator()(DeviceSession::LinkEvent event, const std::string& detail) {
        events.push_back(event);
        details.push_back(detail);
    }
};

void testDropSchedulesReconnect() {
    section("健壮性：掉线后进入重连倒计时");

    auto transport = std::make_unique<ControllableTransport>(1, TransportKind::Tcp);
    ControllableTransport* raw = transport.get();
    raw->open();

    DeviceSession session(makeConfig(1));
    session.attach(*raw);
    LinkLog linkLog;
    session.setLinkCallback(std::ref(linkLog));

    session.tick(0);
    check(!session.waitingReconnect(), "初始不在重连状态");

    // 链路突然断开。
    raw->dropLink();
    check(session.waitingReconnect(), "掉线后进入重连倒计时");
    checkU64(session.stats().reconnectAttempts, 0, "尚未尝试重连");
    checkU64(linkLog.events.size(), 1, "上报一次掉线事件");
    check(linkLog.events[0] == DeviceSession::LinkEvent::Down, "事件类型为 Down");
}

void testReconnectRecovers() {
    section("健壮性：设备恢复后自动重连成功");

    auto transport = std::make_unique<ControllableTransport>(1, TransportKind::Tcp);
    ControllableTransport* raw = transport.get();
    raw->open();

    DeviceSession session(makeConfig(1));
    session.attach(*raw);
    Collector collector;
    session.setResultCallback(std::ref(collector));
    LinkLog linkLog;
    session.setLinkCallback(std::ref(linkLog));

    session.tick(0);
    raw->dropLink();

    // 倒计时未到：不尝试重连。
    session.tick(50);
    checkU64(raw->openCalls, 1, "倒计时未到不重连");

    // 设备恢复。
    raw->online = true;
    session.tick(100);  // 到达初始延迟 100ms
    checkU64(raw->openCalls, 2, "到达延迟后尝试重连");
    check(!session.waitingReconnect(), "重连成功后退出重连状态");
    checkU64(session.stats().reconnects, 1, "重连成功计数 1");
    check(linkLog.events.size() >= 2 && linkLog.events.back() == DeviceSession::LinkEvent::Recovered,
          "上报恢复事件");

    // 重连后能正常发请求并拿到数据。
    check(session.requestReadHoldingRegisters(0, 3), "重连后可发起请求");
    checkU64(collector.count, 1, "收到回调");
    check(collector.last.valid, "数据有效");
    checkU16(test_support::valueAt(collector.last.values, 2), 3, "第三个寄存器");
}

void testBackoffGrowth() {
    section("健壮性：连续失败时指数退避");

    auto transport = std::make_unique<ControllableTransport>(1, TransportKind::Tcp);
    ControllableTransport* raw = transport.get();
    raw->open();

    DeviceSession session(makeConfig(1));
    session.attach(*raw);
    LinkLog linkLog;
    session.setLinkCallback(std::ref(linkLog));

    session.tick(0);
    raw->dropLink();

    // 设备一直离线，观察每次重连的时间间隔是否按 100 → 200 → 400 → 800 增长。
    // 初始延迟 100ms，失败 1 次后进入退避，倍率 2，上限 800。
    raw->online = false;

    uint64_t now = 0;
    std::vector<uint64_t> attemptTimes;
    for (int i = 0; i < 6; ++i) {
        // 每次推进到下一次重连时刻。
        now = session.nextReconnectAtMs();
        session.tick(now);
        attemptTimes.push_back(now);
    }

    // 尝试次数应为 6（每次 tick 到点就试一次）。
    checkU64(session.stats().reconnectAttempts, 6, "共尝试 6 次重连");
    checkU64(session.stats().reconnects, 0, "一直失败，无成功重连");

    // 间隔：第 2 次尝试距第 1 次 100ms；第 3 次距第 2 次 200ms；之后封顶 800ms。
    const uint64_t gap1 = attemptTimes[1] - attemptTimes[0];
    const uint64_t gap2 = attemptTimes[2] - attemptTimes[1];
    const uint64_t gap3 = attemptTimes[3] - attemptTimes[2];
    checkU64(gap1, 100, "第 1 次重试间隔 100ms");
    checkU64(gap2, 200, "第 2 次重试间隔 200ms（退避翻倍）");
    checkU64(gap3, 400, "第 3 次重试间隔 400ms");
    // 再往后封顶在 800。
    const uint64_t gap5 = attemptTimes[5] - attemptTimes[4];
    checkU64(gap5, 800, "退避封顶在 800ms");
}

void testStopDoesNotReconnect() {
    section("健壮性：主动 stop 不触发重连");

    auto transport = std::make_unique<ControllableTransport>(1, TransportKind::Tcp);
    ControllableTransport* raw = transport.get();
    raw->open();

    DeviceManager manager;
    DeviceConfig config = makeConfig(1);
    DeviceSession* session = manager.addDevice(config, std::move(transport));
    check(session != nullptr, "设备已注册");

    // 主动停止：先 stop 再 close，不应被当成掉线。
    manager.stopAll();
    check(!session->waitingReconnect(), "主动停止后不进入重连状态");
    checkU64(session->stats().reconnectAttempts, 0, "未尝试重连");

    manager.tick(1000);
    checkU64(session->stats().reconnectAttempts, 0, "tick 后依然不重连");
}

void testInFlightAbortedOnDrop() {
    section("健壮性：掉线时在途事务被中止");

    auto transport = std::make_unique<ControllableTransport>(1, TransportKind::Tcp);
    ControllableTransport* raw = transport.get();
    raw->open();
    raw->respond_ = false;  // 设备不回应，制造一个在途事务

    DeviceSession session(makeConfig(1));
    session.attach(*raw);
    Collector collector;
    session.setResultCallback(std::ref(collector));

    session.tick(0);
    check(session.requestReadHoldingRegisters(0, 2), "请求已发出");
    check(session.busy(), "事务在途");

    // 掉线：在途事务应立即失败，而不是继续等超时。
    raw->dropLink();
    check(!session.busy(), "掉线后事务被中止");
    checkU64(collector.count, 1, "上报一次中止结果");
    check(!collector.last.valid, "中止结果无效");
    check(!collector.last.timedOut, "不是超时（是被链路断开中止）");
    checkU64(session.bufferedBytes(), 0, "接收缓冲被清空");

    // 重连倒计时期间不应再发请求（避免刷爆失败日志）。
    session.tick(10);
    check(session.waitingReconnect(), "仍在重连倒计时");
}

void testReconnectDisabled() {
    section("健壮性：关闭重连策略后掉线即终止");

    auto transport = std::make_unique<ControllableTransport>(1, TransportKind::Tcp);
    ControllableTransport* raw = transport.get();
    raw->open();

    DeviceConfig config = makeConfig(1);
    config.reconnect.enabled = false;

    DeviceSession session(config);
    session.attach(*raw);
    LinkLog linkLog;
    session.setLinkCallback(std::ref(linkLog));

    session.tick(0);
    raw->dropLink();
    check(!session.waitingReconnect(), "策略关闭时不进入重连状态");
    checkU64(linkLog.events.size(), 0, "不重连也就不上报事件");
}

void testManagerForwardsLinkEvents() {
    section("健壮性：DeviceManager 转发链路上下线事件");

    auto transport = std::make_unique<ControllableTransport>(1, TransportKind::Tcp);
    ControllableTransport* raw = transport.get();
    raw->open();

    DeviceManager manager;
    std::vector<std::string> names;
    std::vector<DeviceSession::LinkEvent> events;
    manager.setLinkCallback([&names, &events](const std::string& name,
                                              DeviceSession::LinkEvent event,
                                              const std::string& /*detail*/) {
        names.push_back(name);
        events.push_back(event);
    });

    DeviceSession* session = manager.addDevice(makeConfig(1), std::move(transport));
    check(session != nullptr, "设备已注册");

    // 会话持有的链路状态应能被 manager 正确读出（唯一所有者原则）。
    check(manager.state("plc-recover") == TransportState::Open, "manager 读到 Open 状态");

    raw->dropLink();
    check(manager.state("plc-recover") == TransportState::Error, "掉线后读到 Error 状态");
    checkU64(events.size(), 1, "转发了一次事件");
    check(events[0] == DeviceSession::LinkEvent::Down, "事件类型为 Down");
    check(!names.empty() && names[0] == "plc-recover", "事件带正确的设备名");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Modbus 健壮性测试（断线重连与指数退避，不依赖 Qt）\n");

    testDropSchedulesReconnect();
    testReconnectRecovers();
    testBackoffGrowth();
    testStopDoesNotReconnect();
    testInFlightAbortedOnDrop();
    testReconnectDisabled();
    testManagerForwardsLinkEvents();

    return test_support::summary();
}
