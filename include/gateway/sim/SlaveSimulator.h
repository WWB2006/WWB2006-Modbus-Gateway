#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gateway/sim/RequestHandler.h"
#include "gateway/transport/ByteAccumulator.h"

namespace gateway::sim {

// ---------------------------------------------------------------------------
// SlaveSimulator：Modbus TCP 从站模拟器。
//
// 为什么单独做成一个可执行文件（而不是塞进网关进程里）：
//   1. 可以独立启停，测试里反复起一个「坏设备」不用重启网关；
//   2. 与网关分属两个进程，走真实的 socket，TCP 粘包/半包/断连都能真实发生，
//      比进程内的假链路更能暴露问题；
//   3. 阶段 8 的异常注入引擎可以直接挂在这个进程上按需「变坏」。
//
// 这个类刻意**不使用 Qt**，只用 POSIX/WinSock 套接字，因此：
//   - 在没有装 Qt 的机器上也能编译运行（与 README「方式一」一致）；
//   - 阶段 11 的无界面网关可以把它当组件直接跑在服务器上。
//
// 线程模型：单线程 accept + 轮询所有连接（select），不做线程池。
// 模拟器要服务的对象是测试脚本，连接数是个位数，简单模型比并发模型更容易验证正确性。
// 这一点与项目一（MyselfWebServer 的主从 Reactor）刚好相反，是有意的：
// 那个项目的目标是「证明能做高并发」，这个项目的目标是「证明协议实现是对的」。
// ---------------------------------------------------------------------------

class SlaveSimulator {
public:
    struct Config {
        std::string bindAddress = "127.0.0.1";
        uint16_t port = 5020;          // 502 需要管理员权限，模拟器默认用 5020
        uint8_t unitId = 1;
        bool supportBroadcast = false;
        bool verbose = false;           // 打印每条请求与响应
    };

    struct Stats {
        uint64_t acceptedConnections = 0;
        uint64_t closedConnections = 0;
        uint64_t requestsHandled = 0;
        uint64_t exceptionResponses = 0;
        uint64_t ignoredRequests = 0;   // 单元号不匹配 / 广播，未应答
        uint64_t malformedFrames = 0;   // 协议标识或长度字段非法
    };

    SlaveSimulator(RegisterMap& map, Config config);
    ~SlaveSimulator();

    SlaveSimulator(const SlaveSimulator&) = delete;
    SlaveSimulator& operator=(const SlaveSimulator&) = delete;

    // 打开监听端口。失败返回 false，error() 给出原因。
    bool start();
    void stop();
    bool running() const noexcept { return listenFd_ != kInvalidFd; }

    // 跑一轮事件循环：接受新连接、读数据、处理请求、发送响应。
    // 返回本轮处理的字节数，供调用方判断「是否空闲」，也便于测试里手动驱动。
    std::size_t pollOnce(int timeoutMs = 100);

    // 跑到收到退出信号或 stop() 被调用为止。
    void run();

    void requestStop() noexcept { stopRequested_ = true; }

    const Stats& stats() const noexcept { return stats_; }
    const std::string& error() const noexcept { return error_; }
    const Config& config() const noexcept { return config_; }

    // 已连接客户端数（测试里用来确认 accept 是否成功）。
    std::size_t clientCount() const noexcept { return clients_.size(); }

private:
    struct Client {
        int fd = -1;
        transport::ByteAccumulator buffer;
    };

    void acceptNewClients();
    void closeClient(std::size_t index);
    void handleClientBytes(Client& client);
    // 处理一条已解出的请求帧并发回响应。
    bool respondTo(const protocol::Frame& frame, std::vector<uint8_t>& out);
    bool sendAll(int fd, const uint8_t* data, std::size_t len);
    void log(const std::string& line) const;

#if defined(_WIN32)
    using SocketHandle = uintptr_t;
#else
    using SocketHandle = int;
#endif
    static constexpr int kInvalidFd = -1;

    RegisterMap* map_ = nullptr;
    Config config_;
    RequestHandler handler_;
    std::vector<Client> clients_;
    Stats stats_;
    std::string error_;
    int listenFd_ = kInvalidFd;
    bool stopRequested_ = false;
    bool winsockReady_ = false;
};

}  // namespace gateway::sim
