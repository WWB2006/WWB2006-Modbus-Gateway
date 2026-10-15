#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gateway/transport/TransportInterface.h"

namespace gateway::transport {

// ---------------------------------------------------------------------------
// NativeTcpTransport：不依赖 Qt 的 TCP 链路。
//
// 为什么需要第二个 TCP 实现（已经有 TcpTransport 了）：
//   1. gateway_daemon 的目标是「在没有 Qt 的服务器上跑」，
//      而 TcpTransport 继承 QObject、用 QTcpSocket 与 Qt 事件循环 ——
//      链上它就等于链上整个 Qt Core，部署体积与依赖面都回来了；
//   2. 阻塞式 socket 的语义在这里反而更合适：daemon 是单线程轮询模型，
//      每次 send 之后由 tick() 驱动读，不需要 Qt 那种"事件来了再叫我"的异步模型。
//
// 与 TcpTransport 的行为差异（刻意保留，不是缺陷）：
//   - open() 是**阻塞**的（带 connectTimeoutMs 上限），而 TcpTransport 的 open()
//     只是"开始连接"、真正连上要等信号；
//   - 读写都在调用 send/close 与内部 pollOnce() 时同步完成，没有后台事件循环。
//
// 缓冲区策略：send 直接 write 全部字节（阻塞 socket 下 write 会自己等）；
// 收到的字节进 readBuffer_，由 pollOnce() 一次性 notifyBytes 给上层。
// 这样「一帧被拆成两次 recv」的情况仍然会被上层的 ByteAccumulator 正确拼回。
// ---------------------------------------------------------------------------
class NativeTcpTransport : public TransportInterface {
public:
    explicit NativeTcpTransport(const TransportConfig& config);
    ~NativeTcpTransport() override;

    NativeTcpTransport(const NativeTcpTransport&) = delete;
    NativeTcpTransport& operator=(const NativeTcpTransport&) = delete;

    bool open() override;
    void close() override;
    bool isOpen() const override;
    bool send(const uint8_t* data, std::size_t len) override;
    TransportKind kind() const override { return TransportKind::Tcp; }
    std::string describe() const override;

    // 非阻塞地读一次 socket：有数据就 notifyBytes 出去。
    // 返回读到的字节数（0 表示暂时没数据；-1 表示连接已断）。
    // 由 gateway_daemon 的主循环每轮调用 —— 这与 DeviceSession::tick 的
    // "外部驱动时钟"模型是同一套思路：链路也不自己起线程。
    int pollOnce();

    // TransportInterface::pump 的覆写：本链路没有事件循环，
    // 因此「收数据」必须由外部主动驱动，pump 就是那个入口。
    void pump() override;

    // socket 是否仍然连接（对端 close 后第一次 pollOnce 会察觉）。
    bool connected() const noexcept { return fd_ >= 0; }

private:
    TransportConfig config_;
    int fd_ = -1;
    std::vector<uint8_t> readBuffer_;
    bool winsockReady_ = false;
};

}  // namespace gateway::transport
