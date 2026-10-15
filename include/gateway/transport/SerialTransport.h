#pragma once

#include "gateway/transport/TransportInterface.h"
#include <string>
#include <cstdint>

#if defined(_WIN32)
#include <windows.h>
typedef HANDLE serial_handle_t;
#define INVALID_SERIAL_HANDLE INVALID_HANDLE_VALUE
#else
typedef int serial_handle_t;
#define INVALID_SERIAL_HANDLE (-1)
#endif

namespace gateway::transport {

class SerialTransport : public TransportInterface {
public:
    SerialTransport(std::string portName, int baudRate = 9600, char parity = 'N', int dataBits = 8, int stopBits = 1);
    ~SerialTransport() override;

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;

    bool send(const uint8_t* data, size_t len) override;
    bool receive(uint8_t* buffer, size_t maxLen, size_t& bytesRead, int timeoutMs) override;

    std::string getEndpointInfo() const override;

    // Modbus RTU requires 3.5 character times between frames
    void waitInterFrameDelay();

private:
    std::string m_portName;
    int m_baudRate;
    char m_parity;
    int m_dataBits;
    int m_stopBits;

    serial_handle_t m_handle;
    bool m_connected;
    int m_interCharDelayUs;
};

} // namespace gateway::transport
