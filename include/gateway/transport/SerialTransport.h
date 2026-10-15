#pragma once

#include <QByteArray>
#include <QObject>
#include <memory>
#include <string>

#include "gateway/transport/TransportInterface.h"

class QSerialPort;
class QTimer;

namespace gateway::transport {

// 基于 QSerialPort 的串口链路（Modbus RTU，通常接 RS-485 转换器）。
//
// 与 TcpTransport 是同一抽象（TransportInterface）的两个实现：
//   - 上层设备层（DeviceSession）完全不知道自己在走 TCP 还是串口；
//   - 两者只在「怎么把字节搬进来、怎么判帧边界」上不同。
//
// [与项目一对应] 项目一里 epoll 负责「可读就绪」通知，这里换成 QSerialPort 的
// readyRead 信号；项目一用非阻塞 fd + ET 直到 EAGAIN，这里由 Qt 的串口后端负责。
// 共同点是：**上层拿到的永远只是字节流，分帧是上层的责任**。
//
// 串口与 TCP 的实质差别在分帧：TCP 有 MBAP 长度字段，串口没有，
// 只能靠「静默间隔 ≥ 3.5 字符时间」判断一帧结束（见 readTimeoutMs 与
// DeviceSession 里的 RTU 解帧路径）。因此串口链路必须把「帧内间隔」这件事
// 交给设备层，本类只负责按到达顺序把字节喂进去。
//
// ⚠️ 头文件里只前置声明 QSerialPort，避免任何依赖 Qt SerialPort 模块的代码
// 泄漏到不含该模块的编译单元（CMake 只有检测到 Qt6::SerialPort 时才会编本文件）。
class SerialTransport : public QObject, public TransportInterface {
    Q_OBJECT

public:
    explicit SerialTransport(const TransportConfig& config, QObject* parent = nullptr);
    ~SerialTransport() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;
    bool send(const uint8_t* data, std::size_t len) override;
    TransportKind kind() const override { return TransportKind::Serial; }
    std::string describe() const override;

    // 帧间静默间隔（毫秒）。Modbus over Serial Line 规定 ≥3.5 字符时间；
    // 9600bps 下约 4ms，115200bps 下约 0.3ms。取整并加一点裕量，
    // 让设备层在这个间隔后把缓冲区的字节当成完整帧去解。
    int frameGapMs() const noexcept { return frameGapMs_; }

private slots:
    void handleReadyRead();
    void handleError();
    void handleFrameGapTimeout();

private:
    TransportConfig config_;
    std::unique_ptr<QSerialPort> port_;
    // 帧间静默定时器：每次收到字节就重启，超时即认为一帧结束。
    // 它只用来自动触发一次"帧结束"通知，真正的切帧由设备层完成。
    QTimer* gapTimer_ = nullptr;
    QByteArray pendingBytes_;
    int frameGapMs_ = 5;
};

}  // namespace gateway::transport
