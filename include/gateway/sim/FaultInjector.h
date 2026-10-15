#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gateway/protocol/ModbusFrame.h"

namespace gateway::sim {

// 六类现场故障。
//
// 为什么需要故障注入引擎（而不是在代码里写死几个 if）：
//   现场的问题永远发生在「概率与时机」上 —— 第 37 次请求丢包、连续两次 CRC 错、
//   设备在重传间隙才回上一帧。这类问题靠人工复现几乎不可能，
//   只能让模拟器**按规则可控地变坏**，把它变成可重复的测试用例。
//
// 这六类覆盖了工业 Modbus 现场绝大多数"读不到数据"的原因：
enum class FaultKind {
    None = 0,
    NoResponse,        // 设备不响应（掉线、断电、地址错）→ 主站必然超时重传
    CorruptCrc,        // 响应帧 CRC 被篡改 → 主站校验失败、丢弃、重传
    ExceptionCode,     // 返回异常码（如非法地址 0x02）→ 链路通但业务失败，不重传
    TruncatedFrame,    // 响应被截断（只发前半段）→ 主站半包等待，最终超时
    BadProtocolId,     // MBAP 协议标识非 0 → 主站判非法帧，清缓冲
    DelayedResponse,   // 响应延迟到超时之后才发 → 主站已重传，这是"迟到响应"
};

const char* toString(FaultKind kind) noexcept;

// 一次故障注入的触发规则。
//
// 规则的表达力刻意设计成「可精确复现」而不是「随机」：
// 随机会让测试变得 flaky（时好时坏），而 flaky 的测试最后会被注释掉。
struct FaultRule {
    FaultKind kind = FaultKind::None;

    // 触发条件 —— 三条都满足才注入。默认全放行。
    // 功能码过滤：0 表示不限功能码。
    uint8_t functionCode = 0;
    // 间隔：每 interval 次匹配的请求触发一次。1 = 每次都触发。
    // 这是复现"偶发丢包"的关键：interval=3 表示每三次坏一次。
    uint32_t interval = 1;
    // 起始延迟：前 startAfter 次匹配的请求保持正常（让主站先建立连接、先跑几个好样本）。
    uint32_t startAfter = 0;
    // 最多注入次数；0 = 不限次。
    uint32_t maxInjections = 0;

    // DelayedResponse 专用：延迟多少毫秒才发。
    uint32_t delayMs = 0;

    // CorruptCrc / BadProtocolId 专用：改第几个字节。
    // -1 表示用「按类型推导的默认位置」（CRC → 倒数第一个字节；协议标识 → 第 3 个字节）。
    int byteIndex = -1;
};

// 一次注入决定：告诉调用方「这条响应该怎么坏」。
struct InjectionDecision {
    FaultKind kind = FaultKind::None;
    // 需要改的字节位置（-1 表示不用改）。
    int byteIndex = -1;
    // 延迟发送的毫秒数（DelayedResponse 用）。
    uint32_t delayMs = 0;
    bool inject = false;

    explicit operator bool() const noexcept { return inject; }
};

// 故障注入引擎：纯逻辑，不碰网络与 Qt。
//
// 用法（SlaveSimulator 内部）：
//   1. 收到请求、生成正常响应后，调用 decide()；
//   2. decide() 返回 inject=true 时按 decision 修改响应（改字节 / 截断 / 丢弃 / 延迟）；
//   3. 记录 apply() 让引擎更新计数，保证 interval / maxInjections 正确推进。
//
// 把「决策」与「执行」分开是为了可测：测试可以只调 decide() 断言决策序列，
// 而不需要真的起一个 socket。
class FaultInjector {
public:
    FaultInjector() = default;

    void addRule(const FaultRule& rule);
    void clearRules();
    std::size_t ruleCount() const noexcept { return rules_.size(); }

    // 根据当前计数与请求功能码决定是否注入、注入哪一类。
    // 多条规则同时命中时，取第一条（规则顺序即优先级，让调用方可控）。
    // 命中即计数：调用方不需要额外调用 noteApplied。
    InjectionDecision decide(uint8_t functionCode);

    // 统计：一共注入了多少次、按类型分。
    uint64_t totalInjections() const noexcept { return totalInjections_; }
    uint32_t injectionsOf(FaultKind kind) const noexcept;

    void reset();

    std::string describe() const;

private:
    // 规则 + 它的运行期计数。
    struct Entry {
        FaultRule rule;
        uint32_t matched = 0;      // 命中的请求数
        uint32_t injected = 0;     // 已注入次数
    };

    std::vector<Entry> rules_;
    uint64_t totalInjections_ = 0;
};

// 施加故障到一条已构造好的响应帧上。返回 false 表示「不要发送这条响应」
// （NoResponse 与 TruncatedFrame 的极端情况），true 表示 out 里是可以发送的（可能是坏帧）。
//
// 独立成自由函数：这样它既能被 SlaveSimulator 用，也能被测试直接调用，
// 不需要构造整个模拟器。
bool applyFault(const InjectionDecision& decision, const std::vector<uint8_t>& response,
                std::vector<uint8_t>& out);

// 从 JSON 文本解析故障规则，供配置文件使用。
// 返回解析成功的规则数；失败时 error 指明第几条规则出了什么问题。
std::size_t parseFaultRules(const std::string& jsonText, std::vector<FaultRule>& out,
                            std::string& error);

// 把 FaultKind 与字符串互转，供配置与日志使用。
const char* faultKindToName(FaultKind kind) noexcept;
bool faultKindFromName(const std::string& name, FaultKind& out) noexcept;

}  // namespace gateway::sim
