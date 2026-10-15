#include "gateway/sim/SlaveSimulator.h"
#include "gateway/util/Logger.h"
#include <iostream>
#include <csignal>
#include <chrono>
#include <thread>

using namespace gateway::sim;

static volatile bool s_stop = false;

void signalHandler(int) {
    s_stop = true;
}

int main(int argc, char* argv[]) {
    uint16_t port = 5020;
    uint8_t unitId = 1;
    std::string faultProfilePath;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "-u" && i + 1 < argc) {
            unitId = static_cast<uint8_t>(std::stoi(argv[++i]));
        } else if (arg == "-f" && i + 1 < argc) {
            faultProfilePath = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: slave_simulator [options]\n"
                      << "Options:\n"
                      << "  -p <port>             TCP port to listen on (default 5020)\n"
                      << "  -u <unitId>           Modbus Unit ID (default 1)\n"
                      << "  -f <profile.json>     Path to fault profile JSON\n"
                      << "  -h, --help            Show this help message\n";
            return 0;
        }
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    LOG_INFO("Starting Modbus Slave Simulator on port " << port << ", Unit " << static_cast<int>(unitId));
    SlaveSimulator simulator(unitId, port);

    if (!faultProfilePath.empty()) {
        if (simulator.getFaultInjector().loadProfileFromFile(faultProfilePath)) {
            LOG_INFO("Loaded fault injection profile: " << faultProfilePath);
        } else {
            LOG_WARN("Could not load fault profile: " << faultProfilePath);
        }
    }

    if (!simulator.startTcpServer()) {
        LOG_ERROR("Failed to start TCP server on port " << port);
        return 1;
    }

    std::cout << "[SIMULATOR RUNNING] Ready on port " << port << ". Press Ctrl+C to stop." << std::endl;

    while (!s_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    simulator.stopTcpServer();
    LOG_INFO("Slave Simulator terminated cleanly. Total requests served: " << simulator.getProcessedRequests());
    return 0;
}
