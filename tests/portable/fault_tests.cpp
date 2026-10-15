// 故障注入引擎测试：六类故障的决策规则与字节级效果。
// 不依赖 Qt、不起 socket —— 直接喂字节、断言字节。

#include <cstdio>
#include <string>
#include <vector>

#include "gateway/protocol/ModbusCodec.h"
#include "gateway/sim/FaultInjector.h"
#include "test_support.h"

using test_support::check;
using test_support::checkBytes;
using test_support::checkU64;
using test_support::section;

namespace {

using gateway::sim::FaultInjector;
using gateway::sim::FaultKind;
using gateway::sim::FaultRule;
using gateway::sim::InjectionDecision;

// 造一条正常的 TCP 响应帧做被注入的对象。
std::vector<uint8_t> normalResponse() {
    const std::vector<uint8_t> pdu = {0x03, 0x04, 0x00, 0x0A, 0x00, 0x14};
    return gateway::protocol::encodeTcpRequest(1, 1, pdu);
}

void testKindNames() {
    section("故障类型：名称与字符串互转");

    check(std::string(gateway::sim::faultKindToName(FaultKind::NoResponse)) == "no-response",
          "NoResponse 名称");
    check(std::string(gateway::sim::faultKindToName(FaultKind::CorruptCrc)) == "corrupt-crc",
          "CorruptCrc 名称");
    check(std::string(gateway::sim::faultKindToName(FaultKind::DelayedResponse)) == "delayed",
          "DelayedResponse 名称");

    FaultKind kind = FaultKind::None;
    check(gateway::sim::faultKindFromName("corrupt-crc", kind) && kind == FaultKind::CorruptCrc,
          "从名称解析 CorruptCrc");
    check(gateway::sim::faultKindFromName("timeout", kind) && kind == FaultKind::NoResponse,
          "timeout 是 NoResponse 的别名");
    check(!gateway::sim::faultKindFromName("nonsense", kind), "未知名称被拒绝");
}

void testNoRule() {
    section("故障注入：无规则时永远不注入");

    FaultInjector injector;
    checkU64(injector.ruleCount(), 0, "无规则");
    for (int i = 0; i < 5; ++i) {
        const auto decision = injector.decide(0x03);
        check(!decision.inject, "无规则时不注入");
    }
    checkU64(injector.totalInjections(), 0, "注入次数为 0");
}

void testIntervalRule() {
    section("故障注入：interval 决定每几次坏一次");

    FaultInjector injector;
    FaultRule rule;
    rule.kind = FaultKind::NoResponse;
    rule.interval = 3;  // 每 3 次触发一次
    injector.addRule(rule);

    // 连续 9 次请求：第 3、6、9 次（1 基）触发 → 下标 2、5、8。
    int injected = 0;
    for (int i = 0; i < 9; ++i) {
        if (injector.decide(0x03).inject) {
            ++injected;
        }
    }
    checkU64(injected, 3, "9 次请求注入 3 次");
    checkU64(injector.totalInjections(), 3, "总注入次数");
}

void testStartAfterRule() {
    section("故障注入：startAfter 让前几次保持正常");

    FaultInjector injector;
    FaultRule rule;
    rule.kind = FaultKind::NoResponse;
    rule.interval = 1;    // 每次都坏
    rule.startAfter = 2;  // 但前 2 次正常
    injector.addRule(rule);

    check(!injector.decide(0x03).inject, "第 1 次正常");
    check(!injector.decide(0x03).inject, "第 2 次正常");
    check(injector.decide(0x03).inject, "第 3 次开始故障");
    check(injector.decide(0x03).inject, "第 4 次故障");
}

void testMaxInjectionsRule() {
    section("故障注入：maxInjections 限制总次数");

    FaultInjector injector;
    FaultRule rule;
    rule.kind = FaultKind::CorruptCrc;
    rule.interval = 1;
    rule.maxInjections = 2;  // 最多坏 2 次
    injector.addRule(rule);

    check(injector.decide(0x03).inject, "第 1 次注入");
    check(injector.decide(0x03).inject, "第 2 次注入");
    check(!injector.decide(0x03).inject, "第 3 次已达上限，不再注入");
    check(!injector.decide(0x03).inject, "第 4 次仍然正常");
    checkU64(injector.totalInjections(), 2, "总注入 2 次");
}

void testFunctionCodeFilter() {
    section("故障注入：按功能码过滤");

    FaultInjector injector;
    FaultRule rule;
    rule.kind = FaultKind::NoResponse;
    rule.functionCode = 0x04;  // 只坏读输入寄存器
    injector.addRule(rule);

    check(!injector.decide(0x03).inject, "0x03 不受影响");
    check(injector.decide(0x04).inject, "0x04 被注入");
    check(!injector.decide(0x03).inject, "0x03 仍不受影响");
    check(injector.decide(0x04).inject, "0x04 再次被注入");
}

void testRulePriority() {
    section("故障注入：多条规则命中时取第一条");

    FaultInjector injector;
    FaultRule first;
    first.kind = FaultKind::NoResponse;
    FaultRule second;
    second.kind = FaultKind::CorruptCrc;
    injector.addRule(first);
    injector.addRule(second);

    const auto decision = injector.decide(0x03);
    check(decision.inject, "命中了规则");
    check(decision.kind == FaultKind::NoResponse, "取第一条规则的类型");
    checkU64(injector.injectionsOf(FaultKind::NoResponse), 1, "NoResponse 计数 1");
    checkU64(injector.injectionsOf(FaultKind::CorruptCrc), 0, "CorruptCrc 计数 0");
}

void testApplyNoResponse() {
    section("故障注入：NoResponse 不发送任何字节");

    const auto response = normalResponse();
    InjectionDecision decision;
    decision.inject = true;
    decision.kind = FaultKind::NoResponse;

    std::vector<uint8_t> out;
    const bool shouldSend = gateway::sim::applyFault(decision, response, out);
    check(!shouldSend, "标记为不发送");
    checkU64(out.size(), 0, "输出为空");
}

void testApplyCorruptCrc() {
    section("故障注入：CorruptCrc 篡改一个字节");

    const auto response = normalResponse();
    const auto original = response;

    InjectionDecision decision;
    decision.inject = true;
    decision.kind = FaultKind::CorruptCrc;

    std::vector<uint8_t> out;
    check(gateway::sim::applyFault(decision, response, out), "仍然发送");
    checkU64(out.size(), response.size(), "帧长不变");
    check(out != original, "内容已改变");
    // 默认改最后一个字节。
    check(out.back() != original.back(), "最后一个字节被改写");
    check(out.back() == static_cast<uint8_t>(original.back() ^ 0xFF), "是异或 0xFF");
}

void testApplyBadProtocolId() {
    section("故障注入：BadProtocolId 改协议标识");

    const auto response = normalResponse();
    // 正常的 MBAP：事务号 0-1，协议标识 2-3 = 0x0000。
    checkU64(response[2], 0, "正常帧协议标识高字节为 0");
    checkU64(response[3], 0, "正常帧协议标识低字节为 0");

    InjectionDecision decision;
    decision.inject = true;
    decision.kind = FaultKind::BadProtocolId;

    std::vector<uint8_t> out;
    check(gateway::sim::applyFault(decision, response, out), "仍然发送");
    checkU64(out[2], 0x00, "协议标识高字节");
    checkU64(out[3], 0x01, "协议标识低字节被改成 1");

    // 篡改后的帧必须被判为非法协议标识。
    gateway::protocol::DecodeResult decoded;
    const std::size_t consumed = gateway::protocol::decodeTcp(out.data(), out.size(), decoded);
    checkU64(consumed, 0, "非法帧解不出");
    check(decoded.status == gateway::protocol::DecodeStatus::BadProtocolId, "判定为 BadProtocolId");
}

void testApplyTruncated() {
    section("故障注入：TruncatedFrame 只发前半段");

    const auto response = normalResponse();
    InjectionDecision decision;
    decision.inject = true;
    decision.kind = FaultKind::TruncatedFrame;

    std::vector<uint8_t> out;
    check(gateway::sim::applyFault(decision, response, out), "仍然发送");
    checkU64(out.size(), response.size() / 2, "帧长为原来的一半");

    // 截断帧对主站来说应表现为"半包"（数据不足），而不是"坏帧"。
    gateway::protocol::DecodeResult decoded;
    const std::size_t consumed = gateway::protocol::decodeTcp(out.data(), out.size(), decoded);
    checkU64(consumed, 0, "半帧解不出");
    check(decoded.status == gateway::protocol::DecodeStatus::Incomplete, "判定为 Incomplete");
}

void testApplyDelayed() {
    section("故障注入：DelayedResponse 帧原样、延迟由调用方处理");

    const auto response = normalResponse();
    InjectionDecision decision;
    decision.inject = true;
    decision.kind = FaultKind::DelayedResponse;
    decision.delayMs = 1500;

    std::vector<uint8_t> out;
    check(gateway::sim::applyFault(decision, response, out), "仍然发送");
    check(out == response, "帧内容不变");
    checkU64(decision.delayMs, 1500, "延迟时间透传");
}

void testAllSixKindsCovered() {
    section("故障注入：六类故障都能被规则触发");

    const FaultKind kinds[] = {
        FaultKind::NoResponse,    FaultKind::CorruptCrc,      FaultKind::ExceptionCode,
        FaultKind::TruncatedFrame, FaultKind::BadProtocolId, FaultKind::DelayedResponse,
    };
    checkU64(sizeof(kinds) / sizeof(kinds[0]), 6, "共六类故障");

    for (const FaultKind kind : kinds) {
        FaultInjector injector;
        FaultRule rule;
        rule.kind = kind;
        injector.addRule(rule);
        const auto decision = injector.decide(0x03);
        check(decision.inject && decision.kind == kind,
              std::string("可触发：") + gateway::sim::faultKindToName(kind));
    }
}

void testParseRules() {
    section("故障注入：从 JSON 解析规则");

    const std::string text = R"({
      "faults": [
        { "kind": "no-response", "functionCode": 3, "interval": 4, "startAfter": 1 },
        { "kind": "corrupt-crc", "maxInjections": 2 },
        { "kind": "delayed", "delayMs": 800 }
      ]
    })";

    std::vector<FaultRule> rules;
    std::string error;
    const std::size_t count = gateway::sim::parseFaultRules(text, rules, error);
    checkU64(count, 3, "解析出 3 条规则：" + error);
    checkU64(rules.size(), 3, "规则数组长度");
    check(rules[0].kind == FaultKind::NoResponse, "第 1 条类型");
    checkU64(rules[0].functionCode, 3, "第 1 条功能码");
    checkU64(rules[0].interval, 4, "第 1 条间隔");
    checkU64(rules[0].startAfter, 1, "第 1 条起始延迟");
    check(rules[1].kind == FaultKind::CorruptCrc, "第 2 条类型");
    checkU64(rules[1].maxInjections, 2, "第 2 条最大次数");
    checkU64(rules[2].delayMs, 800, "第 3 条延迟");

    // 错误处理。
    std::vector<FaultRule> bad;
    checkU64(gateway::sim::parseFaultRules("{}", bad, error), 0, "缺 faults 段返回 0");
    check(gateway::sim::parseFaultRules(R"({"faults":[{"kind":"nope"}]})", bad, error) == 0,
          "未知 kind 返回 0");
    check(error.find("无法识别") != std::string::npos, "给出可定位原因：" + error);
    check(gateway::sim::parseFaultRules(R"({"faults":[{"interval":3}]})", bad, error) == 0,
          "缺 kind 返回 0");
}

void testResetAndDescribe() {
    section("故障注入：重置与描述");

    FaultInjector injector;
    FaultRule rule;
    rule.kind = FaultKind::CorruptCrc;
    injector.addRule(rule);
    injector.decide(0x03);
    injector.decide(0x03);
    checkU64(injector.totalInjections(), 2, "注入 2 次");

    injector.reset();
    checkU64(injector.totalInjections(), 0, "重置后计数清零");
    checkU64(injector.ruleCount(), 1, "规则保留");
    // 重置后 interval 也从头算。
    check(injector.decide(0x03).inject, "重置后第 1 次仍触发");

    const std::string text = injector.describe();
    check(text.find("corrupt-crc") != std::string::npos, "描述含故障类型");

    FaultInjector empty;
    check(empty.describe().find("未配置") != std::string::npos, "无规则时的描述");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Modbus 故障注入引擎测试（六类故障，不依赖 Qt 与真实网络）\n");

    testKindNames();
    testNoRule();
    testIntervalRule();
    testStartAfterRule();
    testMaxInjectionsRule();
    testFunctionCodeFilter();
    testRulePriority();
    testApplyNoResponse();
    testApplyCorruptCrc();
    testApplyBadProtocolId();
    testApplyTruncated();
    testApplyDelayed();
    testAllSixKindsCovered();
    testParseRules();
    testResetAndDescribe();

    return test_support::summary();
}
