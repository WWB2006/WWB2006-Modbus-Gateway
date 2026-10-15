#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace gateway::device {

struct DataPointConfig {
    uint8_t func = 3;       // Modbus Function Code (1, 2, 3, 4, etc.)
    uint16_t address = 0;   // Starting address (0-based)
    uint16_t count = 1;     // Register / Coil count
};

struct DeviceConfig {
    std::string name = "plc-01";
    std::string transport = "tcp"; // "tcp" or "rtu"

    // TCP parameters
    std::string host = "127.0.0.1";
    uint16_t port = 5020;

    // RTU parameters
    std::string serialPort = "COM1";
    int baudRate = 9600;
    char parity = 'N'; // 'N', 'E', 'O'
    int dataBits = 8;
    int stopBits = 1;

    // Modbus parameters
    uint8_t unitId = 1;
    int pollIntervalMs = 500;
    int timeoutMs = 1000;
    int retryCount = 3;

    std::vector<DataPointConfig> points;
};

struct StorageConfig {
    std::string database = "data/records.db";
    size_t batchSize = 500;
};

struct LoggingConfig {
    std::string level = "info";
    std::string file = "logs/gateway.log";
    size_t rotateSizeMb = 32;
};

struct GatewayAppConfig {
    std::vector<DeviceConfig> devices;
    StorageConfig storage;
    LoggingConfig logging;
};

} // namespace gateway::device
