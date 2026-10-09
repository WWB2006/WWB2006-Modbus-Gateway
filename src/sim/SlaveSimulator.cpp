// InetPtonA / InetNtopA 与 ws2tcpip.h 里的声明都需要 _WIN32_WINNT >= 0x0600。
// 这个宏必须出现在**任何** Windows 头文件之前，因此放在文件最顶部，
// 而不是放在 #include <winsock2.h> 之前 —— 一旦有别的头先引入了 windows.h 就失效了。
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif

#include "gateway/sim/SlaveSimulator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "gateway/protocol/ModbusCodec.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define CLOSE_SOCKET closesocket
#define LAST_SOCKET_ERROR WSAGetLastError()
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET ::close
#define LAST_SOCKET_ERROR errno
#endif

namespace gateway::sim {

namespace {

#if defined(_WIN32)
using SockLen = int;
#else
using SockLen = socklen_t;
#endif

// 地址转换的两个薄封装，把平台差异收在一处。
bool parseIPv4(const std::string& text, in_addr& out) {
#if defined(_WIN32)
    return InetPtonA(AF_INET, text.c_str(), &out) == 1;
#else
    return ::inet_pton(AF_INET, text.c_str(), &out) == 1;
#endif
}

std::string formatIPv4(const in_addr& address) {
    char text[INET_ADDRSTRLEN] = {0};
#if defined(_WIN32)
    // InetNtopA 的第二个参数是 PVOID（非 const），这里必须去掉 const 才能调用。
    in_addr copy = address;
    InetNtopA(AF_INET, &copy, text, static_cast<DWORD>(sizeof(text)));
#else
    ::inet_ntop(AF_INET, &address, text, sizeof(text));
#endif
    return text;
}

constexpr std::size_t kReadBufferSize = 4096;

// 把套接字设为非阻塞：模拟器用 select 轮询所有连接，任何一个阻塞都会拖住全部客户端。
bool setNonBlocking(int fd) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void closeSocket(int fd) {
    if (fd >= 0) {
        CLOSE_SOCKET(fd);
    }
}

}  // namespace

// ------------------------------------------------------------- 生命周期

SlaveSimulator::SlaveSimulator(RegisterMap& map, Config config)
    : map_(&map), config_(std::move(config)) {
    handler_.setMap(map);
    handler_.setUnitId(config_.unitId);
    handler_.setSupportBroadcast(config_.supportBroadcast);

#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) == 0) {
        winsockReady_ = true;
    } else {
        error_ = "WSAStartup 失败，无法使用套接字";
    }
#endif
}

SlaveSimulator::~SlaveSimulator() {
    stop();
#if defined(_WIN32)
    if (winsockReady_) {
        WSACleanup();
    }
#endif
}

bool SlaveSimulator::start() {
    if (running()) {
        return true;
    }

    const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        error_ = "创建套接字失败（错误码 " + std::to_string(LAST_SOCKET_ERROR) + "）";
        return false;
    }

    // SO_REUSEADDR：测试里反复启停模拟器时，避免 TIME_WAIT 让 bind 失败。
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (config_.bindAddress == "0.0.0.0" || config_.bindAddress.empty()) {
        address.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (!parseIPv4(config_.bindAddress, address.sin_addr)) {
        error_ = "监听地址不是合法的 IPv4：" + config_.bindAddress;
        closeSocket(fd);
        return false;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error_ = "绑定 " + config_.bindAddress + ":" + std::to_string(config_.port) +
                 " 失败（错误码 " + std::to_string(LAST_SOCKET_ERROR) +
                 "）。端口被占用或权限不足（1024 以下端口需要管理员）";
        closeSocket(fd);
        return false;
    }
    if (::listen(fd, 8) != 0) {
        error_ = "监听失败（错误码 " + std::to_string(LAST_SOCKET_ERROR) + "）";
        closeSocket(fd);
        return false;
    }
    if (!setNonBlocking(fd)) {
        error_ = "设置非阻塞失败";
        closeSocket(fd);
        return false;
    }

    listenFd_ = fd;
    stopRequested_ = false;
    return true;
}

void SlaveSimulator::stop() {
    for (std::size_t i = clients_.size(); i > 0; --i) {
        closeClient(i - 1);
    }
    clients_.clear();
    if (listenFd_ != kInvalidFd) {
        closeSocket(listenFd_);
        listenFd_ = kInvalidFd;
    }
}

// ------------------------------------------------------------- 事件循环

void SlaveSimulator::acceptNewClients() {
    for (;;) {
        sockaddr_in peer{};
        SockLen peerLength = sizeof(peer);
        const int fd = static_cast<int>(
            ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLength));
        if (fd < 0) {
            break;  // 没有更多待处理连接（非阻塞下这是正常出口）
        }
        if (!setNonBlocking(fd)) {
            closeSocket(fd);
            continue;
        }
        int yes = 1;
        // 从站响应通常很小，禁用 Nagle 让测试里的往返延迟更接近真实值。
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        ++stats_.acceptedConnections;
        log("接受连接 " + formatIPv4(peer.sin_addr) + ":" +
            std::to_string(ntohs(peer.sin_port)));

        clients_.push_back(Client{fd, transport::ByteAccumulator{}});
    }
}

void SlaveSimulator::closeClient(std::size_t index) {
    if (index >= clients_.size()) {
        return;
    }
    closeSocket(clients_[index].fd);
    ++stats_.closedConnections;
    log("连接关闭");
    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
}

bool SlaveSimulator::sendAll(int fd, const uint8_t* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
        const int n = static_cast<int>(
            ::send(fd, reinterpret_cast<const char*>(data + sent),
                   static_cast<int>(len - sent), 0));
        if (n <= 0) {
#if !defined(_WIN32)
            if (n < 0 && (errno == EINTR)) {
                continue;
            }
#endif
            return false;  // 对端已关闭或缓冲区满；模拟器不需要为此维护写队列
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool SlaveSimulator::respondTo(const protocol::Frame& frame, std::vector<uint8_t>& out) {
    const ResponsePdu response = handler_.handleRequest(frame);
    if (response.pdu.empty()) {
        ++stats_.ignoredRequests;
        if (config_.verbose && !response.summary.empty()) {
            log("  忽略：" + response.summary);
        }
        return false;
    }

    // 响应帧与请求帧用同一个事务号（TCP），这是主站做请求响应匹配的唯一依据。
    out = protocol::encodeTcpRequest(frame.transactionId, config_.unitId, response.pdu);
    if (out.empty()) {
        ++stats_.malformedFrames;
        return false;
    }

    ++stats_.requestsHandled;
    if (response.exception != protocol::ExceptionCode::None) {
        ++stats_.exceptionResponses;
    }
    if (config_.verbose) {
        log("  " + response.summary + "  -> " + protocol::toHex(response.pdu));
    }
    return true;
}

void SlaveSimulator::handleClientBytes(Client& client) {
    uint8_t buffer[kReadBufferSize];
    for (;;) {
        const int n = static_cast<int>(::recv(client.fd, reinterpret_cast<char*>(buffer),
                                              static_cast<int>(sizeof(buffer)), 0));
        if (n > 0) {
            client.buffer.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            // 对端正常关闭。
            return;  // 由调用方根据 client.buffer 状态判断，这里只标记不再读
        }
#if defined(_WIN32)
        const int code = WSAGetLastError();
        if (code == WSAEWOULDBLOCK) {
            break;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
#endif
        // 其它错误当作断连处理。
        client.fd = -1;
        break;
    }
}

std::size_t SlaveSimulator::pollOnce(int timeoutMs) {
    if (!running()) {
        return 0;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(listenFd_, &readSet);
    int maxFd = listenFd_;
    for (const Client& client : clients_) {
        if (client.fd >= 0) {
            FD_SET(client.fd, &readSet);
            maxFd = std::max(maxFd, client.fd);
        }
    }

    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    const int ready = ::select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);
    std::size_t processed = 0;

    if (ready > 0) {
        if (FD_ISSET(listenFd_, &readSet)) {
            acceptNewClients();
        }

        // 从后往前遍历：closeClient 会删除元素，倒序可以安全跳过被删下标。
        for (std::size_t i = clients_.size(); i > 0; --i) {
            const std::size_t index = i - 1;
            Client& client = clients_[index];
            if (client.fd < 0 || !FD_ISSET(client.fd, &readSet)) {
                continue;
            }

            const int fd = client.fd;
            handleClientBytes(client);
            if (fd < 0) {
                closeClient(index);
                continue;
            }

            // 逐帧解析：TCP 靠 MBAP 长度字段切包，一次可能解出多帧（粘包）。
            for (;;) {
                protocol::DecodeResult decoded;
                const std::size_t consumed =
                    protocol::decodeTcp(client.buffer.data(), client.buffer.size(), decoded);
                if (consumed == 0) {
                    if (decoded.status != protocol::DecodeStatus::Incomplete) {
                        ++stats_.malformedFrames;
                        if (config_.verbose) {
                            log("  丢弃非法帧：" + decoded.message);
                        }
                        // 协议标识或长度字段非法时无法推断帧长，只能断开连接重新开始，
                        // 否则会卡在一个永远解不开的缓冲区上。
                        client.buffer.clear();
                        if (decoded.status == protocol::DecodeStatus::BadProtocolId ||
                            decoded.status == protocol::DecodeStatus::BadLength) {
                            closeClient(index);
                        }
                    }
                    break;
                }
                processed += consumed;
                client.buffer.consume(consumed);

                std::vector<uint8_t> response;
                if (respondTo(decoded.frame, response)) {
                    if (!sendAll(client.fd, response.data(), response.size())) {
                        closeClient(index);
                        break;
                    }
                    processed += response.size();
                }
            }
        }
    }

    // select 返回 0 说明本轮空闲；调用方据此决定是否继续。
    return processed;
}

void SlaveSimulator::run() {
    while (!stopRequested_ && running()) {
        pollOnce(200);
    }
}

void SlaveSimulator::log(const std::string& line) const {
    if (config_.verbose) {
        std::printf("[模拟器] %s\n", line.c_str());
        std::fflush(stdout);
    }
}

}  // namespace gateway::sim
