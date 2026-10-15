// ---------------------------------------------------------------------------
// gateway_daemon：无界面常驻采集进程（阶段 11）。
//
// 与 ui/MainWindow 的关系：
//   两者都是「配置文件 → DeviceManager → 采集 → 落库」这条链路的前端，
//   区别在于驱动方式。MainWindow 用 QTimer（依赖 Qt 事件循环），
//   daemon 用自带的单调时钟循环 —— 因此能在没有图形环境的服务器上跑，
//   也能被 systemd 直接托管。
//
// 为什么单独写成可执行文件而不是给 app 加 --headless：
//   加了 Qt 依赖的二进制即使不用界面也要链 Qt 库，部署体积与依赖面都会变大。
//   daemon 只链 gateway_core，产物是干净的纯 C++ 程序。
//
// 退出方式（三选一，都能优雅关闭并 flush 未落盘数据）：
//   1. 收到 SIGINT / SIGTERM（Ctrl-C 或 systemctl stop）；
//   2. --seconds N 到点自动退出（端到端脚本与 CI 用这个）；
//   3. 读写 /tmp/gateway-daemon.stop 之类的 --stop-file 被创建。
// ---------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "gateway/config/GatewayConfig.h"
#include "gateway/device/DeviceManager.h"
#include "gateway/device/DeviceSession.h"
#include "gateway/metrics/MetricsCollector.h"
#include "gateway/storage/SqliteStorage.h"
#include "gateway/transport/NativeTcpTransport.h"
#include "gateway/transport/TransportFactory.h"

namespace {

std::atomic<bool> g_stopRequested{false};

// ---------------------------------------------------------------------------
// 把原生 TCP 链路注册进链路注册表。
//
// Ruling（为什么 daemon 无条件用原生链路，而不是"有 Qt 就用 Qt"）：
//   采集进程的行为不应该取决于「这台机器恰好装没装 Qt」。
//   若按 Qt 有无切换实现，同一份 gateway.json 在两台机器上会走两套 socket 代码
//   （异步信号槽 vs 主动轮询），一旦只在一边复现出问题，排查成本极高。
//   所以 daemon 固定用 NativeTcpTransport，行为在任何构建下都一致；
//   副作用是 Qt 构建里 daemon 不再用 TcpTransport —— 这是刻意的取舍。
//   代价：Qt 版 TcpTransport 的改动不会自动惠及 daemon，需要同步。
// ---------------------------------------------------------------------------
std::unique_ptr<gateway::transport::TransportInterface> buildNativeTcp(
    const gateway::transport::TransportConfig& config, std::string& out_error) {
    out_error.clear();
    return std::make_unique<gateway::transport::NativeTcpTransport>(config);
}

bool nativeTcpAvailable(gateway::transport::TransportKind kind) {
    return kind == gateway::transport::TransportKind::Tcp;
}

void registerDaemonTransports() {
    gateway::transport::registerTransportBuilder(gateway::transport::TransportKind::Tcp,
                                                &buildNativeTcp, &nativeTcpAvailable);
}

extern "C" void handleSignal(int) {
    // 信号处理器里只做一件事：置标志位。
    // 打印、落库这些动作留给主循环 —— 在信号处理器里调用非可重入函数是未定义行为。
    g_stopRequested.store(true);
}

void printUsage() {
    std::printf(
        "Modbus 网关常驻采集进程（无界面）\n"
        "\n"
        "用法：\n"
        "  gateway_daemon [配置文件] [选项]\n"
        "\n"
        "选项：\n"
        "  --db <路径>        覆盖 SQLite 数据库路径\n"
        "  --seconds <秒>     运行指定秒数后自动退出（0 = 一直跑，默认）\n"
        "  --poll <毫秒>      覆盖所有设备的轮询间隔\n"
        "  --export-csv <路径> 退出前把本次库里的全部样本导出成 CSV\n"
        "  --stop-file <路径> 该文件出现即优雅退出（便于外部脚本控制）\n"
        "  --quiet            不逐轮打印指标\n"
        "  --dry-run          只解析配置并打印计划，不真正采集\n"
        "  --help             显示本帮助\n"
        "\n"
        "退出：Ctrl-C / SIGTERM / 到 --seconds 时间 / 出现 --stop-file 文件。\n");
}

struct Options {
    std::string configPath = "config/gateway.json";
    std::string databaseOverride;
    std::string stopFile;
    std::string exportCsvPath;
    long seconds = 0;
    long pollOverrideMs = 0;
    bool quiet = false;
    bool dryRun = false;
};

bool parseOptions(int argc, char** argv, Options& options, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        }
        if (arg == "--quiet") {
            options.quiet = true;
            continue;
        }
        if (arg == "--dry-run") {
            options.dryRun = true;
            continue;
        }
        if (arg == "--db" || arg == "--seconds" || arg == "--poll" || arg == "--stop-file" ||
            arg == "--export-csv") {
            if (i + 1 >= argc) {
                error = arg + " 后面需要跟一个值";
                return false;
            }
            const std::string value = argv[++i];
            if (arg == "--db") {
                options.databaseOverride = value;
            } else if (arg == "--stop-file") {
                options.stopFile = value;
            } else if (arg == "--export-csv") {
                options.exportCsvPath = value;
            } else {
                char* end = nullptr;
                const long parsed = std::strtol(value.c_str(), &end, 10);
                if (end == nullptr || *end != '\0' || parsed < 0) {
                    error = arg + " 需要非负整数，收到：" + value;
                    return false;
                }
                if (arg == "--seconds") {
                    options.seconds = parsed;
                } else {
                    options.pollOverrideMs = parsed;
                }
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

bool fileExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
#else
    struct stat buffer;
    return ::stat(path.c_str(), &buffer) == 0;
#endif
}

// 单调时钟。用 steady_clock 而不是 system_clock：
// 现场机器可能被 NTP 校时，system_clock 会跳变，一跳就会把超时/轮询时间算错。
uint64_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
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

    // ---- 1. 读配置 ----
    registerDaemonTransports();

    gateway::config::GatewayConfig config;
    if (!gateway::config::loadGatewayConfig(options.configPath, config, error)) {
        std::printf("配置读取失败：%s\n", error.c_str());
        std::printf("提示：可以从仓库根目录运行，或显式传入配置文件路径。\n");
        return 1;
    }

    if (!options.databaseOverride.empty()) {
        config.storage.database = options.databaseOverride;
    }
    if (options.pollOverrideMs > 0) {
        for (auto& device : config.devices) {
            device.pollIntervalMs = static_cast<uint16_t>(options.pollOverrideMs);
        }
    }

    std::printf("Modbus 网关常驻采集进程\n");
    std::printf("  配置文件  : %s\n", options.configPath.c_str());
    std::printf("  数据库    : %s（批量 %zu）\n", config.storage.database.c_str(),
                config.storage.batchSize);
    std::printf("  设备      : %zu 台\n", config.devices.size());
    for (const auto& device : config.devices) {
        std::printf("    - %-12s %-4s 轮询 %ums 超时 %ums 重试 %u 次 点位 %zu 个\n",
                    device.name.c_str(), gateway::transport::toString(device.transport),
                    static_cast<unsigned>(device.pollIntervalMs),
                    static_cast<unsigned>(device.timeoutMs), static_cast<unsigned>(device.retry),
                    device.points.size());
    }
    std::printf("  日志等级  : %s（单文件上限 %zu MB）\n", config.logging.level.c_str(),
                config.logging.rotateSizeMb);

    // 预先检查每台设备的链路是否可用。串口不存在、平台不支持都要在启动时就说清楚，
    // 而不是等到第一次轮询才失败 —— 现场排查最怕「启动了但什么也没发生」。
    //
    // 这里用**注册表**而不是 createTransport：后者所在的翻译单元依赖 Qt，
    // 而 daemon 的目标就是在没有 Qt 的机器上也能跑。有 Qt 的构建会在 main()
    // 之前自动把 Qt 链路注册进来（见 TransportFactory.cpp），因此两者行为一致。
    bool allAvailable = true;
    for (const auto& device : config.devices) {
        if (!gateway::transport::isRegisteredTransportAvailable(device.transport)) {
            std::printf("  [不可用] %s：当前构建不支持 %s 链路\n", device.name.c_str(),
                        gateway::transport::toString(device.transport));
            allAvailable = false;
        }
    }
    if (!allAvailable) {
        std::printf("有设备链路不可用。确认后可用 --dry-run 只做配置检查。\n");
    }

    if (options.dryRun) {
        std::printf("\n--dry-run：配置解析完成，未进行采集。\n");
        return allAvailable ? 0 : 1;
    }

    // ---- 2. 打开存储 ----
    gateway::storage::SqliteStorage storage(config.storage.database, config.storage.batchSize);
    if (!storage.open(error)) {
        std::printf("打开数据库失败：%s\n", error.c_str());
        return 1;
    }
    std::printf("  数据库已就绪：%s\n", storage.describe().c_str());

    // ---- 3. 建管理器和设备 ----
    gateway::device::DeviceManager manager;
    gateway::metrics::Collector metrics(1000);

    for (const auto& deviceConfig : config.devices) {
        std::string createError;
        auto transport = gateway::transport::createRegisteredTransport(
            deviceConfig.toTransportConfig(), createError);
        if (!transport) {
            std::printf("[跳过] %s：%s\n", deviceConfig.name.c_str(), createError.c_str());
            continue;
        }
        gateway::device::DeviceSession* session = manager.addDevice(deviceConfig, std::move(transport));
        if (session != nullptr) {
            session->setAutoPoll(true);
        }
    }

    // 结果回调：写存储 + 记指标。
    // 这里只做「收数据」这一件事 —— 打印、告警之类都放到循环里按节奏做，
    // 否则每来一条结果就 printf 一次，高轮询率下控制台会成为瓶颈。
    uint64_t sampled = 0;
    manager.setResultCallback(
        [&](const std::string& deviceName, const gateway::device::TransactionResult& result) {
            gateway::metrics::Observation observation;
            observation.success = result.valid;
            observation.timedOut = result.timedOut;
            observation.exception = result.exception != gateway::protocol::ExceptionCode::None;
            observation.latencyMs = result.latencyMs;
            observation.address = result.address;
            observation.functionCode = result.functionCode;
            metrics.record(observation);

            std::vector<gateway::storage::Sample> batch;
            batch.reserve(result.values.size());
            const uint64_t timestamp = nowMs();
            for (std::size_t i = 0; i < result.values.size(); ++i) {
                gateway::storage::Sample sample;
                sample.deviceName = deviceName;
                sample.timestampMs = timestamp;
                sample.address = static_cast<uint16_t>(result.address + i);
                sample.value = result.values[i];
                sample.functionCode = result.functionCode;
                sample.valid = result.valid;
                if (!result.valid) {
                    sample.note = result.error;
                }
                batch.push_back(std::move(sample));
            }
            if (batch.empty() && !result.valid) {
                // 失败的单个点位（写入回显）也要留痕，否则「设备一直失败」在库里查不到。
                gateway::storage::Sample sample;
                sample.deviceName = deviceName;
                sample.timestampMs = timestamp;
                sample.address = result.address;
                sample.value = 0;
                sample.functionCode = result.functionCode;
                sample.valid = false;
                sample.note = result.error;
                batch.push_back(std::move(sample));
            }
            if (!batch.empty()) {
                storage.write(batch);
                sampled += batch.size();
            }
        });

    manager.setLinkCallback(
        [](const std::string& deviceName, gateway::device::DeviceSession::LinkEvent event,
           const std::string& detail) {
            const char* label = "未知";
            switch (event) {
                case gateway::device::DeviceSession::LinkEvent::Down:
                    label = "链路断开";
                    break;
                case gateway::device::DeviceSession::LinkEvent::Reconnecting:
                    label = "正在重连";
                    break;
                case gateway::device::DeviceSession::LinkEvent::Recovered:
                    label = "链路恢复";
                    break;
            }
            std::printf("[%s] %s：%s\n", label, deviceName.c_str(), detail.c_str());
            std::fflush(stdout);
        });

    if (!manager.startAll()) {
        std::printf("启动设备失败（部分设备可能没有起来），继续运行并等待自愈。\n");
    }

    // ---- 4. 信号处理 ----
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // ---- 5. 主循环 ----
    const uint64_t startMs = nowMs();
    const uint64_t tickIntervalMs = 50;   // 时钟推进粒度，比最小轮询间隔小一个量级
    const uint64_t reportIntervalMs = 5000;
    uint64_t nextReportAtMs = startMs + reportIntervalMs;
    uint64_t ticks = 0;

    std::printf("\n开始采集。Ctrl-C 退出。\n\n");
    std::fflush(stdout);

    while (!g_stopRequested.load()) {
        const uint64_t now = nowMs();
        // 顺序不能反：先 pump() 把链路里的字节取出来交给会话，再 tick() 推进时钟
        // （判定超时、发起下一次轮询）。反过来会让「刚好在本轮到达的响应」
        // 被当成本轮超时处理，白白多一次重传。
        manager.pump();
        manager.tick(now);
        ++ticks;

        if (!options.quiet && now >= nextReportAtMs) {
            const auto snapshot = metrics.snapshot();
            std::printf("[%5.1fs] %s\n", static_cast<double>(now - startMs) / 1000.0,
                        metrics.describe().c_str());
            std::printf("        已采集 %llu 个样本，待落盘 %zu 条\n",
                        static_cast<unsigned long long>(sampled), storage.pendingCount());
            (void)snapshot;
            std::fflush(stdout);
            nextReportAtMs = now + reportIntervalMs;
        }

        if (options.seconds > 0 && now - startMs >= static_cast<uint64_t>(options.seconds) * 1000) {
            std::printf("\n已运行 %ld 秒，按计划退出。\n", options.seconds);
            break;
        }
        if (fileExists(options.stopFile)) {
            std::printf("\n检测到停止文件 %s，退出。\n", options.stopFile.c_str());
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(tickIntervalMs));
    }

    // ---- 6. 优雅退出：先停设备，再 flush，最后关库 ----
    // 顺序很重要：stopAll 会停掉自动轮询与重连，之后不会再有新数据进来，
    // 这时 flush 才能保证「落盘的内容 == 采集到的内容」。
    std::printf("\n正在停止设备...\n");
    manager.stopAll();

    std::printf("正在落盘剩余数据...\n");
    const auto flushed = storage.flush();
    if (!flushed.ok) {
        std::printf("落盘失败：%s\n", flushed.error.c_str());
    }

    std::printf("\n运行统计\n");
    std::printf("  时钟推进次数 : %llu\n", static_cast<unsigned long long>(ticks));
    std::printf("  采集样本     : %llu\n", static_cast<unsigned long long>(sampled));
    std::printf("%s\n", metrics.describe().c_str());
    for (const auto& name : manager.deviceNames()) {
        const auto* session = manager.session(name);
        if (session == nullptr) {
            continue;
        }
        const auto& stats = session->stats();
        std::printf("  %-12s 请求 %llu / 响应 %llu / 超时 %llu / 重传 %llu / "
                    "重连 %llu(尝试 %llu) / CRC 错 %llu / 异常 %llu\n",
                    name.c_str(), static_cast<unsigned long long>(stats.requests),
                    static_cast<unsigned long long>(stats.responses),
                    static_cast<unsigned long long>(stats.timeouts),
                    static_cast<unsigned long long>(stats.retries),
                    static_cast<unsigned long long>(stats.reconnects),
                    static_cast<unsigned long long>(stats.reconnectAttempts),
                    static_cast<unsigned long long>(stats.crcErrors),
                    static_cast<unsigned long long>(stats.exceptions));
    }

    const auto total = storage.totalSamples();

    // ---- 7. 可选：导出 CSV ----
    // 放在 close() 之前：查询要用的连接还在手上，导出完再关。
    // 导出的是「库里现有的全部样本」，而不是只在本次进程内采集的 ——
    // 现场常见的用法是「接着昨天的库继续采，收工时导一份全量给同事」。
    if (!options.exportCsvPath.empty()) {
        // 上界用 int64 的最大值而不是 uint64 的最大值：
        // 查询里 ts_ms 会被绑成 sqlite3_int64（有符号），传 0xFFFF...FF 会变成 -1，
        // 于是 `ts_ms <= -1` 恒假、导出一个空文件 —— 这种错很难从结果上看出来。
        constexpr uint64_t kMaxTimestampMs = 0x7FFFFFFFFFFFFFFFull;
        const auto all = storage.query("", 0, kMaxTimestampMs, 0);
        std::string exportError;
        if (gateway::storage::exportCsv(options.exportCsvPath, all, exportError)) {
            std::printf("已导出 %zu 条样本到 %s\n", all.size(), options.exportCsvPath.c_str());
        } else {
            std::printf("导出 CSV 失败：%s\n", exportError.c_str());
        }
    }

    storage.close();
    std::printf("数据库共 %zu 条样本，已关闭。\n", total);
    return 0;
}
