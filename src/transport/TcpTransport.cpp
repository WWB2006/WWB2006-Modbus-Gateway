#include "gateway/transport/TcpTransport.h"
#include "gateway/util/Logger.h"
#include <atomic>
#include <cstring>

namespace gateway::transport {

static std::atomic<int> s_wsaRefCount{0};

void TcpTransport::initNetwork() {
#if defined(_WIN32)
    if (s_wsaRefCount.fetch_add(1) == 0) {
        WSADATA wsaData;
        int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (res != 0) {
            LOG_ERROR("WSAStartup failed: " << res);
        }
    }
#endif
}

void TcpTransport::cleanupNetwork() {
#if defined(_WIN32)
    if (s_wsaRefCount.fetch_sub(1) == 1) {
        WSACleanup();
    }
#endif
}

TcpTransport::TcpTransport(std::string host, uint16_t port, int timeoutMs)
    : m_host(std::move(host)), m_port(port), m_timeoutMs(timeoutMs),
      m_socket(INVALID_SOCKET_VAL), m_connected(false) {
    initNetwork();
}

TcpTransport::~TcpTransport() {
    disconnect();
    cleanupNetwork();
}

bool TcpTransport::connect() {
    if (m_connected) {
        disconnect();
    }

    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET_VAL) {
        LOG_ERROR("Failed to create TCP socket for " << m_host << ":" << m_port);
        return false;
    }

    // Set TCP_NODELAY
    int flag = 1;
    setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    // Set non-blocking mode for connect timeout
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(m_socket, FIONBIO, &mode);
#else
    int flags = fcntl(m_socket, F_GETFL, 0);
    fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
#if defined(_WIN32)
    addr.sin_addr.s_addr = inet_addr(m_host.c_str());
#else
    inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);
#endif

    int res = ::connect(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool connectOk = false;

    if (res == 0) {
        connectOk = true;
    } else {
#if defined(_WIN32)
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
#else
        if (errno == EINPROGRESS) {
#endif
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(m_socket, &writeSet);

            timeval tv{};
            tv.tv_sec = m_timeoutMs / 1000;
            tv.tv_usec = (m_timeoutMs % 1000) * 1000;

            int selRes = select(static_cast<int>(m_socket + 1), nullptr, &writeSet, nullptr, &tv);
            if (selRes > 0) {
                int soError = 0;
                socklen_t len = sizeof(soError);
                getsockopt(m_socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
                if (soError == 0) {
                    connectOk = true;
                }
            }
        }
    }

    if (!connectOk) {
        disconnect();
        return false;
    }

    // Restore to blocking mode
#if defined(_WIN32)
    mode = 0;
    ioctlsocket(m_socket, FIONBIO, &mode);
#else
    fcntl(m_socket, F_SETFL, flags & ~O_NONBLOCK);
#endif

    m_connected = true;
    return true;
}

void TcpTransport::disconnect() {
    if (m_socket != INVALID_SOCKET_VAL) {
#if defined(_WIN32)
        closesocket(m_socket);
#else
        close(m_socket);
#endif
        m_socket = INVALID_SOCKET_VAL;
    }
    m_connected = false;
}

bool TcpTransport::isConnected() const {
    return m_connected && m_socket != INVALID_SOCKET_VAL;
}

bool TcpTransport::send(const uint8_t* data, size_t len) {
    if (!isConnected()) return false;

    size_t totalSent = 0;
    while (totalSent < len) {
        int sent = ::send(m_socket, reinterpret_cast<const char*>(data + totalSent), static_cast<int>(len - totalSent), 0);
        if (sent <= 0) {
            disconnect();
            return false;
        }
        totalSent += static_cast<size_t>(sent);
    }
    return true;
}

bool TcpTransport::receive(uint8_t* buffer, size_t maxLen, size_t& bytesRead, int timeoutMs) {
    bytesRead = 0;
    if (!isConnected()) return false;

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(m_socket, &readSet);

    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int selRes = select(static_cast<int>(m_socket + 1), &readSet, nullptr, nullptr, &tv);
    if (selRes <= 0) {
        // Timeout or select error
        return false;
    }

    int n = ::recv(m_socket, reinterpret_cast<char*>(buffer), static_cast<int>(maxLen), 0);
    if (n <= 0) {
        disconnect();
        return false;
    }

    bytesRead = static_cast<size_t>(n);
    return true;
}

std::string TcpTransport::getEndpointInfo() const {
    return m_host + ":" + std::to_string(m_port);
}

} // namespace gateway::transport
