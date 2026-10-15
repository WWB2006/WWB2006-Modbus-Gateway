#include "gateway/util/AppConfig.h"
#include "gateway/util/Logger.h"
#include "gateway/util/Metrics.h"
#include "gateway/device/DeviceManager.h"
#include "gateway/device/PollScheduler.h"
#include "gateway/storage/SqliteStorage.h"
#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>
#include <iomanip>

using namespace gateway;

static volatile bool s_running = true;

void handleSignal(int) {
    s_running = false;
}

int main(int argc, char* argv[]) {
    std::string configPath = "config/gateway.json";
    std::string dbOverride;
    int metricsIntervalSec = 5;
    int benchDurationSec = 0;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-c" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            dbOverride = argv[++i];
        } else if (arg == "--metrics-interval" && i + 1 < argc) {
            metricsIntervalSec = std::stoi(argv[++i]);
        } else if (arg == "--bench" && i + 1 < argc) {
            benchDurationSec = std::stoi(argv[++i]);
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: gateway_cli [options]\n"
                      << "Options:\n"
                      << "  -c <config.json>         Configuration file path (default: config/gateway.json)\n"
                      << "  --db <records.db>        Override SQLite database output path\n"
                      << "  --metrics-interval <sec> Metrics display interval in seconds (default: 5)\n"
                      << "  --bench <seconds>        Run benchmark for specified seconds then exit\n"
                      << "  -v, --verbose            Enable DEBUG level logging\n"
                      << "  -h, --help               Show this help message\n";
            return 0;
        }
    }

    if (verbose) {
        util::Logger::instance().setLevel(util::LogLevel::DEBUG);
    } else {
        util::Logger::instance().setLevel(util::LogLevel::INFO);
    }

    std::cout << "========================================================\n"
              << "       Modbus Industrial Gateway Daemon (CLI Mode)      \n"
              << "========================================================\n";

    device::GatewayAppConfig appConfig;
    if (!util::AppConfig::loadFromFile(configPath, appConfig)) {
        LOG_INFO("Using generated default configuration");
    }

    if (!dbOverride.empty()) {
        appConfig.storage.database = dbOverride;
    }

    if (!appConfig.logging.file.empty()) {
        util::Logger::instance().setOutputFile(appConfig.logging.file);
    }

    storage::SqliteStorage storage(appConfig.storage.database, appConfig.storage.batchSize);
    if (!storage.init()) {
        LOG_ERROR("Failed to initialize database storage: " << appConfig.storage.database);
        return 1;
    }

    device::DeviceManager devManager;
    device::PollScheduler scheduler(devManager, storage);

    scheduler.setRecordCallback([](const storage::CommunicationRecord& rec) {
        if (rec.status != "OK") {
            LOG_WARN("Point poll error: " << rec.deviceName << " Func=" << static_cast<int>(rec.functionCode)
                     << " Addr=" << rec.address << " -> " << rec.status);
        }
    });

    scheduler.setStateCallback([](const std::string& devName, device::DeviceState state) {
        LOG_INFO("Device state changed: " << devName << " -> " << device::deviceStateToString(state));
    });

    if (!scheduler.start(appConfig.devices)) {
        LOG_ERROR("Failed to start poll scheduler");
        return 1;
    }

    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    auto startTime = std::chrono::steady_clock::now();
    auto lastMetricsTime = startTime;

    std::cout << "[GATEWAY STARTED] Polling " << appConfig.devices.size() << " devices. Press Ctrl+C to terminate.\n";

    while (s_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        auto now = std::chrono::steady_clock::now();
        if (benchDurationSec > 0) {
            auto elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
            if (elapsedSec >= benchDurationSec) {
                std::cout << "\n[BENCHMARK COMPLETE] " << benchDurationSec << " seconds reached.\n";
                break;
            }
        }

        auto elapsedSinceMetrics = std::chrono::duration_cast<std::chrono::seconds>(now - lastMetricsTime).count();
        if (elapsedSinceMetrics >= metricsIntervalSec) {
            lastMetricsTime = now;
            auto snap = util::Metrics::instance().snapshot();
            std::cout << ">>> [METRICS] " << snap.summary() << std::endl;
        }
    }

    std::cout << "\nStopping gateway scheduler...\n";
    scheduler.stop();
    storage.flush();
    storage.close();

    auto finalMetrics = util::Metrics::instance().snapshot();
    std::cout << "========================================================\n"
              << " Final Metrics Summary:\n"
              << "   Total Requests:      " << finalMetrics.totalRequests << "\n"
              << "   Successful Requests: " << finalMetrics.successfulRequests << "\n"
              << "   Failed Requests:     " << finalMetrics.failedRequests << "\n"
              << "   Timeouts:            " << finalMetrics.timeoutCount << "\n"
              << "   CRC Errors:          " << finalMetrics.crcErrorCount << "\n"
              << "   Modbus Exceptions:   " << finalMetrics.exceptionCount << "\n"
              << "   Reconnection Events: " << finalMetrics.reconnectCount << "\n"
              << "   Success Rate:        " << std::fixed << std::setprecision(2) << finalMetrics.successRate() << "%\n"
              << "   Latency P50:         " << finalMetrics.p50LatencyMs << " ms\n"
              << "   Latency P90:         " << finalMetrics.p90LatencyMs << " ms\n"
              << "   Latency P95:         " << finalMetrics.p95LatencyMs << " ms\n"
              << "   Latency P99:         " << finalMetrics.p99LatencyMs << " ms\n"
              << "   Latency Average:     " << finalMetrics.avgLatencyMs << " ms\n"
              << "========================================================\n";

    return 0;
}
