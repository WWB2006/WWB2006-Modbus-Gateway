#pragma once

#include <string>
#include "gateway/device/DeviceConfig.h"

namespace gateway::util {

class AppConfig {
public:
    static bool loadFromFile(const std::string& path, gateway::device::GatewayAppConfig& outConfig);
    static bool saveToFile(const std::string& path, const gateway::device::GatewayAppConfig& config);
    static gateway::device::GatewayAppConfig createDefault();
};

} // namespace gateway::util
