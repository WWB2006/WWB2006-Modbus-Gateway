#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gateway/sim/RegisterMap.h"

namespace gateway::sim {

// 从站模拟器的配置文件。
//
// 为什么要从 JSON 读寄存器初值（而不是写死在代码里）：
// 边界场景（地址刚好越界一位、CRC 错误、寄存器全 0）需要频繁改初值，
// 每次改都重编译会把「构造故障场景」变成一件麻烦事，最后没人愿意做。
// 配置化的实际收益是：测试脚本可以直接生成一份 JSON 来构造它要的场景。
struct SimulatorConfig {
    std::string bindAddress = "127.0.0.1";
    uint16_t port = 5020;
    uint8_t unitId = 1;
    bool supportBroadcast = false;
    bool verbose = false;

    std::size_t holdingSize = 128;
    std::size_t inputSize = 64;
    std::size_t coilSize = 64;
    std::size_t discreteInputSize = 32;

    std::map<uint32_t, uint32_t> holdingValues;
    std::map<uint32_t, uint32_t> inputValues;
    std::map<uint32_t, bool> coilValues;
    std::map<uint32_t, bool> discreteInputValues;
};

// 解析失败时返回 false，error 给出「第几行附近、期望什么」的可定位信息。
// 自研的极简解析器只覆盖本配置文件用到的子集（对象、数组、数字、布尔、字符串），
// 不引入第三方依赖 —— 与项目「不依赖 Qt 也能跑」的约束一致。
bool loadSimulatorConfig(const std::string& path, SimulatorConfig& out, std::string& error);

// 按配置初始化数据区。
void applyTo(RegisterMap& map, const SimulatorConfig& config);

}  // namespace gateway::sim
