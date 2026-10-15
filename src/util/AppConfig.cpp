#include "gateway/util/AppConfig.h"
#include "gateway/util/Logger.h"
#include "nlohmann/json.hpp"
#include <fstream>

using json = nlohmann::json;

namespace gateway::util {

gateway::device::GatewayAppConfig AppConfig::createDefault() {
    gateway::device::GatewayAppConfig cfg;

    gateway::device::DeviceConfig dev1;
    dev1.name = "plc-01";
    dev1.transport = "tcp";
    dev1.host = "127.0.0.1";
    dev1.port = 5020;
    dev1.unitId = 1;
    dev1.pollIntervalMs = 500;
    dev1.timeoutMs = 1000;
    dev1.retryCount = 3;
    dev1.points.push_back({3, 0, 10});
    cfg.devices.push_back(dev1);

    gateway::device::DeviceConfig dev2;
    dev2.name = "meter-02";
    dev2.transport = "rtu";
    dev2.serialPort = "COM11";
    dev2.baudRate = 9600;
    dev2.parity = 'N';
    dev2.dataBits = 8;
    dev2.stopBits = 1;
    dev2.unitId = 2;
    dev2.pollIntervalMs = 1000;
    dev2.timeoutMs = 1000;
    dev2.retryCount = 3;
    dev2.points.push_back({4, 0, 4});
    cfg.devices.push_back(dev2);

    cfg.storage.database = "data/records.db";
    cfg.storage.batchSize = 500;

    cfg.logging.level = "info";
    cfg.logging.file = "logs/gateway.log";
    cfg.logging.rotateSizeMb = 32;

    return cfg;
}

bool AppConfig::loadFromFile(const std::string& path, gateway::device::GatewayAppConfig& outConfig) {
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_WARN("Config file " << path << " not found, using default configuration");
        outConfig = createDefault();
        return false;
    }

    try {
        json j;
        file >> j;

        outConfig.devices.clear();
        if (j.contains("devices") && j["devices"].is_array()) {
            for (const auto& d : j["devices"]) {
                gateway::device::DeviceConfig dev;
                dev.name = d.value("name", "dev");
                dev.transport = d.value("transport", "tcp");
                dev.host = d.value("host", "127.0.0.1");
                dev.port = static_cast<uint16_t>(d.value("port", 5020));
                dev.serialPort = d.value("serialPort", "COM1");
                dev.baudRate = d.value("baudRate", 9600);
                std::string p = d.value("parity", "N");
                dev.parity = p.empty() ? 'N' : p[0];
                dev.dataBits = d.value("dataBits", 8);
                dev.stopBits = d.value("stopBits", 1);
                dev.unitId = static_cast<uint8_t>(d.value("unitId", 1));
                dev.pollIntervalMs = d.value("pollIntervalMs", 500);
                dev.timeoutMs = d.value("timeoutMs", 1000);
                dev.retryCount = d.value("retry", 3);

                if (d.contains("points") && d["points"].is_array()) {
                    for (const auto& pt : d["points"]) {
                        gateway::device::DataPointConfig point;
                        point.func = static_cast<uint8_t>(pt.value("func", 3));
                        point.address = static_cast<uint16_t>(pt.value("address", 0));
                        point.count = static_cast<uint16_t>(pt.value("count", 1));
                        dev.points.push_back(point);
                    }
                }
                outConfig.devices.push_back(dev);
            }
        }

        if (j.contains("storage")) {
            outConfig.storage.database = j["storage"].value("database", "data/records.db");
            outConfig.storage.batchSize = j["storage"].value("batchSize", 500);
        }

        if (j.contains("logging")) {
            outConfig.logging.level = j["logging"].value("level", "info");
            outConfig.logging.file = j["logging"].value("file", "logs/gateway.log");
            outConfig.logging.rotateSizeMb = j["logging"].value("rotateSizeMb", 32);
        }

        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("Failed to parse config file " << path << ": " << ex.what());
        outConfig = createDefault();
        return false;
    }
}

bool AppConfig::saveToFile(const std::string& path, const gateway::device::GatewayAppConfig& config) {
    try {
        json j;
        j["devices"] = json::array();
        for (const auto& dev : config.devices) {
            json d;
            d["name"] = dev.name;
            d["transport"] = dev.transport;
            if (dev.transport == "tcp") {
                d["host"] = dev.host;
                d["port"] = dev.port;
            } else {
                d["serialPort"] = dev.serialPort;
                d["baudRate"] = dev.baudRate;
                d["parity"] = std::string(1, dev.parity);
                d["dataBits"] = dev.dataBits;
                d["stopBits"] = dev.stopBits;
            }
            d["unitId"] = dev.unitId;
            d["pollIntervalMs"] = dev.pollIntervalMs;
            d["timeoutMs"] = dev.timeoutMs;
            d["retry"] = dev.retryCount;
            d["points"] = json::array();
            for (const auto& pt : dev.points) {
                json p;
                p["func"] = pt.func;
                p["address"] = pt.address;
                p["count"] = pt.count;
                d["points"].push_back(p);
            }
            j["devices"].push_back(d);
        }

        j["storage"]["database"] = config.storage.database;
        j["storage"]["batchSize"] = config.storage.batchSize;

        j["logging"]["level"] = config.logging.level;
        j["logging"]["file"] = config.logging.file;
        j["logging"]["rotateSizeMb"] = config.logging.rotateSizeMb;

        std::ofstream file(path);
        if (!file.is_open()) return false;
        file << j.dump(2);
        return true;
    } catch (const std::exception& ex) {
        LOG_ERROR("Failed to save config file " << path << ": " << ex.what());
        return false;
    }
}

} // namespace gateway::util
