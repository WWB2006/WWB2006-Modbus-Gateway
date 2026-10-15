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
    // 阶段 9：监听链路状态变化，掉线即安排重连。
    transport.onState([this](transport::TransportState state) {
        if (state == transport::TransportState::Error ||
            state == transport::TransportState::Closed) {
            // 主动 close（比如程序退出）不算故障：只有"本该连着却断了"才重连。
            // 这里用 linkWasDown_ 做闸门，避免 onState 反复触发导致重复安排。
            if (!reconnectScheduled_ && config_.reconnect.enabled && !stopRequested_) {
                scheduleReconnect(nowMs_, std::string("链路断开：") +
                                              transport::toString(state));
            }
        }
    });
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
    if (pending_.attempts == 0) {
        pending_.firstSentMs = nowMs;  // 只记第一次，重传不覆盖
    }
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

    // 阶段 9：先处理重连倒计时。倒计时期间不发起新的轮询请求，
    // 否则会往一个已经断开的链路里塞请求、每次都失败并刷爆日志。
    if (reconnectScheduled_) {
        if (nowMs_ < nextReconnectAtMs_) {
            return;
        }
        attemptReconnect();
        return;
    }

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

void DeviceSession::pump() {
    // 会话不解释字节，只负责把「该收数据了」这件事转达给链路。
    // 收到的字节会经由 attach() 注册的 onBytes 回调回到 onBytes()。
    if (transport_ != nullptr) {
        transport_->pump();
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
    // 补上本次事务的总耗时：从第一次发出（而不只是最后一次重传）算起，
    // 这样重传带来的延迟才不会在指标里"消失"。
    TransactionResult stamped = result;
    if (pending_.active) {
        stamped.latencyMs = static_cast<uint32_t>(nowMs_ >= pending_.firstSentMs
                                                      ? nowMs_ - pending_.firstSentMs
                                                      : 0);
    }
    pending_ = Pending{};
    state_ = SessionState::Idle;
    nextPollAtMs_ = nowMs_ + config_.pollIntervalMs;
    if (resultCallback_) {
        resultCallback_(stamped);
    }
}

// ------------------------------------------------ 阶段 9：断线重连

uint32_t DeviceSession::backoffDelayMs() const {
    const ReconnectPolicy& policy = config_.reconnect;
    // 前 failuresBeforeBackoff 次失败不进入退避，用初始延迟快速重试
    // （现场多数掉线是瞬时的，快速重试能立刻恢复）。
    if (consecutiveReconnectFailures_ < policy.failuresBeforeBackoff) {
        return policy.initialDelayMs;
    }
    // 之后指数增长：initial * multiplier^(失败次数 - 阈值)，封顶 maxDelayMs。
    const uint32_t exponent = consecutiveReconnectFailures_ - policy.failuresBeforeBackoff;
    uint64_t delay = policy.initialDelayMs;
    const uint32_t multiplier = policy.multiplier == 0 ? 2 : policy.multiplier;
    for (uint32_t i = 0; i < exponent; ++i) {
        delay *= multiplier;
        // 提前封顶，避免 32 次失败后 overflow。
        if (delay >= policy.maxDelayMs) {
            return policy.maxDelayMs;
        }
    }
    return static_cast<uint32_t>(delay);
}

void DeviceSession::scheduleReconnect(uint64_t nowMs, const std::string& reason) {
    if (stopRequested_ || !config_.reconnect.enabled) {
        return;
    }
    // 掉线时把在途事务判失败，否则它会一直"等响应"直到超时，
    // 让上层误以为设备还在。
    if (pending_.active) {
        TransactionResult result;
        result.functionCode = pending_.functionCode;
        result.address = pending_.address;
        result.count = pending_.count;
        result.error = "链路断开，事务中止：" + reason;
        resolve(result);
    }
    // 清空接收缓冲：断开后残留的半帧是垃圾数据，留着会让重连后的第一帧解析错位。
    accumulator_.clear();

    reconnectScheduled_ = true;
    nextReconnectAtMs_ = nowMs + backoffDelayMs();
    state_ = SessionState::Failed;
    if (linkCallback_) {
        linkCallback_(LinkEvent::Down, reason);
    }
}

bool DeviceSession::attemptReconnect() {
    if (transport_ == nullptr || stopRequested_) {
        reconnectScheduled_ = false;
        return false;
    }
    stats_.reconnectAttempts += 1;

    // 链路已经是开的（比如 TCP 自己重连成功了）就直接恢复。
    if (transport_->isOpen()) {
        reconnectScheduled_ = false;
        consecutiveReconnectFailures_ = 0;
        state_ = SessionState::Idle;
        nextPollAtMs_ = nowMs_;  // 立刻允许下一轮轮询
        stats_.reconnects += 1;
        if (linkCallback_) {
            linkCallback_(LinkEvent::Recovered, "链路已恢复");
        }
        return true;
    }

    if (transport_->open()) {
        reconnectScheduled_ = false;
        consecutiveReconnectFailures_ = 0;
        state_ = SessionState::Idle;
        nextPollAtMs_ = nowMs_;
        stats_.reconnects += 1;
        if (linkCallback_) {
            linkCallback_(LinkEvent::Recovered, "重连成功");
        }
        return true;
    }

    // 失败：累加失败次数，按退避策略安排下一次。
    consecutiveReconnectFailures_ += 1;
    nextReconnectAtMs_ = nowMs_ + backoffDelayMs();
    if (linkCallback_) {
        linkCallback_(LinkEvent::Reconnecting,
                      "第 " + std::to_string(consecutiveReconnectFailures_) +
                          " 次重连失败，下次在 " + std::to_string(backoffDelayMs()) + "ms 后");
    }
    return false;
}

transport::TransportState DeviceSession::linkState() const {
    if (transport_ == nullptr) {
        return transport::TransportState::Closed;
    }
    return transport_->state();
}

void DeviceSession::stop() {
    stopRequested_ = true;
    autoPoll_ = false;
    reconnectScheduled_ = false;
    if (pending_.active) {
        TransactionResult result;
        result.functionCode = pending_.functionCode;
        result.address = pending_.address;
        result.count = pending_.count;
        result.error = "会话已停止";
        resolve(result);
    }
}

}  // namespace gateway::device
