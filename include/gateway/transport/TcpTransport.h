#pragma once

#include <QAbstractSocket>
#include <QByteArray>
#include <QObject>
#include <string>

#include "gateway/transport/TransportInterface.h"

class QTcpSocket;
class QTimer;

namespace gateway::transport {

// 基于 QTcpSocket 的 TCP 链路。
//
// [与项目一对应] MyselfWebServer 用 epoll(ET) + 非阻塞 IO + 自定义 Buffer 收发字节；
// 这里换成 Qt 的事件循环 + QTcpSocket 的信号槽。两者解决的是同一个问题
// （把字节流可靠地搬进应用层），区别只在事件来源：一个是 epoll_wait，一个是 Qt 事件分发。
// 注意 QTcpSocket 已经把「按需读、可写才写」封装好了，所以这一层不再需要手动注册 EPOLLIN/EPOLLOUT。
//
// 本类只负责收发字节，不做任何 Modbus 语义；切帧由 device 层的解码器完成。
class TcpTransport : public QObject, public TransportInterface {
    Q_OBJECT

public:
    explicit TcpTransport(const TransportConfig& config, QObject* parent = nullptr);
    ~TcpTransport() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;
    bool send(const uint8_t* data, std::size_t len) override;
    TransportKind kind() const override { return TransportKind::Tcp; }
    std::string describe() const override;

private slots:
    void handleConnected();
    void handleDisconnected();
    void handleReadyRead();
    void handleBytesWritten(qint64 bytes);
    void handleError(QAbstractSocket::SocketError error);
    void handleConnectTimeout();

private:
    // 把 writeQueue_ 里的积压字节尽可能续发出去；写不动就返回，等下一次 bytesWritten。
    void flushWriteQueue();

    TransportConfig config_;
    QTcpSocket* socket_ = nullptr;
    QTimer* connectTimer_ = nullptr;
    // 发送缓冲被写满时暂存的剩余字节。空表示当前没有积压。
    QByteArray writeQueue_;
};

}  // namespace gateway::transport
