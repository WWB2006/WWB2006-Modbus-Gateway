#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gateway/transport/TransportInterface.h"

namespace gateway::device {

// 一个轮询点：功能码 + 起始地址 + 数量 +（写操作时的）写入值。
struct PollPoint {
    uint8_t functionCode = 0x03;
    uint16_t address = 0;
    uint16_t count = 10;
    // 仅写类功能码（0x05 写单线圈 / 0x06 写单寄存器 / 0x0F、0x10 写多个）使用；
    // 读类功能码忽略该字段。写单寄存器时它就是「要写进去的那个值」，
    // 与 count（读多少个寄存器）语义完全不同，不能混用。
    uint16_t value = 0;
};

// 阶段 9：断线重连策略（指数退避）。
//
// 为什么需要退避而不是"立刻重连"：
//   现场设备掉电或网线被拔时，如果每秒重连几十次，会把设备的 TCP 连接表打满，
//   设备恢复后反而因为资源耗尽连不上。指数退避（1s → 2s → 4s → … → 上限）
//   既保证恢复后能较快接上，又不会在长时间故障中拖垮设备。
struct ReconnectPolicy {
    bool enabled = true;
    // 首次重连延迟（毫秒）。
    uint32_t initialDelayMs = 1000;
    // 退避上限（毫秒）。指数增长到这里就封顶，避免退避到几十分钟。
    uint32_t maxDelayMs = 30000;
    // 退避倍率。2 = 每次都翻倍。
    uint32_t multiplier = 2;
    // 连续失败多少次后进入退避（前几次立即重试）。
    uint32_t failuresBeforeBackoff = 2;
};

// 一台设备的采集配置，字段与 config/gateway.json 一一对应。
struct DeviceConfig {
    std::string name;
    transport::TransportKind transport = transport::TransportKind::Tcp;
    uint8_t unitId = 1;
    uint16_t timeoutMs = 1000;
    uint8_t retry = 3;  // 最多重传次数，总尝试次数 = retry + 1
    uint16_t pollIntervalMs = 500;
    std::vector<PollPoint> points;

    // 阶段 9：断线自动恢复策略。
    ReconnectPolicy reconnect;

    // —— 链路参数：与 gateway.json 中的连接字段一一对应 ——
    // TCP
    std::string host = "127.0.0.1";
    uint16_t port = 502;
    int connectTimeoutMs = 3000;
    // 串口（RTU）
    std::string serialPort = "COM11";
    int baudRate = 9600;
    char parity = 'N';
    int dataBits = 8;
    int stopBits = 1;

    // 把本结构里的链路字段原样搬进 TransportConfig。
    // 早期版本只复制了 kind，导致 gateway.json 里的 host/port/serialPort 等
    // 全部被丢弃、设备永远连到默认的 127.0.0.1:502 —— 这里逐项补齐。
    transport::TransportConfig toTransportConfig() const {
        transport::TransportConfig config;
        config.kind = transport;
        config.host = host;
        config.port = port;
        config.connectTimeoutMs = connectTimeoutMs;
        config.serialPort = serialPort;
        config.baudRate = baudRate;
        config.parity = parity;
        config.dataBits = dataBits;
        config.stopBits = stopBits;
        return config;
    }
};

}  // namespace gateway::device
