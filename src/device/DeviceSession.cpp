#include "gateway/device/DeviceSession.h"

#include "gateway/protocol/ModbusCodec.h"
#include "gateway/transport/TransportInterface.h"

namespace gateway::device {
namespace {

// 链路类型（Tcp/Serial）与协议类型（Tcp/Rtu）是两套枚举：
// 前者描述「怎么连」，后者描述「帧长怎么算」。RTU 走串口，TCP 走以太网。
protocol::Transport toProtocolTransport(transport::TransportKind kind) {
    return kind == transport::TransportKind::Tcp ? protocol::Transport::Tcp
                                                 : protocol::Transport::Rtu;
}

}  // namespace

const char* toString(SessionState state) noexcept {
    switch (state) {
        case SessionState::Idle:
            return "idle";
        case SessionState::WaitingResponse:
            return "waiting";
        case SessionState::Failed:
            return "failed";
    }
    return "unknown";
}

DeviceSession::DeviceSession(DeviceConfig config) : config_(std::move(config)) {}

void DeviceSession::attach(transport::TransportInterface& transport) {
    transport_ = &transport;
    transport.onBytes([this](const uint8_t* data, std::size_t len) { onBytes(data, len); });
}

bool DeviceSession::beginTransaction(uint8_t functionCode, uint16_t address, uint16_t count,
                                     uint16_t value) {
    if (pending_.active || transport_ == nullptr) {
        return false;  // 一次只允许一个在途事务，避免响应错配
    }

    Pending candidate;
    candidate.functionCode = functionCode;
    candidate.address = address;
    candidate.count = count;
    candidate.value = value;
    // 用 buildPduFor 做参数校验：地址越界、数量为 0 或超过协议上限时它返回空 PDU。
    if (buildPduFor(candidate).empty()) {
        return false;
    }

    candidate.active = true;
    candidate.transactionId = nextTransactionId_++;
    pending_ = candidate;
    stats_.requests += 1;
    state_ = SessionState::WaitingResponse;
    return sendPending(nowMs_);
}

bool DeviceSession::requestReadHoldingRegisters(uint16_t address, uint16_t count) {
    return beginTransaction(0x03, address, count, 0);
}

bool DeviceSession::requestReadInputRegisters(uint16_t address, uint16_t count) {
    return beginTransaction(0x04, address, count, 0);
}

bool DeviceSession::requestWriteSingleRegister(uint16_t address, uint16_t value) {
    // 写单个寄存器固定只写 1 个，count 恒为 1；要写的数值走 value。
    return beginTransaction(0x06, address, 1, value);
}

std::vector<uint8_t> DeviceSession::buildPduFor(const Pending& pending) const {
    switch (pending.functionCode) {
        case 0x03:
            return protocol::buildReadHoldingRegisters(pending.address, pending.count);
        case 0x04:
            return protocol::buildReadInputRegisters(pending.address, pending.count);
        case 0x06:
            return protocol::buildWriteSingleRegister(pending.address, pending.value);
        default:
            return {};
    }
}

bool DeviceSession::sendPending(uint64_t nowMs) {
    const auto pdu = buildPduFor(pending_);
    if (pdu.empty()) {
        failPending("无法构造请求 PDU");
        return false;
    }

    std::vector<uint8_t> frame;
    if (config_.transport == transport::TransportKind::Tcp) {
        frame = protocol::encodeTcpRequest(pending_.transactionId, config_.unitId, pdu);
    } else {
        frame = protocol::encodeRtuRequest(config_.unitId, pdu);
    }
    if (frame.empty()) {
        failPending("无法组帧");
        return false;
    }

    // 先更新状态再发出：真实链路是异步的，但同步的测试替身会在 send 内部立刻回调，
    // 提前写好 attempts 与 deadline 可以避免这种再入把状态改坏。
    pending_.attempts += 1;
    pending_.deadlineMs = nowMs + config_.timeoutMs;
    state_ = SessionState::WaitingResponse;

    if (!transport_->send(frame.data(), frame.size())) {
        failPending("发送失败：链路未打开");
        return false;
    }
    return true;
}

void DeviceSession::tick(uint64_t nowMs) {
    nowMs_ = nowMs;

    if (pending_.active) {
        if (nowMs_ < pending_.deadlineMs) {
            return;  // 还在等响应
        }
        stats_.timeouts += 1;
        if (pending_.attempts <= config_.retry) {
            stats_.retries += 1;
            sendPending(nowMs_);
            return;
        }
        TransactionResult result;
        result.functionCode = pending_.functionCode;
        result.address = pending_.address;
        result.count = pending_.count;
        result.timedOut = true;
        result.error = "等待响应超时，共尝试 " + std::to_string(pending_.attempts) + " 次";
        resolve(result);
        state_ = SessionState::Failed;
        return;
    }

    state_ = SessionState::Idle;

    if (!autoPoll_ || transport_ == nullptr || config_.points.empty()) {
        return;
    }
    if (!transport_->isOpen() || nowMs_ < nextPollAtMs_) {
        return;
    }

    const PollPoint point = config_.points[nextPointIndex_ % config_.points.size()];
    nextPointIndex_ += 1;
    nextPollAtMs_ = nowMs_ + config_.pollIntervalMs;

    switch (point.functionCode) {
        case 0x03:
            requestReadHoldingRegisters(point.address, point.count);
            break;
        case 0x04:
            requestReadInputRegisters(point.address, point.count);
            break;
        case 0x06:
            // 写单寄存器写入的是 value，不是 count：
            // count 表示「读多少个寄存器」，把它当写入值会把配置里的数量误写进设备。
            requestWriteSingleRegister(point.address, point.value);
            break;
        default:
            // 未支持的轮询功能码：本轮跳过，等下一次轮询时机，不阻塞其它点位。
            break;
    }
}

void DeviceSession::onBytes(const uint8_t* data, std::size_t len) {
    accumulator_.append(data, len);

    // 循环切帧：一次收到多帧（粘包）时要全部解出来，收到半帧时保留剩余字节继续等。
    // [与项目一对应] 项目一的 TcpConnection 在可读事件里同样循环调用 HttpParser，
    // 直到缓冲区里没有完整报文为止；两处都是「事件驱动 + 缓冲区 + 状态机」的组合。
    while (accumulator_.size() > 0) {
        protocol::DecodeResult decoded;
        std::size_t consumed = 0;
        if (config_.transport == transport::TransportKind::Tcp) {
            consumed = protocol::decodeTcp(accumulator_.data(), accumulator_.size(), decoded);
        } else {
            consumed =
                protocol::decodeRtuResponse(accumulator_.data(), accumulator_.size(), decoded);
        }

        if (consumed > 0) {
            accumulator_.consume(consumed);
            handleFrame(decoded.frame);
            continue;
        }

        switch (decoded.status) {
            case protocol::DecodeStatus::Incomplete:
                return;  // 半包：保留缓冲区，等下一批字节
            case protocol::DecodeStatus::BadCrc:
                // 误码帧按推测帧长整帧丢弃，避免把紧随其后的正确帧也拆散；
                // 只有连帧长都推断不出来时才退化成逐字节重新同步。
                stats_.crcErrors += 1;
                if (decoded.frameLengthHint > 0 && decoded.frameLengthHint <= accumulator_.size()) {
                    accumulator_.consume(decoded.frameLengthHint);
                } else {
                    accumulator_.consume(1);
                }
                continue;
            default:
                // TCP 是字节流：协议标识或长度字段出错时无法重新同步，
                // 只能清空缓冲并计数；真正的恢复手段是阶段 9 的断开重连。
                stats_.protocolErrors += 1;
                accumulator_.clear();
                return;
        }
    }
}

void DeviceSession::handleFrame(const protocol::Frame& frame) {
    if (!pending_.active) {
        stats_.droppedResponses += 1;  // 没有在途事务，说明是迟到响应
        return;
    }
    if (!protocol::matchesRequest(frame, toProtocolTransport(config_.transport),
                                  pending_.transactionId, config_.unitId,
                                  pending_.functionCode)) {
        stats_.droppedResponses += 1;
        return;
    }

    TransactionResult result;
    result.functionCode = frame.functionCode;
    result.address = pending_.address;
    result.count = pending_.count;

    if (frame.isException()) {
        // 异常响应是「有效响应」：它说明设备确实回复了，因此不重传、不判超时。
        stats_.exceptions += 1;
        stats_.responses += 1;
        result.exception = frame.exceptionCode();
        result.error = protocol::toChineseHint(result.exception);
        resolve(result);
        return;
    }

    switch (pending_.functionCode) {
        case 0x03:
        case 0x04: {
            const auto parsed = protocol::parseReadRegistersResponse(frame, pending_.count);
            result.valid = parsed.valid;
            result.values = parsed.values;
            result.exception = parsed.exception;
            result.error = parsed.error;
            break;
        }
        case 0x06: {
            const auto parsed = protocol::parseWriteSingleResponse(frame);
            result.valid = parsed.valid;
            result.value = parsed.value;
            result.exception = parsed.exception;
            result.error = parsed.error;
            break;
        }
        default:
            result.error = "未支持的响应功能码";
            break;
    }

    if (result.valid) {
        stats_.responses += 1;
    } else {
        stats_.protocolErrors += 1;
    }
    resolve(result);
}

void DeviceSession::failPending(const std::string& reason) {
    TransactionResult result;
    result.functionCode = pending_.functionCode;
    result.address = pending_.address;
    result.count = pending_.count;
    result.error = reason;
    resolve(result);
    state_ = SessionState::Failed;
}

void DeviceSession::resolve(const TransactionResult& result) {
    pending_ = Pending{};
    state_ = SessionState::Idle;
    nextPollAtMs_ = nowMs_ + config_.pollIntervalMs;
    if (resultCallback_) {
        resultCallback_(result);
    }
}

}  // namespace gateway::device
