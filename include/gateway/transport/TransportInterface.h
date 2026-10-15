#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace gateway::transport {

class TransportInterface {
public:
    virtual ~TransportInterface() = default;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    virtual bool send(const uint8_t* data, size_t len) = 0;
    virtual bool receive(uint8_t* buffer, size_t maxLen, size_t& bytesRead, int timeoutMs) = 0;

    virtual std::string getEndpointInfo() const = 0;
};

} // namespace gateway::transport
