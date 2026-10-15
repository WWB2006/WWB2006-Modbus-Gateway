#include "gateway/transport/SerialTransport.h"

#include <QSerialPort>
#include <QTimer>

#include <algorithm>
#include <cstdio>

namespace gateway::transport {
namespace {

// 把 TransportConfig::parity 的单字符映射成 QSerialPort 的枚举。
// 只认 N / E / O 三种（Modbus 规范里 RTU 常用这三种）；其它值退化为无校验。
QSerialPort::Parity toQtParity(char parity) {
    switch (parity) {
        case 'E':
        case 'e':
            return QSerialPort::EvenParity;
        case 'O':
        case 'o':
            return QSerialPort::OddParity;
        case 'N':
        case 'n':
        default:
            return QSerialPort::NoParity;
    }
}

QSerialPort::DataBits toQtDataBits(int dataBits) {
    switch (dataBits) {
        case 5:
            return QSerialPort::Data5;
        case 6:
            return QSerialPort::Data6;
        case 7:
            return QSerialPort::Data7;
        case 8:
        default:
            return QSerialPort::Data8;
    }
}

QSerialPort::StopBits toQtStopBits(int stopBits) {
    return stopBits == 2 ? QSerialPort::TwoStop : QSerialPort::OneStop;
}

// 3.5 字符时间：1 个字符 = 1 起始位 + 数据位 + 校验位 + 停止位。
// 9600bps / 8N1 → 10 bit/字符 → 3.5 字符 ≈ 3.65ms，向上取 4ms。
// 这里按「字符位宽 × 3.5 + 1ms 裕量」估算，并夹在 [1, 50] ms。
int estimateFrameGapMs(int baudRate, int dataBits, char parity, int stopBits) {
    if (baudRate <= 0) {
        return 10;
    }
    const int bitsPerChar = 1 + dataBits + (parity == 'N' || parity == 'n' ? 0 : 1) + stopBits;
    const double ms = (bitsPerChar * 3.5) * 1000.0 / static_cast<double>(baudRate);
    int gap = static_cast<int>(ms) + 1;  // +1ms 裕量，抵消调度抖动
    gap = std::max(1, std::min(50, gap));
    return gap;
}

}  // namespace

SerialTransport::SerialTransport(const TransportConfig& config, QObject* parent)
    : QObject(parent), config_(config) {
    frameGapMs_ = estimateFrameGapMs(config_.baudRate, config_.dataBits, config_.parity,
                                     config_.stopBits);
}

SerialTransport::~SerialTransport() { close(); }

bool SerialTransport::open() {
    notifyState(TransportState::Connecting);

    if (!port_) {
        port_ = std::make_unique<QSerialPort>();
    }
    if (port_->isOpen()) {
        port_->close();
    }

    port_->setPortName(QString::fromStdString(config_.serialPort));
    port_->setBaudRate(config_.baudRate);
    port_->setParity(toQtParity(config_.parity));
    port_->setDataBits(toQtDataBits(config_.dataBits));
    port_->setStopBits(toQtStopBits(config_.stopBits));
    // Modbus RTU 要求无流控。
    port_->setFlowControl(QSerialPort::NoFlowControl);

    if (!port_->open(QIODevice::ReadWrite)) {
        notifyMessage("打开串口失败：" + port_->errorString().toStdString());
        notifyState(TransportState::Error);
        return false;
    }

    if (!gapTimer_) {
        gapTimer_ = new QTimer(this);
        gapTimer_->setSingleShot(true);
        connect(gapTimer_, &QTimer::timeout, this, &SerialTransport::handleFrameGapTimeout);
    }

    connect(port_.get(), &QSerialPort::readyRead, this, &SerialTransport::handleReadyRead);
    connect(port_.get(), &QSerialPort::errorOccurred, this, &SerialTransport::handleError);

    notifyState(TransportState::Open);
    notifyMessage("串口已打开：" + describe());
    return true;
}

void SerialTransport::close() {
    if (gapTimer_) {
        gapTimer_->stop();
    }
    if (port_) {
        if (port_->isOpen()) {
            port_->close();
        }
        port_->disconnect(this);
    }
    notifyState(TransportState::Closed);
}

bool SerialTransport::isOpen() const { return port_ != nullptr && port_->isOpen(); }

bool SerialTransport::send(const uint8_t* data, std::size_t len) {
    if (!isOpen() || data == nullptr || len == 0) {
        return false;
    }
    // 串口写入不需要排队重试：QSerialPort 的 write 成功后由 Qt 后端异步发出。
    // 半帧写完的情况由驱动层的写缓冲兜住，不在这里手搓队列（与 TCP 不同，
    // 因为串口的写缓冲远小于网络窗口，实际不会出现"写不动"）。
    const qint64 written =
        port_->write(reinterpret_cast<const char*>(data), static_cast<qint64>(len));
    if (written != static_cast<qint64>(len)) {
        notifyMessage("串口写入不完整：" + std::to_string(written) + "/" + std::to_string(len));
        return false;
    }
    return port_->flush();
}

std::string SerialTransport::describe() const {
    std::string text = "serial://" + config_.serialPort + "@" + std::to_string(config_.baudRate);
    text += " ";
    text += config_.parity;
    text += std::to_string(config_.dataBits);
    text += std::to_string(config_.stopBits);
    text += " (帧间静默 " + std::to_string(frameGapMs_) + "ms)";
    return text;
}

void SerialTransport::handleReadyRead() {
    if (!port_) {
        return;
    }
    const QByteArray chunk = port_->readAll();
    if (chunk.isEmpty()) {
        return;
    }
    // 必须立即把字节转给设备层：串口的"帧结束"是靠静默间隔推断的，
    // 若在这里攒包再一次性交付，设备层就失去了时间信息，无法区分
    // 「半包」和「两帧」。因此本类只做「字节到达 → 原样上报」。
    notifyBytes(reinterpret_cast<const uint8_t*>(chunk.constData()),
                static_cast<std::size_t>(chunk.size()));

    // 重启静默定时器。它超时只代表"总线安静下来了"，用于日志与诊断；
    // 真正的帧边界由设备层的 RTU 解帧循环处理（解出完整帧就消费掉）。
    if (gapTimer_) {
        gapTimer_->start(frameGapMs_);
    }
}

void SerialTransport::handleFrameGapTimeout() {
    // 静默间隔到达。设备层已经在 onBytes 里尝试过解帧，这里只补一条诊断信息，
    // 便于排查「设备回了一串但拼不出合法帧」的情况（例如波特率不匹配）。
    if (pendingBytes_.isEmpty()) {
        notifyMessage("串口静默 " + std::to_string(frameGapMs_) + "ms，总线空闲");
    }
}

void SerialTransport::handleError() {
    if (!port_) {
        return;
    }
    const QSerialPort::SerialPortError error = port_->error();
    if (error == QSerialPort::NoError) {
        return;
    }
    // ResourceError 表示设备被拔掉/转换器掉电，这是现场最常见的情况，
    // 必须上报为 Error 状态，好让阶段 9 的退避重连接管。
    notifyMessage("串口错误：" + port_->errorString().toStdString());
    if (error == QSerialPort::ResourceError || error == QSerialPort::DeviceNotFoundError ||
        error == QSerialPort::PermissionError) {
        notifyState(TransportState::Error);
    }
}

}  // namespace gateway::transport
