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
};

}  // namespace gateway::transport
