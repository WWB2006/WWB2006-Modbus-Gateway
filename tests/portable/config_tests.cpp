// 配置解析测试：网关配置与从站配置的字段约束。
// 不依赖 Qt，因此任何环境都能跑。
//
// 这一层的价值在于：配置文件写错是**静默错误**的主要来源 ——
// 功能码写错、设备名重复、地址越界，程序都能"正常启动"但行为不对。
// 因此把校验前移到加载期，并用测试把这些约束钉死。

#include <cstdio>
#include <string>

#include "gateway/config/GatewayConfig.h"
#include "gateway/json/JsonValue.h"
#include "test_support.h"

using test_support::check;
using test_support::checkU16;
using test_support::checkU64;
using test_support::section;

namespace {

// 仓库根目录（由 main 从 argv[1] 填入）。
// 用例需要读仓库里的真实文件时用它拼绝对路径，不靠当前工作目录猜。
std::string g_repoRoot;

void testJsonBasics() {
    section("JSON 解析：对象 / 数组 / 标量");

    gateway::json::Value root;
    std::string error;
    const std::string text = R"({
        "name": "plc",
        "count": 42,
        "ratio": 1.5,
        "enabled": true,
        "disabled": false,
        "nothing": null,
        "items": [1, 2, 3],
        "nested": { "inner": "value" }
    })";
    check(gateway::json::parse(text, root, error), "解析合法 JSON：" + error);
    check(root.isObject(), "根是对象");

    const gateway::json::Value* name = root.member("name");
    check(name != nullptr && name->isString() && name->text == "plc", "字符串字段");
    const gateway::json::Value* count = root.member("count");
    check(count != nullptr && count->isNumber() && count->number == 42, "整数字段");
    const gateway::json::Value* ratio = root.member("ratio");
    check(ratio != nullptr && ratio->number == 1.5, "浮点字段");
    const gateway::json::Value* enabled = root.member("enabled");
    check(enabled != nullptr && enabled->isBool() && enabled->boolean, "true 字面量");
    const gateway::json::Value* disabled = root.member("disabled");
    check(disabled != nullptr && disabled->isBool() && !disabled->boolean, "false 字面量");
    const gateway::json::Value* nothing = root.member("nothing");
    check(nothing != nullptr && nothing->type == gateway::json::Value::Type::Null, "null 字面量");

    const gateway::json::Value* items = root.member("items");
    check(items != nullptr && items->isArray() && items->array.size() == 3, "数组长度");
    checkU64(static_cast<uint64_t>(items->array[2].number), 3, "数组末尾元素");

    const gateway::json::Value* nested = root.member("nested");
    check(nested != nullptr && nested->isObject(), "嵌套对象");
    const gateway::json::Value* inner = nested->member("inner");
    check(inner != nullptr && inner->text == "value", "嵌套字段");

    check(root.member("missing") == nullptr, "不存在的键返回 nullptr");
}

void testJsonErrors() {
    section("JSON 解析：错误定位");

    gateway::json::Value root;
    std::string error;

    check(!gateway::json::parse("", root, error), "空内容被拒绝");
    check(error == "配置内容为空", "空内容给出明确原因");

    check(!gateway::json::parse("{\"a\": }", root, error), "缺值被拒绝");
    check(error.find("第 1 行") != std::string::npos, "错误信息带行号：" + error);

    check(!gateway::json::parse("{\"a\": 1} extra", root, error), "尾部多余内容被拒绝");
    check(!gateway::json::parse("{\"a\": 1", root, error), "未闭合对象被拒绝");
    check(!gateway::json::parse("[1, 2", root, error), "未闭合数组被拒绝");
    check(!gateway::json::parse("{a: 1}", root, error), "键必须是字符串");

    // 多行输入时行号要正确累加。
    check(!gateway::json::parse("{\n  \"a\": 1,\n  \"b\": @\n}", root, error), "非法字符被拒绝");
    check(error.find("第 3 行") != std::string::npos, "跨行错误定位到第 3 行：" + error);
}

void testGatewayConfigValid() {
    section("网关配置：多设备与点位");

    const std::string text = R"({
      "devices": [
        {
          "name": "plc-01",
          "transport": "tcp",
          "host": "127.0.0.1",
          "port": 5020,
          "unitId": 1,
          "pollIntervalMs": 500,
          "timeoutMs": 1000,
          "retry": 3,
          "points": [ { "func": 3, "address": 0, "count": 10 } ]
        },
        {
          "name": "meter-02",
          "transport": "rtu",
          "serialPort": "COM11",
          "baudRate": 9600,
          "parity": "N",
          "dataBits": 8,
          "stopBits": 1,
          "unitId": 2,
          "points": [
            { "func": 4, "address": 0, "count": 4 },
            { "func": 6, "address": 8192, "count": 1, "value": 1 }
          ]
        }
      ],
      "storage": { "database": "data/records.db", "batchSize": 500 },
      "logging": { "level": "info", "rotateSizeMb": 32 }
    })";

    gateway::config::GatewayConfig config;
    std::string error;
    check(gateway::config::parseGatewayConfig(text, config, error), "解析成功：" + error);
    checkU64(config.devices.size(), 2, "解析出两台设备");

    const auto& plc = config.devices[0];
    check(plc.name == "plc-01", "设备名");
    check(plc.transport == gateway::transport::TransportKind::Tcp, "TCP 链路类型");
    check(plc.host == "127.0.0.1", "主机地址");
    checkU16(plc.port, 5020, "端口");
    checkU16(plc.unitId, 1, "从站地址");
    checkU16(plc.timeoutMs, 1000, "超时");
    checkU64(plc.retry, 3, "重传次数");
    checkU64(plc.points.size(), 1, "一个点位");
    checkU16(plc.points[0].functionCode, 0x03, "功能码 3");
    checkU16(plc.points[0].count, 10, "读取数量");

    const auto& meter = config.devices[1];
    check(meter.transport == gateway::transport::TransportKind::Serial, "RTU 链路类型");
    check(meter.serialPort == "COM11", "串口名");
    checkU64(meter.points.size(), 2, "两个点位");
    // 写点位：count 恒为 1，写入值在 value 字段。
    checkU16(meter.points[1].functionCode, 0x06, "写单寄存器功能码");
    checkU16(meter.points[1].count, 1, "写点位 count 恒为 1");
    checkU16(meter.points[1].value, 1, "写点位 value");

    check(config.storage.database == "data/records.db", "存储路径");
    checkU64(config.storage.batchSize, 500, "批量大小");
    check(config.logging.level == "info", "日志级别");
}

void testGatewayConfigDefaults() {
    section("网关配置：缺省值");

    const std::string text = R"({
      "devices": [ { "name": "minimal", "transport": "tcp" } ]
    })";
    gateway::config::GatewayConfig config;
    std::string error;
    check(gateway::config::parseGatewayConfig(text, config, error), "最简配置可解析：" + error);
    checkU64(config.devices.size(), 1, "一台设备");
    const auto& device = config.devices[0];
    check(device.host == "127.0.0.1", "默认主机");
    checkU16(device.port, 502, "默认端口");
    checkU16(device.unitId, 1, "默认从站地址");
    checkU16(device.timeoutMs, 1000, "默认超时");
    checkU64(device.retry, 3, "默认重传次数");
    checkU64(device.points.size(), 0, "默认可无点位");
    check(config.storage.database == "data/records.db", "默认数据库路径");
    check(config.logging.level == "info", "默认日志级别");
}

void testGatewayConfigRejects() {
    section("网关配置：非法输入被拒绝");

    gateway::config::GatewayConfig config;
    std::string error;

    check(!gateway::config::parseGatewayConfig("{}", config, error), "缺 devices 段");
    check(error.find("devices") != std::string::npos, "指明缺失字段：" + error);

    check(!gateway::config::parseGatewayConfig(R"({"devices": []})", config, error),
          "空设备列表被拒绝");
    check(error.find("不能为空") != std::string::npos, "指明原因：" + error);

    check(!gateway::config::parseGatewayConfig(
              R"({"devices": [{"transport": "tcp"}]})", config, error),
          "缺设备名的设备被拒绝");

    // 设备名重复：两个会话同名会导致按名字取会话时永远拿到第一个。
    check(!gateway::config::parseGatewayConfig(
              R"({"devices": [{"name": "dup"}, {"name": "dup"}]})", config, error),
          "设备名重复被拒绝");
    check(error.find("重复") != std::string::npos, "指明重复：" + error);

    check(!gateway::config::parseGatewayConfig(
              R"({"devices": [{"name": "x", "transport": "udp"}]})", config, error),
          "未知链路类型被拒绝");

    // 从站地址范围：0-247，超出永远不会被应答。
    check(!gateway::config::parseGatewayConfig(
              R"({"devices": [{"name": "x", "unitId": 300}]})", config, error),
          "从站地址超范围被拒绝");

    // 点位功能码：只支持 3 / 4 / 6。
    check(!gateway::config::parseGatewayConfig(
              R"({"devices": [{"name": "x", "points": [{"func": 7}]}]})", config, error),
          "未知功能码被拒绝");
    check(error.find("func") != std::string::npos, "指明 func 字段：" + error);

    // 读点位 count 为 0 无意义。
    check(!gateway::config::parseGatewayConfig(
              R"({"devices": [{"name": "x", "points": [{"func": 3, "count": 0}]}]})",
              config, error),
          "读点位 count 为 0 被拒绝");

    // 协议上限：读保持寄存器最多 125 个。
    check(!gateway::config::parseGatewayConfig(
              R"({"devices": [{"name": "x", "points": [{"func": 3, "count": 126}]}]})",
              config, error),
          "读数量超协议上限被拒绝");

    // parity 只能是 N / E / O。
    check(!gateway::config::parseGatewayConfig(
              R"({"devices": [{"name": "x", "parity": "X"}]})", config, error),
          "非法校验位被拒绝");
}

void testGatewayConfigRealFile() {
    section("网关配置：仓库内的 config/gateway.json 可解析");

    gateway::config::GatewayConfig config;
    std::string error;

    // 这个用例要读的是**仓库里真实的**示例配置，所以必须能定位到仓库根目录。
    //
    // 早期版本靠「当前工作目录」猜（先试 config/gateway.json 再试 ../config/gateway.json），
    // 在可移植脚本里能过（cwd = build_portable/），但 ctest 把工作目录设在
    // build/<cfg>/tests/portable/，两个候选路径都不对 —— 于是同一份代码
    // 「脚本里全绿、ctest 里红」。教训：测试对「自己在哪」的假设必须显式传入，
    // 不能靠 cwd 猜。
    std::vector<std::string> candidates;
    if (!g_repoRoot.empty()) {
        candidates.push_back(g_repoRoot + "/config/gateway.json");
    }
    // 手动运行的兜底：覆盖「从仓库根跑」与「从一级构建目录跑」两种常见位置。
    candidates.push_back("config/gateway.json");
    candidates.push_back("../config/gateway.json");
    candidates.push_back("../../config/gateway.json");

    bool ok = false;
    for (const std::string& candidate : candidates) {
        error.clear();
        if (gateway::config::loadGatewayConfig(candidate, config, error)) {
            ok = true;
            break;
        }
    }
    check(ok, "仓库配置可加载：" + error);
    if (ok) {
        checkU64(config.devices.size(), 3, "示例配置含三台设备");
        check(config.devices[0].name == "plc-01", "第一台设备名");
        check(config.devices[2].points.size() == 2, "第三台设备有两个点位");
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // ctest 与构建脚本都会把仓库根目录作为第一个参数传进来；
    // 不传时留空，由各用例退回相对路径兜底（见 testGatewayConfigRealFile）。
    if (argc > 1) {
        g_repoRoot = argv[1];
    }
    std::printf("Modbus 配置解析测试（网关配置与 JSON，不依赖 Qt）\n");

    testJsonBasics();
    testJsonErrors();
    testGatewayConfigValid();
    testGatewayConfigDefaults();
    testGatewayConfigRejects();
    testGatewayConfigRealFile();

    return test_support::summary();
}
