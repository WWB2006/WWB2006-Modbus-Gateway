#pragma once

#include "gateway/device/DeviceConfig.h"
#include "gateway/device/ReconnectPolicy.h"
#include "gateway/transport/TransportInterface.h"
#include "gateway/storage/RecordRepository.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <map>
#include <atomic>

namespace gateway::device {

enum class DeviceState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR_STATE
};

inline std::string deviceStateToString(DeviceState st) {
    switch (st) {
        case DeviceState::DISCONNECTED: return "DISCONNECTED";
        case DeviceState::CONNECTING:   return "CONNECTING";
        case DeviceState::CONNECTED:    return "CONNECTED";
        case DeviceState::ERROR_STATE:  return "ERROR";
        default:                        return "UNKNOWN";
    }
}

struct ManagedDevice {
    DeviceConfig config;
    std::unique_ptr<transport::TransportInterface> transport;
    ReconnectPolicy reconnectPolicy;
    DeviceState state = DeviceState::DISCONNECTED;
    int64_t nextReconnectTimeMs = 0;
    std::mutex deviceMutex;
};

class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    bool addDevice(const DeviceConfig& config);
    bool removeDevice(const std::string& name);
    void clear();

    std::vector<std::string> getDeviceNames();
    DeviceState getDeviceState(const std::string& name);

    /**
     * @brief Executes a single point read from the specified device.
     * Handles reconnect backoff if disconnected.
     */
    bool pollPoint(const std::string& deviceName, const DataPointConfig& point, storage::CommunicationRecord& outRecord);

    /**
     * @brief Writes a single register or coil to a device.
     */
    bool writeSingleRegister(const std::string& deviceName, uint16_t address, uint16_t value, storage::CommunicationRecord& outRecord);
    bool writeSingleCoil(const std::string& deviceName, uint16_t address, bool value, storage::CommunicationRecord& outRecord);

private:
    std::unique_ptr<transport::TransportInterface> createTransport(const DeviceConfig& cfg);

    std::map<std::string, std::shared_ptr<ManagedDevice>> m_devices;
    std::mutex m_managerMutex;
    std::atomic<uint16_t> m_txIdCounter{1};
};

} // namespace gateway::device
