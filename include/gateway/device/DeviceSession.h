#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gateway/device/DeviceConfig.h"
#include "gateway/protocol/ExceptionCode.h"
#include "gateway/protocol/ModbusFrame.h"
#include "gateway/transport/ByteAccumulator.h"

namespace gateway::transport {
class TransportInterface;
}

namespace gateway::device {

enum class SessionState {
    Idle,
    WaitingResponse,
    Failed,
};

const char* toString(SessionState state) noexcept;

// 运行指标。项目一用 Prometheus 暴露连接数、请求数、P99 直方图；
// 这里先按设备累计最关键的几类事件，阶段 7 再接到曲线与指标接口上。
struct DeviceStats {
    uint64_t requests = 0;         // 发起的逻辑请求数（不含重传）
    uint64_t responses = 0;        // 收到的有效响应数（含异常响应）
    uint64_t timeouts = 0;         // 超时次数
    uint64_t retries = 0;          // 重传次数
    uint64_t crcErrors = 0;        // CRC 校验失败帧数
    uint64_t droppedResponses = 0; // 事务号/单元号不匹配，被判定为迟到或他人响应
    uint64_t exceptions = 0;       // 从站返回异常码次数
    uint64_t protocolErrors = 0;   // 非法协议标识、非法长度、未知功能码
    // 阶段 9：链路自愈相关。
    uint64_t reconnects = 0;         // 成功重连次数
    uint64_t reconnectAttempts = 0;  // 发起的重连尝试次数（含失败）
};

struct TransactionResult {
    bool valid = false;
    bool timedOut = false;
    uint8_t functionCode = 0;
    uint16_t address = 0;
    uint16_t count = 0;
    uint16_t value = 0;  // 写单个寄存器的回显值
    std::vector<uint16_t> values;
    protocol::ExceptionCode exception = protocol::ExceptionCode::None;
    std::string error;
    // 本次事务从发出到拿到结果（或判超时）的耗时，毫秒。
    // 阶段 7 的指标收集器用这个值算 P50/P95/P99。
    // 注意它包含重传时间：一个事务重传三次后的总耗时才是"用户实际等了多久"。
    uint32_t latencyMs = 0;
};

// 一台设备的会话：把「一次请求 → 一个响应」的全过程管起来。
//
// 关键约束（与项目二的指导文档一致）：
//   1. 同一时刻只允许一个在途事务，避免响应错配；
//   2. 超时由外部时钟驱动，不自己起线程，也不阻塞等待；
//   3. 半包、粘包、坏 CRC 全部在 onBytes 里消化，不把错误状态泄漏给上层。
//
// [与项目一对应] 项目一用 TimerQueue + epoll_wait 的超时值驱动定时任务；
// 这里改成调用方每轮循环调用 tick(nowMs)，Qt 侧由 QTimer 驱动、测试侧由假时钟驱动。
// 这样设备层不需要知道时间从哪来，也不需要 Qt。
class DeviceSession {
public:
    using ResultCallback = std::function<void(const TransactionResult&)>;

    explicit DeviceSession(DeviceConfig config);

    void setTransport(transport::TransportInterface* transport) { transport_ = transport; }
    // 绑定链路并同时接管它的字节回调。推荐用这个而不是 setTransport：
    // 少写一行接线代码，也少一次「忘记把字节转给会话」的机会。
    void attach(transport::TransportInterface& transport);
    void setResultCallback(ResultCallback callback) { resultCallback_ = std::move(callback); }
    void setAutoPoll(bool enabled) { autoPoll_ = enabled; }

    // 三个显式请求入口，与轮询点支持的三种功能码一一对应。
    // 返回值只表示「请求是否被受理并已发出」，结果通过 resultCallback 异步给出。
    bool requestReadHoldingRegisters(uint16_t address, uint16_t count);
    bool requestReadInputRegisters(uint16_t address, uint16_t count);
    bool requestWriteSingleRegister(uint16_t address, uint16_t value);

    // 由链路回调驱动：收到多少字节就交进来多少，内部负责攒包与切帧。
    void onBytes(const uint8_t* data, std::size_t len);

    // 由外部时钟驱动：检查超时、决定是否重传、决定是否发起下一次轮询。
    void tick(uint64_t nowMs);

    // 主动驱动链路收一次数据（阶段 11）。
    // 对 Qt 链路是空操作（事件循环已经在做）；对原生链路（NativeTcpTransport）
    // 会真正去读一次 socket。无界面网关的主循环每轮调用它，
    // 否则数据永远进不来 —— 这正是「链路能发不能收」这类 bug 的来源。
    void pump();

    bool busy() const { return pending_.active; }
    SessionState state() const { return state_; }
    const DeviceStats& stats() const { return stats_; }
    const DeviceConfig& config() const { return config_; }
    std::size_t bufferedBytes() const { return accumulator_.size(); }

    // 阶段 9：链路状态回调。
    // 链路由 Open 变 Error/Closed 时，会话会进入重连倒计时；
    // 上层（界面 / 网关）通过这个回调得知"设备掉线了 / 恢复了"。
    enum class LinkEvent { Down, Reconnecting, Recovered };
    using LinkCallback = std::function<void(LinkEvent, const std::string& detail)>;
    void setLinkCallback(LinkCallback callback) { linkCallback_ = std::move(callback); }

    // 当前是否处于等待重连状态（链路断开、退避倒计时中）。
    bool waitingReconnect() const noexcept { return reconnectScheduled_; }
    // 下一次重连尝试的绝对时刻（毫秒）。
    uint64_t nextReconnectAtMs() const noexcept { return nextReconnectAtMs_; }

    // 链路当前状态。
    // 这是「状态唯一所有者」原则的落点：链路的 onState 回调只由会话注册，
    // DeviceManager / 界面都通过这里读，不再各自注册 onState 把彼此覆盖掉。
    transport::TransportState linkState() const;

    // 停止会话：不再自动轮询、不再自动重连。
    // 程序退出或用户主动断开时调用 —— 否则「主动断开」会被误判为掉线而触发重连。
    void stop();

private:
    struct Pending {
        bool active = false;
        uint16_t transactionId = 0;
        uint8_t functionCode = 0;
        uint16_t address = 0;
        uint16_t count = 0;
        uint16_t value = 0;
        uint8_t attempts = 0;
        uint64_t deadlineMs = 0;
        // 第一次发出的时刻。用来算事务总耗时（含所有重传），
        // 与只记最后一次的 deadlineMs 不同。
        uint64_t firstSentMs = 0;
    };

    std::vector<uint8_t> buildPduFor(const Pending& pending) const;
    // 三个请求入口共用的骨架：校验在途事务、构造 PDU、登记事务、发出。
    bool beginTransaction(uint8_t functionCode, uint16_t address, uint16_t count, uint16_t value);
    bool sendPending(uint64_t nowMs);
    void handleFrame(const protocol::Frame& frame);
    void resolve(const TransactionResult& result);
    void failPending(const std::string& reason);

    // 阶段 9：链路断开时的处理。安排一次重连（按退避策略算延迟）。
    void scheduleReconnect(uint64_t nowMs, const std::string& reason);
    // 到点后执行重连尝试。成功返回 true。
    bool attemptReconnect();
    // 根据连续失败次数算下一次退避延迟。
    uint32_t backoffDelayMs() const;

    DeviceConfig config_;
    transport::TransportInterface* transport_ = nullptr;
    transport::ByteAccumulator accumulator_;
    Pending pending_;
    DeviceStats stats_;
    ResultCallback resultCallback_;
    LinkCallback linkCallback_;
    SessionState state_ = SessionState::Idle;
    uint64_t nowMs_ = 0;
    uint64_t nextPollAtMs_ = 0;
    std::size_t nextPointIndex_ = 0;
    uint16_t nextTransactionId_ = 1;
    bool autoPoll_ = false;

    // 阶段 9：重连状态。
    bool reconnectScheduled_ = false;
    uint64_t nextReconnectAtMs_ = 0;
    uint32_t consecutiveReconnectFailures_ = 0;
    bool stopRequested_ = false;
};

}  // namespace gateway::device
