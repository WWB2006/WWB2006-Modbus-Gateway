#include "gateway/transport/TcpTransport.h"

#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>

namespace gateway::transport {

TcpTransport::TcpTransport(const TransportConfig& config, QObject* parent)
    : QObject(parent), config_(config) {
    socket_ = new QTcpSocket(this);
    connectTimer_ = new QTimer(this);
    connectTimer_->setSingleShot(true);

    connect(socket_, &QTcpSocket::connected, this, &TcpTransport::handleConnected);
    connect(socket_, &QTcpSocket::disconnected, this, &TcpTransport::handleDisconnected);
    connect(socket_, &QTcpSocket::readyRead, this, &TcpTransport::handleReadyRead);
    connect(socket_, &QTcpSocket::bytesWritten, this, &TcpTransport::handleBytesWritten);
    connect(socket_, &QAbstractSocket::errorOccurred, this, &TcpTransport::handleError);
    connect(connectTimer_, &QTimer::timeout, this, &TcpTransport::handleConnectTimeout);
}

TcpTransport::~TcpTransport() {
    // 析构时不再回调外部：先断开信号，避免在销毁过程中触发设备层逻辑。
    if (socket_ != nullptr) {
        disconnect(socket_, nullptr, this, nullptr);
        socket_->abort();
    }
}

bool TcpTransport::open() {
    if (isOpen()) {
        return true;
    }
    notifyState(TransportState::Connecting);
    socket_->connectToHost(QString::fromStdString(config_.host), config_.port);
    connectTimer_->start(config_.connectTimeoutMs);  // QTcpSocket 没有内建连接超时，用 QTimer 兜底
    return true;
}

void TcpTransport::close() {
    connectTimer_->stop();
    writeQueue_.clear();  // 链路已断，积压字节不再有意义
    if (socket_->state() != QAbstractSocket::UnconnectedState) {
        socket_->abort();
    }
    notifyState(TransportState::Closed);
}

bool TcpTransport::isOpen() const {
    return socket_->state() == QAbstractSocket::ConnectedState;
}

bool TcpTransport::send(const uint8_t* data, std::size_t len) {
    if (!isOpen() || data == nullptr || len == 0) {
        return false;
    }

    // 已有积压时直接追加：必须保证「先调用的帧先上线」，否则 Modbus 事务会错配。
    if (!writeQueue_.isEmpty()) {
        writeQueue_.append(reinterpret_cast<const char*>(data), static_cast<int>(len));
        return true;
    }

    const qint64 written = socket_->write(reinterpret_cast<const char*>(data),
                                          static_cast<qint64>(len));
    if (written < 0) {
        return false;  // 真正的链路错误
    }

    // write 之后不等待：真正的发送由 Qt 事件循环完成。
    // 这和项目一的「写不完就注册 EPOLLOUT」是同一个模型 ——
    // 早期版本只调用一次 write 并把「写不全」直接当失败返回，
    // 结果是半个 Modbus 帧已经进入链路、从站一直等剩余字节，后续所有帧全部错位。
    // 现在把剩余字节存进 writeQueue_，等 bytesWritten 信号续发。
    if (static_cast<std::size_t>(written) < len) {
        writeQueue_.append(reinterpret_cast<const char*>(data) + written,
                           static_cast<int>(len - static_cast<std::size_t>(written)));
    }
    return true;
}

std::string TcpTransport::describe() const {
    return "tcp://" + config_.host + ":" + std::to_string(config_.port);
}

void TcpTransport::handleConnected() {
    connectTimer_->stop();
    notifyState(TransportState::Open);
    notifyMessage("已连接 " + describe());
}

void TcpTransport::handleDisconnected() {
    // 重连策略属于阶段 9；这一层只上报状态，由上层决定是否重连。
    writeQueue_.clear();
    notifyState(TransportState::Closed);
    notifyMessage("连接已断开 " + describe());
}

void TcpTransport::handleReadyRead() {
    while (socket_->bytesAvailable() > 0) {
        const QByteArray chunk = socket_->readAll();
        if (chunk.isEmpty()) {
            break;
        }
        notifyBytes(reinterpret_cast<const uint8_t*>(chunk.constData()),
                    static_cast<std::size_t>(chunk.size()));
    }
}

void TcpTransport::handleBytesWritten(qint64 bytes) {
    Q_UNUSED(bytes);
    // 发送缓冲腾出空间了，把上一帧没写完的尾巴续上。
    flushWriteQueue();
}

void TcpTransport::flushWriteQueue() {
    while (!writeQueue_.isEmpty() && isOpen()) {
        const qint64 written = socket_->write(writeQueue_);
        if (written <= 0) {
            return;  // 仍然写不进去：保留积压，等下一次 bytesWritten
        }
        writeQueue_.remove(0, static_cast<int>(written));
    }
}

void TcpTransport::handleError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error);
    notifyState(TransportState::Error);
    notifyMessage("链路错误：" + socket_->errorString().toStdString());
}

void TcpTransport::handleConnectTimeout() {
    if (isOpen()) {
        return;
    }
    socket_->abort();
    notifyState(TransportState::Error);
    notifyMessage("连接超时 " + describe());
}

}  // namespace gateway::transport
