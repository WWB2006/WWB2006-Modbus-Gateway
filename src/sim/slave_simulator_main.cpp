// 从站模拟器入口（阶段 3）。
//
// 独立成一个可执行文件的原因见 SlaveSimulator.h 的说明：测试脚本需要反复启停它，
// 并且它必须能走真实 socket，才能复现粘包、半包、断连这类只在网络上传才出现的问题。
//
// 用法：
//   slave_simulator                          # 用 config/slave-simulator.json
//   slave_simulator path/to/config.json      # 指定配置
//   slave_simulator --port 5021 --unit 2     # 临时覆盖端口与从站地址
//   slave_simulator --quiet                  # 不逐条打印请求（性能测试用）

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "gateway/sim/RegisterMap.h"
#include "gateway/sim/SimulatorConfig.h"
#include "gateway/sim/SlaveSimulator.h"

namespace {

const char* kDefaultConfig = "config/slave-simulator.json";

void printUsage() {
    std::printf(
        "Modbus 从站模拟器（阶段 3）\n"
        "\n"
        "用法：\n"
        "  slave_simulator [配置文件] [选项]\n"
        "\n"
        "选项：\n"
        "  --port <端口>    覆盖监听端口（默认取配置文件，5020）\n"
        "  --bind <地址>    覆盖监听地址（默认 127.0.0.1）\n"
        "  --unit <地址>    覆盖从站地址（默认 1）\n"
        "  --quiet          不逐条打印请求与响应\n"
        "  --help           显示本帮助\n");
}

// 命令行解析结果：只保留「是否给了」与「给了什么」，配置合并留给调用方。
struct Options {
    std::string configPath = kDefaultConfig;
    bool hasPort = false;
    bool hasBind = false;
    bool hasUnit = false;
    bool hasVerbose = false;
    long port = 0;
    std::string bind;
    long unit = 0;
    bool verbose = true;
};

// 解析成功返回 true；失败时把原因写进 error。
bool parseOptions(int argc, char** argv, Options& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        }
        if (arg == "--quiet") {
            options.hasVerbose = true;
            options.verbose = false;
            continue;
        }
        if (arg == "--port" || arg == "--bind" || arg == "--unit") {
            if (i + 1 >= argc) {
                error = arg + " 后面需要跟一个值";
                return false;
            }
            const std::string value = argv[++i];
            if (arg == "--port") {
                char* end = nullptr;
                const long parsed = std::strtol(value.c_str(), &end, 10);
                if (end == nullptr || *end != '\0' || parsed < 1 || parsed > 65535) {
                    error = "端口必须是 1-65535 之间的整数，收到：" + value;
                    return false;
                }
                options.hasPort = true;
                options.port = parsed;
            } else if (arg == "--unit") {
                char* end = nullptr;
                const long parsed = std::strtol(value.c_str(), &end, 10);
                if (end == nullptr || *end != '\0' || parsed < 0 || parsed > 255) {
                    error = "从站地址必须是 0-255 之间的整数，收到：" + value;
                    return false;
                }
                options.hasUnit = true;
                options.unit = parsed;
            } else {
                options.hasBind = true;
                options.bind = value;
            }
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            error = "无法识别的选项 " + arg + "（用 --help 查看用法）";
            return false;
        }
        options.configPath = arg;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error)) {
        std::printf("参数错误：%s\n", error.c_str());
        return 2;
    }

    gateway::sim::SimulatorConfig config;
    if (!gateway::sim::loadSimulatorConfig(options.configPath, config, error)) {
        std::printf("配置读取失败：%s\n", error.c_str());
        std::printf("提示：可以从仓库根目录运行，或显式传入配置文件路径。\n");
        return 1;
    }

    // 命令行覆盖配置文件：现场调试时改端口不该需要改文件。
    if (options.hasPort) {
        config.port = static_cast<uint16_t>(options.port);
    }
    if (options.hasBind) {
        config.bindAddress = options.bind;
    }
    if (options.hasUnit) {
        config.unitId = static_cast<uint8_t>(options.unit);
    }
    if (options.hasVerbose) {
        config.verbose = options.verbose;
    }

    gateway::sim::RegisterMap map;
    gateway::sim::applyTo(map, config);

    gateway::sim::SlaveSimulator::Config simConfig;
    simConfig.bindAddress = config.bindAddress;
    simConfig.port = config.port;
    simConfig.unitId = config.unitId;
    simConfig.supportBroadcast = config.supportBroadcast;
    simConfig.verbose = config.verbose;

    gateway::sim::SlaveSimulator simulator(map, simConfig);
    if (!simulator.start()) {
        std::printf("启动失败：%s\n", simulator.error().c_str());
        return 1;
    }

    std::printf("Modbus 从站模拟器已启动\n");
    std::printf("  监听      : %s:%u\n", config.bindAddress.c_str(),
                static_cast<unsigned>(config.port));
    std::printf("  从站地址  : %u\n", static_cast<unsigned>(config.unitId));
    std::printf("  配置文件  : %s\n", options.configPath.c_str());
    std::printf("  数据区    :\n%s", map.describe().c_str());
    std::printf("\n可直接用 pymodbus / QModMaster / mbpoll 连接验证。Ctrl-C 退出。\n\n");
    std::fflush(stdout);

    simulator.run();

    const auto& stats = simulator.stats();
    std::printf("\n已停止。共接受连接 %llu 个，处理请求 %llu 条，"
                "其中异常响应 %llu 条，忽略 %llu 条，丢弃非法帧 %llu 条。\n",
                static_cast<unsigned long long>(stats.acceptedConnections),
                static_cast<unsigned long long>(stats.requestsHandled),
                static_cast<unsigned long long>(stats.exceptionResponses),
                static_cast<unsigned long long>(stats.ignoredRequests),
                static_cast<unsigned long long>(stats.malformedFrames));
    return 0;
}
