#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gateway/device/DeviceConfig.h"

namespace gateway::config {

// 网关运行所需的存储配置（阶段 6 使用）。
struct StorageConfig {
    std::string database = "data/records.db";
    std::size_t batchSize = 500;
};

// 日志配置（阶段 11 使用）。
struct LoggingConfig {
    std::string level = "info";
    std::size_t rotateSizeMb = 32;
};

// config/gateway.json 的完整映射。
struct GatewayConfig {
    std::vector<gateway::device::DeviceConfig> devices;
    StorageConfig storage;
    LoggingConfig logging;
};

// 从 JSON 文本解析网关配置。
//
// 失败时返回 false，error 给出「第几行第几列 + 期望什么」的可定位信息。
//
// 为什么单独做一个解析器而不是复用 SimulatorConfig.cpp 里的那份：
// 那份的 JSON 解析器藏在匿名命名空间里、且只认识从站的字段结构；
// 网关配置的设备列表 / 点位数组结构完全不同。两份共用一个**通用 JSON 值解析器**
// （见 JsonValue.h），再各自做字段映射 —— 这样既避免重复实现 JSON 语法，
// 又让两份配置的字段约束各写各的（比如设备名不能重复，从站不需要这条）。
bool parseGatewayConfig(const std::string& text, GatewayConfig& out, std::string& error);

// 从文件读并解析。文件打不开时 error 直接给出路径。
bool loadGatewayConfig(const std::string& path, GatewayConfig& out, std::string& error);

}  // namespace gateway::config
