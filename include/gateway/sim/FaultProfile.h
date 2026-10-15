#pragma once

#include <string>
#include <cstdint>

namespace gateway::sim {

struct DelayFault {
    int minMs = 0;
    int maxMs = 0;
    double ratio = 0.0; // 0.0 to 1.0 probability
};

struct CrcFault {
    double ratio = 0.0; // 0.0 to 1.0 probability
};

struct ExceptionFault {
    double ratio = 0.0;
    uint8_t code = 0x06; // Default to 0x06 (Slave Device Busy)
};

struct DisconnectFault {
    uint64_t afterRequests = 0;
    int durationMs = 0;
};

struct FaultProfile {
    std::string name = "normal";
    DelayFault delay;
    CrcFault crcError;
    ExceptionFault exception;
    DisconnectFault disconnect;
};

} // namespace gateway::sim
