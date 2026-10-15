// 阶段 11 链路层测试：原生 TCP 链路（不依赖 Qt）+ 链路注册表。
//
// 为什么这组用例重要：
//   1. NativeTcpTransport 是 gateway_daemon 在没有 Qt 的服务器上唯一能用的链路。
//      它之前只有「能编译」的保证，没有任何「真的能收发字节」的证据；
//   2. 它跟 TcpTransport 是两套完全不同的 socket 代码（阻塞 connect + select 轮询
//      vs 非阻塞 + 信号槽），Qt 那套的测试完全覆盖不到它；
//   3. 链路注册表把「链路从哪来」解耦了，注册/注销/未注册三种状态都要有断言，
//      否则「链路莫名不可用」会以静默的方式回归。
//
// 测试策略：真起一个从站模拟器，走真实 loopback socket。
// 不 mock socket —— 这层被测的正是 socket 行为本身，mock 掉就等于什么都没测。

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "gateway/device/DeviceConfig.h"
#include "gateway/device/DeviceManager.h"
#include "gateway/protocol/ModbusCodec.h"
#include "gateway/sim/RegisterMap.h"
#include "gateway/sim/SlaveSimulator.h"
#include "gateway/transport/NativeTcpTransport.h"
#include "gateway/transport/TransportFactory.h"
#include "test_support.h"

using test_support::check;
using test_support::checkU16;
using test_support::checkU64;
using test_support::section;

namespace {

// 测试专用端口。避开 5020（仓库示例配置）与 502（需要管理员权限）。
constexpr uint16_t kSimPort = 15021;
// 这个端口上什么都不监听，用来验证「连接被拒绝」的错误路径。
// 选一个不常见的号段，降低与系统服务冲突的概率。
constexpr uint16_t kClosedPort = 15999;

gateway::transport::TransportConfig tcpConfig(uint16_t port) {
    gateway::transport::TransportConfig config;
    config.kind = gateway::transport::TransportKind::Tcp;
    config.host = "127.0.0.1";
    config.port = port;
    config.connectTimeoutMs = 1500;  // 测试里不需要等太久
    return config;
}

gateway::sim::RegisterMap makeMap() {
    gateway::sim::RegisterMap map;
    map.resizeHoldingRegisters(128);
    map.setHoldingRegister(0, 100);
    map.setHoldingRegister(1, 250);
    map.setHoldingRegister(2, 380);
    map.setHoldingRegister(3, 7);
    return map;
}

gateway::sim::SlaveSimulator::Config simConfig() {
    gateway::sim::SlaveSimulator::Config config;
    config.bindAddress = "127.0.0.1";
    config.port = kSimPort;
    config.unitId = 1;
    config.verbose = false;
    return config;
}

// 协作式推进：模拟器处理请求、客户端收响应，两边轮流跑。
// 用真实时钟做上限而不是死循环，避免失败时挂住 CI。
// 返回是否在超时前收够了期望字节数。
bool pumpUntil(gateway::sim::SlaveSimulator& sim,
               gateway::transport::NativeTcpTransport& transport,
               const std::vector<uint8_t>& received, std::size_t expected,
               int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (received.size() < expected && std::chrono::steady_clock::now() < deadline) {
        sim.pollOnce(1);
        transport.pollOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return received.size() >= expected;
}

// ---------------------------------------------------------------------------
// 1. 链路注册表：未注册 / 已注册 / 注销 三态
// ---------------------------------------------------------------------------
void testRegistry() {
    section("链路注册表：注册、查询、注销");

    // 起点：串口在无 Qt 构建里一定没注册（测试就是无 Qt 构建）。
    check(!gateway::transport::isRegisteredTransportAvailable(
              gateway::transport::TransportKind::Serial),
          "未注册的串口链路应报告不可用");

    std::string error;
    auto missing = gateway::transport::createRegisteredTransport(
        tcpConfig(kSimPort), error);
    check(missing == nullptr, "未注册 TCP 时 createRegisteredTransport 返回空");
    check(!error.empty(), "未注册时应给出可读的失败原因，实际为空");

    // 注册一个 TCP 构造器。
    auto builder = [](const gateway::transport::TransportConfig& config,
                      std::string& outError) -> std::unique_ptr<gateway::transport::TransportInterface> {
        outError.clear();
        return std::make_unique<gateway::transport::NativeTcpTransport>(config);
    };
    auto availability = [](gateway::transport::TransportKind kind) {
        return kind == gateway::transport::TransportKind::Tcp;
    };
    gateway::transport::registerTransportBuilder(gateway::transport::TransportKind::Tcp, builder,
                                                availability);

    check(gateway::transport::isRegisteredTransportAvailable(
              gateway::transport::TransportKind::Tcp),
          "注册后 TCP 链路应报告可用");

    error.clear();
    auto created = gateway::transport::createRegisteredTransport(tcpConfig(kSimPort), error);
    check(created != nullptr, "注册后应能造出链路，错误：" + error);
    if (created != nullptr) {
        check(created->kind() == gateway::transport::TransportKind::Tcp, "造出的链路类型是 TCP");
        check(!created->isOpen(), "刚造出的链路默认是关闭的");
        check(created->state() == gateway::transport::TransportState::Closed,
              "刚造出的链路状态是 Closed");
        // 基类的 pump() 是空实现，不能崩 —— 这是 Qt 链路继承的默认行为。
        created->pump();
        check(true, "默认 pump() 可安全调用（空实现）");
    }

    // 注销后应回到「不可用」。
    gateway::transport::unregisterTransportBuilder(gateway::transport::TransportKind::Tcp);
    check(!gateway::transport::isRegisteredTransportAvailable(
              gateway::transport::TransportKind::Tcp),
          "注销后 TCP 链路应报告不可用");
}

// ---------------------------------------------------------------------------
// 2. 原生 TCP 链路的错误路径（不需要对端）
// ---------------------------------------------------------------------------
void testErrorPaths() {
    section("原生 TCP 链路：连接失败与未连接时的行为");

    gateway::transport::NativeTcpTransport transport(tcpConfig(kClosedPort));

    std::string lastMessage;
    transport.onMessage([&lastMessage](const std::string& text) { lastMessage = text; });

    // 没人监听 → open() 必须失败，且不能阻塞到操作系统的默认超时。
    const auto started = std::chrono::steady_clock::now();
    const bool opened = transport.open();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    check(!opened, "连接一个没有监听的端口应失败");
    check(!transport.isOpen(), "连接失败后 isOpen 应为假");
    check(transport.state() == gateway::transport::TransportState::Error,
          "连接失败后状态应为 Error");
    check(!lastMessage.empty(), "连接失败应给出可读原因，实际为空");
    // 上限放宽到 10s：Windows 上「连接被拒绝」是立即返回的，
    // 但不同网络栈配置下可能略慢。这里只保证「没有卡到系统默认超时（约 2 分钟）」。
    check(elapsed < 10000, "连接失败应在 10 秒内返回（实际 " + std::to_string(elapsed) + "ms）");

    // 未连接时发数据必须安全失败，不能崩。
    const std::vector<uint8_t> bytes = {0x01, 0x03};
    check(!transport.send(bytes.data(), bytes.size()), "未连接时 send 应返回失败");
    check(transport.pollOnce() < 0, "未连接时 pollOnce 应返回负数表示无链路");

    // close() 在未连接时应是幂等的。
    transport.close();
    transport.close();
    check(!transport.isOpen(), "重复 close 后仍是关闭状态");
}

// ---------------------------------------------------------------------------
// 3. 原生 TCP 链路 ↔ 从站模拟器：真实 loopback 收发
// ---------------------------------------------------------------------------
void testRoundTripAgainstSimulator() {
    section("原生 TCP 链路：与从站模拟器真实收发（loopback）");

    auto map = makeMap();
    gateway::sim::SlaveSimulator simulator(map, simConfig());
    if (!simulator.start()) {
        check(false, "模拟器启动失败（端口 " + std::to_string(kSimPort) + "）：" +
                         simulator.error());
        return;
    }
    check(simulator.running(), "模拟器已监听 " + std::to_string(kSimPort));

    gateway::transport::NativeTcpTransport transport(tcpConfig(kSimPort));

    std::vector<uint8_t> received;
    transport.onBytes([&received](const uint8_t* data, std::size_t len) {
        received.insert(received.end(), data, data + len);
    });

    // 先让模拟器 accept 掉连接，再发请求 —— 顺序反了也能工作（backlog 会缓冲），
    // 但显式 accept 过一轮能让后续的 pollOnce 循环更确定。
    check(transport.open(), "连接模拟器成功");
    check(transport.isOpen(), "open 之后 isOpen 为真");
    check(transport.state() == gateway::transport::TransportState::Open, "open 之后状态为 Open");

    simulator.pollOnce(50);
    checkU64(simulator.clientCount(), 1, "模拟器已接受 1 个连接");

    // 造一条真实的 Modbus TCP 请求：读保持寄存器 HR[0..3]，期望 100/250/380/7。
    const auto pdu = gateway::protocol::buildReadHoldingRegisters(0, 4);
    const auto frame = gateway::protocol::encodeTcpRequest(1, 1, pdu);
    check(transport.send(frame.data(), frame.size()), "发送请求帧成功");

    // 响应帧长：MBAP 7 字节 + PDU(1 功能码 + 1 字节数 + 8 数据) = 17 字节。
    constexpr std::size_t kExpectedBytes = 17;
    const bool arrived = pumpUntil(simulator, transport, received, kExpectedBytes);
    check(arrived, "在超时前收到完整响应（收到 " + std::to_string(received.size()) + " 字节）");

    if (arrived) {
        gateway::protocol::DecodeResult decoded;
        const std::size_t consumed =
            gateway::protocol::decodeTcp(received.data(), received.size(), decoded);
        check(consumed == kExpectedBytes, "响应帧正好消费 17 字节（实际 " +
                                              std::to_string(consumed) + "）");

        const auto parsed =
            gateway::protocol::parseReadRegistersResponse(decoded.frame, 4);
        check(parsed.valid, "响应解析成功：" + parsed.error);
        checkU64(parsed.values.size(), 4, "读到 4 个寄存器");
        checkU16(test_support::valueAt(parsed.values, 0), 100, "HR[0]");
        checkU16(test_support::valueAt(parsed.values, 1), 250, "HR[1]");
        checkU16(test_support::valueAt(parsed.values, 2), 380, "HR[2]");
        checkU16(test_support::valueAt(parsed.values, 3), 7, "HR[3]");
    }

    // 再发一条写请求，验证链路可以连续使用（不是「只能跑一次」）。
    received.clear();
    const auto writePdu = gateway::protocol::buildWriteSingleRegister(10, 4321);
    const auto writeFrame = gateway::protocol::encodeTcpRequest(2, 1, writePdu);
    check(transport.send(writeFrame.data(), writeFrame.size()), "发送写请求成功");

    constexpr std::size_t kWriteResponseBytes = 12;  // MBAP 7 + 功能码 1 + 地址 2 + 值 2
    const bool writeArrived = pumpUntil(simulator, transport, received, kWriteResponseBytes);
    check(writeArrived, "收到写响应（收到 " + std::to_string(received.size()) + " 字节）");
    if (writeArrived) {
        gateway::protocol::DecodeResult decoded;
        gateway::protocol::decodeTcp(received.data(), received.size(), decoded);
        const auto parsed = gateway::protocol::parseWriteSingleResponse(decoded.frame);
        check(parsed.valid, "写响应解析成功：" + parsed.error);
        checkU16(parsed.address, 10, "写响应回显地址");
        checkU16(parsed.value, 4321, "写响应回显值");
        // 从站侧确实写进去了。
        checkU16(map.holdingRegister(10), 4321, "从站侧 HR[10] 已被写入");
    }

    // close() 后从站侧应看到连接断开。
    transport.close();
    check(!transport.isOpen(), "close 之后 isOpen 为假");
    check(transport.state() == gateway::transport::TransportState::Closed,
          "close 之后状态为 Closed");

    simulator.stop();
}

// ---------------------------------------------------------------------------
// 4. 会话 + 管理器的 pump 转发
// ---------------------------------------------------------------------------
void testManagerPump() {
    section("DeviceManager::pump 把驱动请求转达到链路");

    // 用一个记录调用次数的假链路，确认 pump 真的被转发到了链路层。
    // 真链路（NativeTcpTransport）的 pump 已经在上一节验证过行为，
    // 这里只需要证明「管理器的 pump 会走到链路上」这一条转发路径。
    class CountingTransport : public gateway::transport::TransportInterface {
    public:
        bool open() override {
            open_ = true;
            notifyState(gateway::transport::TransportState::Open);
            return true;
        }
        void close() override {
            open_ = false;
            notifyState(gateway::transport::TransportState::Closed);
        }
        bool isOpen() const override { return open_; }
        bool send(const uint8_t*, std::size_t) override { return open_; }
        gateway::transport::TransportKind kind() const override {
            return gateway::transport::TransportKind::Tcp;
        }
        std::string describe() const override { return "counting"; }
        void pump() override { ++pumpCalls; }

        int pumpCalls = 0;

    private:
        bool open_ = false;
    };

    gateway::device::DeviceConfig config;
    config.name = "counting-device";
    config.transport = gateway::transport::TransportKind::Tcp;

    gateway::device::DeviceManager manager;
    auto transport = std::make_unique<CountingTransport>();
    CountingTransport* raw = transport.get();
    manager.addDevice(config, std::move(transport));

    manager.startAll();
    manager.pump();
    manager.pump();
    checkU64(raw->pumpCalls, 2, "两次 pump 应转发到链路两次");

    manager.stopAll();
}

}  // namespace

int main() {
    std::printf("Modbus 原生 TCP 链路与注册表测试（不依赖 Qt）\n");

    testRegistry();
    testErrorPaths();
    testRoundTripAgainstSimulator();
    testManagerPump();

    return test_support::summary();
}
