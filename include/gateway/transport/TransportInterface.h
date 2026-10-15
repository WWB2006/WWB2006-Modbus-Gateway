#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace gateway::transport {

enum class TransportKind {
    Tcp,
    Serial,
};

enum class TransportState {
    Closed,
    Connecting,
    Open,
    Error,
};

const char* toString(TransportKind kind) noexcept;
const char* toString(TransportState state) noexcept;

struct TransportConfig {
    TransportKind kind = TransportKind::Tcp;

    // TCP
    std::string host = "127.0.0.1";
    uint16_t port = 502;
    int connectTimeoutMs = 3000;

    // 串口（阶段 4 使用）
    std::string serialPort = "COM11";
    int baudRate = 9600;
    char parity = 'N';
    int dataBits = 8;
    int stopBits = 1;
};

// 链路抽象：只负责「打开、关闭、收发字节」，不做任何 Modbus 语义。
//
// [与项目一对应] 项目一里 TcpConnection + Channel 封装了「一条连接」的读写与事件；
// 这里用 TransportInterface 承担同样的角色，但把「必须是网络连接」放宽成「任何能收发字节的链路」，
// 于是 TCP、串口，以及测试用的假链路都能塞进同一套设备层代码。
//
// 刻意不依赖 Qt，因此可以用假链路在没有网络与硬件的情况下驱动整个设备层做测试。
//
// 回调设计：每个回调槽**只允许一个订阅者**（后注册的覆盖先注册的）。
// 早期版本里 DeviceSession::attach() 与 DeviceManager::addDevice() 都注册了 onState，
// 后者把前者覆盖掉，导致「掉线触发重连」的处理器被静默替换、重连失效。
// 现在明确约定：**状态由会话统一持有并转发**，DeviceManager 通过 session 读状态，
// 不再自己注册 onState，避免同一份状态有两个"所有者"。
class TransportInterface {
public:
    using BytesCallback = std::function<void(const uint8_t*, std::size_t)>;
    using StateCallback = std::function<void(TransportState)>;
    using MessageCallback = std::function<void(const std::string&)>;

    virtual ~TransportInterface() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool send(const uint8_t* data, std::size_t len) = 0;
    virtual TransportKind kind() const = 0;
    virtual std::string describe() const = 0;

    // 当前链路状态。由 notifyState 维护，供上层随时查询。
    TransportState state() const noexcept { return state_; }

    // -----------------------------------------------------------------------
    // 主动驱动一次「收数据」（阶段 11）。
    //
    // 为什么需要它：字节送到 notifyBytes 有两条路——
    //   - Qt 链路（TcpTransport/SerialTransport）：Qt 事件循环自动投递，不需要外部驱动；
    //   - 原生链路（NativeTcpTransport）：没有事件循环，必须由主循环轮询 socket。
    // 所以基类给一个**空实现**：Qt 链路继承默认行为，原生链路覆写它。
    //
    // 不用「在 isOpen() 里顺手读一把」这种隐式做法：那会让一个查询函数产生副作用，
    // 调用方无法控制读的时机，也让「为什么这里会收到数据」变得不可追踪。
    // -----------------------------------------------------------------------
    virtual void pump() {}

    void onBytes(BytesCallback callback) { bytesCallback_ = std::move(callback); }
    void onState(StateCallback callback) { stateCallback_ = std::move(callback); }
    void onMessage(MessageCallback callback) { messageCallback_ = std::move(callback); }

protected:
    void notifyBytes(const uint8_t* data, std::size_t len) {
        if (bytesCallback_) {
            bytesCallback_(data, len);
        }
    }
    void notifyState(TransportState state) {
        state_ = state;
        if (stateCallback_) {
            stateCallback_(state);
        }
    }
    void notifyMessage(const std::string& text) {
        if (messageCallback_) {
            messageCallback_(text);
        }
    }

private:
    BytesCallback bytesCallback_;
    StateCallback stateCallback_;
    MessageCallback messageCallback_;
    TransportState state_ = TransportState::Closed;
};

}  // namespace gateway::transport
