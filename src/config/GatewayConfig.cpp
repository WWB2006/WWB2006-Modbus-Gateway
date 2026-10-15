#include "gateway/config/GatewayConfig.h"

#include <fstream>
#include <set>
#include <sstream>

#include "gateway/json/JsonValue.h"

namespace gateway::config {
namespace {

using gateway::json::Value;

// ---- 取值辅助：缺省即用结构体里的默认值，类型不对则报错 ----

bool readString(const Value* node, const char* key, std::string& out, std::string& error) {
    const Value* value = node != nullptr ? node->member(key) : nullptr;
    if (value == nullptr) {
        return true;
    }
    if (!value->isString()) {
        error = std::string("字段 ") + key + " 必须是字符串";
        return false;
    }
    out = value->text;
    return true;
}

bool readU16(const Value* node, const char* key, uint16_t& out, std::string& error) {
    const Value* value = node != nullptr ? node->member(key) : nullptr;
    if (value == nullptr) {
        return true;
    }
    if (!value->isNumber() || value->number < 0 || value->number > 65535) {
        error = std::string("字段 ") + key + " 必须是 0-65535 之间的数字";
        return false;
    }
    out = static_cast<uint16_t>(value->number);
    return true;
}

bool readU8(const Value* node, const char* key, uint8_t& out, std::string& error) {
    uint16_t wide = out;
    if (!readU16(node, key, wide, error)) {
        return false;
    }
    if (wide > 247) {
        // Modbus 的合法从站地址范围是 1-247（0 是广播地址）。
        // 越界的地址永远不会被应答，属于「配了但没用」，因此在配置期拦住。
        error = std::string("字段 ") + key + " 必须在 0-247 之间（Modbus 从站地址范围）";
        return false;
    }
    out = static_cast<uint8_t>(wide);
    return true;
}

bool readInt(const Value* node, const char* key, int& out, std::string& error) {
    const Value* value = node != nullptr ? node->member(key) : nullptr;
    if (value == nullptr) {
        return true;
    }
    if (!value->isNumber() || value->number < 0 || value->number > 2147483647.0) {
        error = std::string("字段 ") + key + " 必须是非负整数";
        return false;
    }
    out = static_cast<int>(value->number);
    return true;
}

bool readSize(const Value* node, const char* key, std::size_t& out, std::string& error) {
    const Value* value = node != nullptr ? node->member(key) : nullptr;
    if (value == nullptr) {
        return true;
    }
    if (!value->isNumber() || value->number < 0) {
        error = std::string("字段 ") + key + " 必须是非负数字";
        return false;
    }
    out = static_cast<std::size_t>(value->number);
    return true;
}

// parity 在 JSON 里写单字符 "N" / "E" / "O"。
bool readParity(const Value* node, char& out, std::string& error) {
    std::string text;
    if (!readString(node, "parity", text, error)) {
        return false;
    }
    if (text.empty()) {
        return true;
    }
    const char ch = text[0];
    if (ch != 'N' && ch != 'n' && ch != 'E' && ch != 'e' && ch != 'O' && ch != 'o') {
        error = "字段 parity 只能是 N / E / O（无校验 / 偶校验 / 奇校验）";
        return false;
    }
    out = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return true;
}

// 解析一个轮询点。func 支持 3 / 4 / 6，其它值在加载期拒绝，
// 而不是留到轮询时静默跳过（阶段 2 的 tick 里会跳过未知功能码，
// 但那是运行期防护，配置错误应该在启动时就暴露）。
bool parsePoint(const Value& node, gateway::device::PollPoint& out, std::string& error) {
    if (!node.isObject()) {
        error = "points 的元素必须是对象";
        return false;
    }
    uint8_t func = 0;
    uint16_t address = 0;
    uint16_t count = 1;
    uint16_t value = 0;
    if (!readU8(&node, "func", func, error) || !readU16(&node, "address", address, error) ||
        !readU16(&node, "count", count, error) || !readU16(&node, "value", value, error)) {
        return false;
    }
    if (func != 0x03 && func != 0x04 && func != 0x06) {
        error = "点位 func 只支持 3（读保持寄存器）/ 4（读输入寄存器）/ 6（写单个寄存器），收到 " +
                std::to_string(func);
        return false;
    }
    if (func != 0x06 && count == 0) {
        error = "读点位的 count 必须大于 0";
        return false;
    }
    if (func == 0x03 && count > 125) {
        error = "读保持寄存器一次最多 125 个（Modbus 协议上限）";
        return false;
    }
    if (func == 0x04 && count > 125) {
        error = "读输入寄存器一次最多 125 个（Modbus 协议上限）";
        return false;
    }
    out.functionCode = func;
    out.address = address;
    out.count = func == 0x06 ? 1 : count;  // 写单寄存器时 count 恒为 1
    out.value = value;
    return true;
}

bool parseDevice(const Value& node, gateway::device::DeviceConfig& out, std::string& error) {
    if (!node.isObject()) {
        error = "devices 的元素必须是对象";
        return false;
    }
    if (!readString(&node, "name", out.name, error)) {
        return false;
    }
    if (out.name.empty()) {
        error = "设备必须指定 name";
        return false;
    }

    std::string transport = "tcp";
    if (!readString(&node, "transport", transport, error)) {
        return false;
    }
    if (transport == "tcp") {
        out.transport = gateway::transport::TransportKind::Tcp;
    } else if (transport == "rtu" || transport == "serial") {
        out.transport = gateway::transport::TransportKind::Serial;
    } else {
        error = "设备 " + out.name + " 的 transport 只能是 tcp 或 rtu，收到 " + transport;
        return false;
    }

    if (!readString(&node, "host", out.host, error) || !readU16(&node, "port", out.port, error) ||
        !readU8(&node, "unitId", out.unitId, error) ||
        !readU16(&node, "timeoutMs", out.timeoutMs, error) ||
        !readU16(&node, "pollIntervalMs", out.pollIntervalMs, error) ||
        !readInt(&node, "connectTimeoutMs", out.connectTimeoutMs, error) ||
        !readString(&node, "serialPort", out.serialPort, error) ||
        !readInt(&node, "baudRate", out.baudRate, error) ||
        !readParity(&node, out.parity, error) ||
        !readInt(&node, "dataBits", out.dataBits, error) ||
        !readInt(&node, "stopBits", out.stopBits, error)) {
        return false;
    }

    uint16_t retry16 = out.retry;
    if (!readU16(&node, "retry", retry16, error)) {
        return false;
    }
    if (retry16 > 255) {
        error = "设备 " + out.name + " 的 retry 不能超过 255";
        return false;
    }
    out.retry = static_cast<uint8_t>(retry16);

    const Value* points = node.member("points");
    if (points != nullptr) {
        if (!points->isArray()) {
            error = "设备 " + out.name + " 的 points 必须是数组";
            return false;
        }
        for (const Value& pointNode : points->array) {
            gateway::device::PollPoint point;
            if (!parsePoint(pointNode, point, error)) {
                error = "设备 " + out.name + "：" + error;
                return false;
            }
            out.points.push_back(point);
        }
    }
    return true;
}

}  // namespace

bool parseGatewayConfig(const std::string& text, GatewayConfig& out, std::string& error) {
    error.clear();

    Value root;
    if (!gateway::json::parse(text, root, error)) {
        return false;
    }
    if (!root.isObject()) {
        error = "根节点必须是对象";
        return false;
    }

    GatewayConfig config;

    const Value* devices = root.member("devices");
    if (devices == nullptr) {
        error = "缺少 devices 段";
        return false;
    }
    if (!devices->isArray()) {
        error = "devices 必须是数组";
        return false;
    }
    if (devices->array.empty()) {
        error = "devices 不能为空：至少配置一台设备";
        return false;
    }

    // 设备名重复会导致 DeviceManager 里两个会话同名，
    // 界面按名字取会话时永远只能拿到第一个 —— 属于静默错误，配置期就拦。
    std::set<std::string> names;
    for (const Value& deviceNode : devices->array) {
        gateway::device::DeviceConfig device;
        if (!parseDevice(deviceNode, device, error)) {
            return false;
        }
        if (!names.insert(device.name).second) {
            error = "设备名重复：" + device.name + "（每台设备必须有唯一名字）";
            return false;
        }
        config.devices.push_back(std::move(device));
    }

    const Value* storage = root.member("storage");
    if (storage != nullptr && !storage->isObject()) {
        error = "storage 必须是对象";
        return false;
    }
    if (!readString(storage, "database", config.storage.database, error) ||
        !readSize(storage, "batchSize", config.storage.batchSize, error)) {
        return false;
    }

    const Value* logging = root.member("logging");
    if (logging != nullptr && !logging->isObject()) {
        error = "logging 必须是对象";
        return false;
    }
    if (!readString(logging, "level", config.logging.level, error) ||
        !readSize(logging, "rotateSizeMb", config.logging.rotateSizeMb, error)) {
        return false;
    }

    out = std::move(config);
    return true;
}

bool loadGatewayConfig(const std::string& path, GatewayConfig& out, std::string& error) {
    error.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "打不开配置文件：" + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!parseGatewayConfig(buffer.str(), out, error)) {
        error = path + " " + error;
        return false;
    }
    return true;
}

}  // namespace gateway::config
