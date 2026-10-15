#include "gateway/device/DeviceManager.h"
#include "gateway/transport/TcpTransport.h"
#include "gateway/transport/SerialTransport.h"
#include "gateway/protocol/ModbusCodec.h"
#include "gateway/util/Logger.h"
#include "gateway/util/Metrics.h"
#include <chrono>
#include <sstream>

namespace gateway::device {

DeviceManager::DeviceManager() = default;

DeviceManager::~DeviceManager() {
    clear();
}

std::unique_ptr<transport::TransportInterface> DeviceManager::createTransport(const DeviceConfig& cfg) {
    if (cfg.transport == "tcp") {
        return std::make_unique<transport::TcpTransport>(cfg.host, cfg.port, cfg.timeoutMs);
    } else {
        return std::make_unique<transport::SerialTransport>(
            cfg.serialPort, cfg.baudRate, cfg.parity, cfg.dataBits, cfg.stopBits);
    }
}

bool DeviceManager::addDevice(const DeviceConfig& config) {
    std::lock_guard<std::mutex> lock(m_managerMutex);
    auto dev = std::make_shared<ManagedDevice>();
    dev->config = config;
    dev->transport = createTransport(config);
    dev->reconnectPolicy = ReconnectPolicy(1000, 30000, config.retryCount);
    dev->state = DeviceState::DISCONNECTED;
    m_devices[config.name] = dev;
    LOG_INFO("Device registered: " << config.name << " (" << config.transport << ")");
    return true;
}

bool DeviceManager::removeDevice(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_managerMutex);
    auto it = m_devices.find(name);
    if (it != m_devices.end()) {
        if (it->second->transport) {
            it->second->transport->disconnect();
        }
        m_devices.erase(it);
        LOG_INFO("Device removed: " << name);
        return true;
    }
    return false;
}

void DeviceManager::clear() {
    std::lock_guard<std::mutex> lock(m_managerMutex);
    for (auto& pair : m_devices) {
        if (pair.second->transport) {
            pair.second->transport->disconnect();
        }
    }
    m_devices.clear();
}

std::vector<std::string> DeviceManager::getDeviceNames() {
    std::lock_guard<std::mutex> lock(m_managerMutex);
    std::vector<std::string> names;
    names.reserve(m_devices.size());
    for (const auto& pair : m_devices) {
        names.push_back(pair.first);
    }
    return names;
}

DeviceState DeviceManager::getDeviceState(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_managerMutex);
    auto it = m_devices.find(name);
    if (it != m_devices.end()) {
        return it->second->state;
    }
    return DeviceState::DISCONNECTED;
}

bool DeviceManager::pollPoint(const std::string& deviceName, const DataPointConfig& point, storage::CommunicationRecord& outRecord) {
    std::shared_ptr<ManagedDevice> dev;
    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        auto it = m_devices.find(deviceName);
        if (it == m_devices.end()) return false;
        dev = it->second;
    }

    std::lock_guard<std::mutex> devLock(dev->deviceMutex);

    outRecord.timestampMs = storage::CommunicationRecord::currentTimestampMs();
    outRecord.deviceName = deviceName;
    outRecord.unitId = dev->config.unitId;
    outRecord.functionCode = point.func;
    outRecord.address = point.address;
    outRecord.count = point.count;

    int64_t nowMs = outRecord.timestampMs;

    // Check connection & reconnect backoff
    if (!dev->transport->isConnected()) {
        if (nowMs < dev->nextReconnectTimeMs) {
            outRecord.status = "DISCONNECTED_BACKOFF";
            outRecord.latencyMs = 0;
            return false;
        }

        dev->state = DeviceState::CONNECTING;
        LOG_INFO("Attempting reconnection to " << deviceName << " (" << dev->transport->getEndpointInfo() << ")...");
        if (!dev->transport->connect()) {
            int backoffDelay = dev->reconnectPolicy.nextDelayMs();
            if (backoffDelay < 0) {
                backoffDelay = 30000; // Cap at 30s
            }
            dev->nextReconnectTimeMs = nowMs + backoffDelay;
            dev->state = DeviceState::ERROR_STATE;
            util::Metrics::instance().recordReconnect();
            util::Metrics::instance().recordTimeout();
            outRecord.status = "CONNECT_FAILED";
            outRecord.latencyMs = 0;
            LOG_WARN("Reconnection to " << deviceName << " failed. Next retry in " << backoffDelay << "ms");
            return false;
        }

        dev->state = DeviceState::CONNECTED;
        dev->reconnectPolicy.reset();
        dev->nextReconnectTimeMs = 0;
        LOG_INFO("Successfully reconnected to " << deviceName);
    }

    // Build Request
    protocol::ModbusRequest req;
    req.transactionId = m_txIdCounter.fetch_add(1);
    req.unitId = dev->config.unitId;
    req.functionCode = point.func;
    req.address = point.address;
    req.count = point.count;

    std::vector<uint8_t> frameData;
    bool isTcp = (dev->config.transport == "tcp");
    if (isTcp) {
        frameData = protocol::ModbusCodec::buildTcpFrame(req);
    } else {
        frameData = protocol::ModbusCodec::buildRtuFrame(req);
    }

    auto start = std::chrono::high_resolution_clock::now();

    if (!dev->transport->send(frameData.data(), frameData.size())) {
        dev->transport->disconnect();
        dev->state = DeviceState::ERROR_STATE;
        int delay = dev->reconnectPolicy.nextDelayMs();
        dev->nextReconnectTimeMs = nowMs + ((delay > 0) ? delay : 5000);
        util::Metrics::instance().recordTimeout();
        outRecord.status = "SEND_FAILED";
        return false;
    }

    uint8_t rxBuffer[2048];
    size_t bytesRead = 0;
    if (!dev->transport->receive(rxBuffer, sizeof(rxBuffer), bytesRead, dev->config.timeoutMs)) {
        auto end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::milli>(end - start).count();
        outRecord.latencyMs = latency;
        outRecord.status = "TIMEOUT";
        util::Metrics::instance().recordTimeout();

        // Check if transport died
        if (!dev->transport->isConnected()) {
            dev->state = DeviceState::ERROR_STATE;
            int delay = dev->reconnectPolicy.nextDelayMs();
            dev->nextReconnectTimeMs = nowMs + ((delay > 0) ? delay : 5000);
        }
        return false;
    }

    auto end = std::chrono::high_resolution_clock::now();
    double latency = std::chrono::duration<double, std::milli>(end - start).count();
    outRecord.latencyMs = latency;

    protocol::ModbusResponse resp;
    size_t consumed = 0;
    protocol::CodecStatus codecStatus;
    if (isTcp) {
        codecStatus = protocol::ModbusCodec::parseTcpResponse(rxBuffer, bytesRead, resp, consumed);
    } else {
        codecStatus = protocol::ModbusCodec::parseRtuResponse(rxBuffer, bytesRead, dev->config.unitId, point.func, resp, consumed);
    }

    if (codecStatus == protocol::CodecStatus::CRC_ERROR) {
        util::Metrics::instance().recordCrcError();
        outRecord.status = "CRC_ERROR";
        return false;
    } else if (codecStatus != protocol::CodecStatus::OK) {
        util::Metrics::instance().recordTimeout();
        outRecord.status = "PARSE_ERROR_" + protocol::codecStatusToString(codecStatus);
        return false;
    }

    if (resp.isException) {
        util::Metrics::instance().recordException(latency);
        outRecord.status = "EXCEPTION_" + std::to_string(static_cast<int>(resp.exceptionCode));
        return false;
    }

    // Success! Reset reconnect policy and record values
    dev->reconnectPolicy.reset();
    dev->state = DeviceState::CONNECTED;
    util::Metrics::instance().recordSuccess(latency);
    outRecord.status = "OK";

    std::ostringstream valStream;
    if (!resp.registerValues.empty()) {
        for (size_t i = 0; i < resp.registerValues.size(); ++i) {
            if (i > 0) valStream << ",";
            valStream << resp.registerValues[i];
        }
    } else if (!resp.coilValues.empty()) {
        for (size_t i = 0; i < resp.coilValues.size(); ++i) {
            if (i > 0) valStream << ",";
            valStream << (resp.coilValues[i] ? "1" : "0");
        }
    }
    outRecord.valuesStr = valStream.str();
    return true;
}

bool DeviceManager::writeSingleRegister(const std::string& deviceName, uint16_t address, uint16_t value, storage::CommunicationRecord& outRecord) {
    std::shared_ptr<ManagedDevice> dev;
    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        auto it = m_devices.find(deviceName);
        if (it == m_devices.end()) return false;
        dev = it->second;
    }

    std::lock_guard<std::mutex> devLock(dev->deviceMutex);
    outRecord.timestampMs = storage::CommunicationRecord::currentTimestampMs();
    outRecord.deviceName = deviceName;
    outRecord.unitId = dev->config.unitId;
    outRecord.functionCode = 6;
    outRecord.address = address;
    outRecord.count = 1;
    outRecord.valuesStr = std::to_string(value);

    if (!dev->transport->isConnected() && !dev->transport->connect()) {
        outRecord.status = "CONNECT_FAILED";
        return false;
    }

    protocol::ModbusRequest req;
    req.transactionId = m_txIdCounter.fetch_add(1);
    req.unitId = dev->config.unitId;
    req.functionCode = 6;
    req.address = address;
    req.count = 1;
    req.values = {value};

    bool isTcp = (dev->config.transport == "tcp");
    std::vector<uint8_t> frame = isTcp ? protocol::ModbusCodec::buildTcpFrame(req) : protocol::ModbusCodec::buildRtuFrame(req);

    auto start = std::chrono::high_resolution_clock::now();
    if (!dev->transport->send(frame.data(), frame.size())) {
        outRecord.status = "SEND_FAILED";
        return false;
    }

    uint8_t rx[512];
    size_t rxLen = 0;
    if (!dev->transport->receive(rx, sizeof(rx), rxLen, dev->config.timeoutMs)) {
        outRecord.status = "TIMEOUT";
        return false;
    }
    auto end = std::chrono::high_resolution_clock::now();
    outRecord.latencyMs = std::chrono::duration<double, std::milli>(end - start).count();

    protocol::ModbusResponse resp;
    size_t consumed = 0;
    protocol::CodecStatus st = isTcp ? protocol::ModbusCodec::parseTcpResponse(rx, rxLen, resp, consumed)
                                     : protocol::ModbusCodec::parseRtuResponse(rx, rxLen, dev->config.unitId, 6, resp, consumed);

    if (st == protocol::CodecStatus::OK && !resp.isException) {
        outRecord.status = "OK";
        return true;
    }
    outRecord.status = resp.isException ? ("EXCEPTION_" + std::to_string(static_cast<int>(resp.exceptionCode))) : "ERROR";
    return false;
}

bool DeviceManager::writeSingleCoil(const std::string& deviceName, uint16_t address, bool value, storage::CommunicationRecord& outRecord) {
    std::shared_ptr<ManagedDevice> dev;
    {
        std::lock_guard<std::mutex> lock(m_managerMutex);
        auto it = m_devices.find(deviceName);
        if (it == m_devices.end()) return false;
        dev = it->second;
    }

    std::lock_guard<std::mutex> devLock(dev->deviceMutex);
    outRecord.timestampMs = storage::CommunicationRecord::currentTimestampMs();
    outRecord.deviceName = deviceName;
    outRecord.unitId = dev->config.unitId;
    outRecord.functionCode = 5;
    outRecord.address = address;
    outRecord.count = 1;
    outRecord.valuesStr = value ? "1" : "0";

    if (!dev->transport->isConnected() && !dev->transport->connect()) {
        outRecord.status = "CONNECT_FAILED";
        return false;
    }

    protocol::ModbusRequest req;
    req.transactionId = m_txIdCounter.fetch_add(1);
    req.unitId = dev->config.unitId;
    req.functionCode = 5;
    req.address = address;
    req.count = 1;
    req.coilValues = {value};

    bool isTcp = (dev->config.transport == "tcp");
    std::vector<uint8_t> frame = isTcp ? protocol::ModbusCodec::buildTcpFrame(req) : protocol::ModbusCodec::buildRtuFrame(req);

    auto start = std::chrono::high_resolution_clock::now();
    if (!dev->transport->send(frame.data(), frame.size())) {
        outRecord.status = "SEND_FAILED";
        return false;
    }

    uint8_t rx[512];
    size_t rxLen = 0;
    if (!dev->transport->receive(rx, sizeof(rx), rxLen, dev->config.timeoutMs)) {
        outRecord.status = "TIMEOUT";
        return false;
    }
    auto end = std::chrono::high_resolution_clock::now();
    outRecord.latencyMs = std::chrono::duration<double, std::milli>(end - start).count();

    protocol::ModbusResponse resp;
    size_t consumed = 0;
    protocol::CodecStatus st = isTcp ? protocol::ModbusCodec::parseTcpResponse(rx, rxLen, resp, consumed)
                                     : protocol::ModbusCodec::parseRtuResponse(rx, rxLen, dev->config.unitId, 5, resp, consumed);

    if (st == protocol::CodecStatus::OK && !resp.isException) {
        outRecord.status = "OK";
        return true;
    }
    outRecord.status = resp.isException ? ("EXCEPTION_" + std::to_string(static_cast<int>(resp.exceptionCode))) : "ERROR";
    return false;
}

} // namespace gateway::device
