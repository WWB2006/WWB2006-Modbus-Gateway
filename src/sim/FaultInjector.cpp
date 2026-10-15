#include "gateway/sim/FaultInjector.h"

#include <algorithm>
#include <sstream>

#include "gateway/json/JsonValue.h"

namespace gateway::sim {

const char* toString(FaultKind kind) noexcept { return faultKindToName(kind); }

const char* faultKindToName(FaultKind kind) noexcept {
    switch (kind) {
        case FaultKind::None:
            return "none";
        case FaultKind::NoResponse:
            return "no-response";
        case FaultKind::CorruptCrc:
            return "corrupt-crc";
        case FaultKind::ExceptionCode:
            return "exception";
        case FaultKind::TruncatedFrame:
            return "truncated";
        case FaultKind::BadProtocolId:
            return "bad-protocol-id";
        case FaultKind::DelayedResponse:
            return "delayed";
    }
    return "unknown";
}

bool faultKindFromName(const std::string& name, FaultKind& out) noexcept {
    if (name == "none") {
        out = FaultKind::None;
    } else if (name == "no-response" || name == "noresponse" || name == "timeout") {
        out = FaultKind::NoResponse;
    } else if (name == "corrupt-crc" || name == "bad-crc" || name == "crc") {
        out = FaultKind::CorruptCrc;
    } else if (name == "exception") {
        out = FaultKind::ExceptionCode;
    } else if (name == "truncated" || name == "truncate") {
        out = FaultKind::TruncatedFrame;
    } else if (name == "bad-protocol-id" || name == "badprotocolid") {
        out = FaultKind::BadProtocolId;
    } else if (name == "delayed" || name == "delay") {
        out = FaultKind::DelayedResponse;
    } else {
        return false;
    }
    return true;
}

void FaultInjector::addRule(const FaultRule& rule) {
    Entry entry;
    entry.rule = rule;
    rules_.push_back(entry);
}

void FaultInjector::clearRules() { rules_.clear(); }

InjectionDecision FaultInjector::decide(uint8_t functionCode) {
    for (std::size_t i = 0; i < rules_.size(); ++i) {
        Entry& entry = rules_[i];

        // 功能码过滤：0 = 不限。
        if (entry.rule.functionCode != 0 && entry.rule.functionCode != functionCode) {
            continue;
        }

        // startAfter：前若干次匹配请求保持正常，让主站先跑出几个好样本。
        // 注意 matched 在「通过功能码过滤」后就自增，所以 startAfter 计的是
        // 「匹配该规则的请求数」而不是「全部请求数」—— 与直觉一致。
        const uint32_t matchedIndex = entry.matched;
        entry.matched += 1;

        if (matchedIndex < entry.rule.startAfter) {
            continue;
        }

        // maxInjections：达到上限后这条规则不再注入。
        if (entry.rule.maxInjections != 0 && entry.injected >= entry.rule.maxInjections) {
            continue;
        }

        // interval：每 interval 次触发一次，从 startAfter 之后开始算。
        // offset = matchedIndex - startAfter，offset % interval == 0 时触发。
        const uint32_t offset = matchedIndex - entry.rule.startAfter;
        const uint32_t interval = std::max<uint32_t>(1, entry.rule.interval);
        if (offset % interval != 0) {
            continue;
        }

        InjectionDecision decision;
        decision.inject = true;
        decision.kind = entry.rule.kind;
        decision.byteIndex = entry.rule.byteIndex;
        decision.delayMs = entry.rule.delayMs;
        // 决策即计数：decide() 被调用一次就意味着这条规则已经"命中并注入"一次。
        // 之所以不拆成"先决策、执行后再 noteApplied"两步，是因为 SlaveSimulator
        // 里"决定注入"和"发送响应"之间没有可能失败的中间步骤 ——
        // 拆开只会多一个"忘记 noteApplied 导致 interval 永远算不对"的坑。
        entry.injected += 1;
        totalInjections_ += 1;
        return decision;
    }

    return InjectionDecision{};
}

uint32_t FaultInjector::injectionsOf(FaultKind kind) const noexcept {
    uint32_t count = 0;
    for (const Entry& entry : rules_) {
        if (entry.rule.kind == kind) {
            count += entry.injected;
        }
    }
    return count;
}

void FaultInjector::reset() {
    for (Entry& entry : rules_) {
        entry.matched = 0;
        entry.injected = 0;
    }
    totalInjections_ = 0;
}

std::string FaultInjector::describe() const {
    if (rules_.empty()) {
        return "故障注入：未配置（模拟器行为完全正常）";
    }
    std::ostringstream out;
    out << "故障注入：共 " << rules_.size() << " 条规则，已注入 " << totalInjections_ << " 次\n";
    for (const Entry& entry : rules_) {
        out << "  - " << faultKindToName(entry.rule.kind);
        if (entry.rule.functionCode != 0) {
            out << " 功能码 0x" << std::hex << static_cast<int>(entry.rule.functionCode) << std::dec;
        }
        out << " 每 " << entry.rule.interval << " 次触发";
        if (entry.rule.startAfter != 0) {
            out << "，前 " << entry.rule.startAfter << " 次正常";
        }
        if (entry.rule.maxInjections != 0) {
            out << "，最多 " << entry.rule.maxInjections << " 次";
        }
        out << "（已注入 " << entry.injected << " 次）";
        if (entry.rule.kind == FaultKind::DelayedResponse) {
            out << "，延迟 " << entry.rule.delayMs << "ms";
        }
        out << "\n";
    }
    return out.str();
}

bool applyFault(const InjectionDecision& decision, const std::vector<uint8_t>& response,
                std::vector<uint8_t>& out) {
    out.clear();
    if (!decision.inject) {
        out = response;
        return true;
    }

    switch (decision.kind) {
        case FaultKind::None:
            out = response;
            return true;

        case FaultKind::NoResponse:
            // 不发送任何字节。调用方据此跳过 send 调用，主站会等到超时。
            out.clear();
            return false;

        case FaultKind::CorruptCrc: {
            if (response.empty()) {
                return false;
            }
            out = response;
            // 默认改最后一个字节（TCP 帧没有 CRC，但这里模拟的是"帧内容被干扰"；
            // 对 TCP 来说表现为长度合法但内容错乱，主站解出的值不对）。
            std::size_t index = decision.byteIndex >= 0
                                    ? static_cast<std::size_t>(decision.byteIndex)
                                    : out.size() - 1;
            if (index >= out.size()) {
                index = out.size() - 1;
            }
            out[index] ^= 0xFF;
            return true;
        }

        case FaultKind::ExceptionCode:
            // 异常响应由 RequestHandler 生成（它知道具体异常码）；
            // 走到这里说明规则没有配套设置异常码，按"原样返回"处理以免制造非法帧。
            out = response;
            return true;

        case FaultKind::TruncatedFrame: {
            if (response.size() < 2) {
                out.clear();
                return false;
            }
            // 只发前半段：主站攒着等后半段，最终超时。
            out.assign(response.begin(), response.begin() + static_cast<std::ptrdiff_t>(response.size() / 2));
            return true;
        }

        case FaultKind::BadProtocolId: {
            if (response.size() < 3) {
                out = response;
                return true;
            }
            out = response;
            // MBAP 的协议标识在字节 2-3（事务号占 0-1），必须为 0。
            // 改成非 0 让主站判非法帧并清缓冲。
            std::size_t index = decision.byteIndex >= 0 ? static_cast<std::size_t>(decision.byteIndex)
                                                        : 2u;
            if (index + 1 >= out.size()) {
                index = 2;
            }
            if (index + 1 < out.size()) {
                out[index] = 0x00;
                out[index + 1] = 0x01;  // 协议标识 = 1，非法
            }
            return true;
        }

        case FaultKind::DelayedResponse:
            // 延迟由调用方按 decision.delayMs 处理（这里原样返回帧）。
            out = response;
            return true;
    }
    out = response;
    return true;
}

std::size_t parseFaultRules(const std::string& jsonText, std::vector<FaultRule>& out,
                            std::string& error) {
    error.clear();
    out.clear();

    gateway::json::Value root;
    if (!gateway::json::parse(jsonText, root, error)) {
        return 0;
    }
    if (!root.isObject()) {
        error = "故障规则配置的根节点必须是对象";
        return 0;
    }
    const gateway::json::Value* faults = root.member("faults");
    if (faults == nullptr) {
        error = "缺少 faults 段";
        return 0;
    }
    if (!faults->isArray()) {
        error = "faults 必须是数组";
        return 0;
    }

    std::size_t count = 0;
    for (std::size_t i = 0; i < faults->array.size(); ++i) {
        const gateway::json::Value& node = faults->array[i];
        if (!node.isObject()) {
            error = "第 " + std::to_string(i + 1) + " 条故障规则必须是对象";
            return count;
        }

        FaultRule rule;
        const gateway::json::Value* kindNode = node.member("kind");
        if (kindNode == nullptr || !kindNode->isString()) {
            error = "第 " + std::to_string(i + 1) + " 条故障规则缺少 kind 字段";
            return count;
        }
        if (!faultKindFromName(kindNode->text, rule.kind)) {
            error = "第 " + std::to_string(i + 1) + " 条故障规则的 kind 无法识别：" + kindNode->text;
            return count;
        }

        // 各可选字段。类型不对就报错，避免"配了但不生效"。
        struct Field {
            const char* name;
            double* target;
        };
        double functionCode = 0;
        double interval = 1;
        double startAfter = 0;
        double maxInjections = 0;
        double delayMs = 0;
        double byteIndex = -1;
        const Field fields[] = {
            {"functionCode", &functionCode}, {"interval", &interval},
            {"startAfter", &startAfter},     {"maxInjections", &maxInjections},
            {"delayMs", &delayMs},           {"byteIndex", &byteIndex},
        };
        for (const Field& field : fields) {
            const gateway::json::Value* value = node.member(field.name);
            if (value == nullptr) {
                continue;
            }
            if (!value->isNumber()) {
                error = "第 " + std::to_string(i + 1) + " 条故障规则的 " + field.name +
                        " 必须是数字";
                return count;
            }
            *field.target = value->number;
        }

        rule.functionCode = static_cast<uint8_t>(functionCode);
        rule.interval = static_cast<uint32_t>(interval < 1 ? 1 : interval);
        rule.startAfter = static_cast<uint32_t>(startAfter < 0 ? 0 : startAfter);
        rule.maxInjections = static_cast<uint32_t>(maxInjections < 0 ? 0 : maxInjections);
        rule.delayMs = static_cast<uint32_t>(delayMs < 0 ? 0 : delayMs);
        rule.byteIndex = static_cast<int>(byteIndex);

        out.push_back(rule);
        ++count;
    }
    return count;
}

}  // namespace gateway::sim
