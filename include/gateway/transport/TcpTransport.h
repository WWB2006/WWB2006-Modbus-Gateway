#pragma once

#include "gateway/transport/TransportInterface.h"
#include <string>
#include <cstdint>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define INVALID_SOCKET_VAL INVALID_SOCKET
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
typedef int socket_t;
#define INVALID_SOCKET_VAL (-1)
#endif

namespace gateway::transport {

class TcpTransport : public TransportInterface {
public:
    TcpTransport(std::string host, uint16_t port, int timeoutMs = 1000);
    ~TcpTransport() override;

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;

    bool send(const uint8_t* data, size_t len) override;
    bool receive(uint8_t* buffer, size_t maxLen, size_t& bytesRead, int timeoutMs) override;

    std::string getEndpointInfo() const override;

    void setTimeoutMs(int timeoutMs) { m_timeoutMs = timeoutMs; }

private:
    static void initNetwork();
    static void cleanupNetwork();

    std::string m_host;
    uint16_t m_port;
    int m_timeoutMs;
    socket_t m_socket;
    bool m_connected;
};

} // namespace gateway::transport
