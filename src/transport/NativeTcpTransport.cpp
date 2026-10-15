// NativeTcpTransport：不依赖 Qt 的 TCP 链路（阶段 11 的 gateway_daemon 使用）。
//
// 详细设计说明见头文件。这里只强调三个实现要点：
//   1. Windows / POSIX 的 socket API 差异全部收敛在本文件里（typedef + 几个小函数）；
//   2. 所有错误都走 notifyMessage / notifyState，不抛异常、不 printf ——
//      链路层不该决定日志往哪写；
//   3. connect 用「非阻塞 connect + select 等可写 + 查 SO_ERROR」实现带超时的连接，
//      连上后**恢复阻塞模式**：本链路由 pump() 主动驱动，不需要非阻塞 IO 的复杂度。

#include "gateway/transport/NativeTcpTransport.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace gateway::transport {

namespace {

#ifdef _WIN32
using SocketFd = SOCKET;
constexpr SocketFd kInvalidSocket = INVALID_SOCKET;

// WinSock 必须按进程初始化。用引用计数而不是 bool：
// 可能有多个实例，谁也不能假设自己是第一个或最后一个。
int& winsockRefCount() {
    static int count = 0;
    return count;
}

bool acquireWinsock() {
    if (winsockRefCount()++ == 0) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            winsockRefCount() = 0;
            return false;
        }
    }
    return true;
}

void releaseWinsock() {
    if (--winsockRefCount() == 0) {
        WSACleanup();
    }
}
#else
using SocketFd = int;
constexpr SocketFd kInvalidSocket = -1;
#endif

// 最后一次 socket 错误的可读文案。把「错误码」翻译成人能看懂的话，
// 现场排查时比 errno=10061 有用得多。
std::string lastSocketError() {
#ifdef _WIN32
    const int code = WSAGetLastError();
    char buffer[256] = {0};
    const DWORD length =
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                       buffer, static_cast<DWORD>(sizeof(buffer) - 1), nullptr);
    std::string text;
    if (length > 0) {
        text.assign(buffer, length);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.empty()) {
        text = "WinSock 错误 " + std::to_string(code);
    }
    return text;
#else
    return std::strerror(errno);
#endif
}

void closeSocket(SocketFd fd) {
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

bool setBlocking(SocketFd fd, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ::ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int updated = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, updated) == 0;
#endif
}

// 解析点分十进制 IPv4。
//
// 为什么不用 inet_pton：MinGW-w64 的 ws2tcpip.h 只在 `_WIN32_WINNT >= 0x0600`
// 的块里把 InetPtonA 宏映射成 inet_pton，但**不声明** inet_pton 本身，
// 于是 g++ 直接报「'::inet_pton' has not been declared」。
// 与其去猜各版本头文件的条件，不如用两边都稳的老接口：
// Windows 用 inet_addr（失败返回 INADDR_NONE），POSIX 用 inet_pton。
// 副作用：Windows 侧无法解析 255.255.255.255（它等于 INADDR_NONE）——
// 那不是合法的单播目标地址，本项目不会用到。
bool parseIpv4(const std::string& text, in_addr& out) {
#ifdef _WIN32
    const unsigned long address = ::inet_addr(text.c_str());
    if (address == INADDR_NONE) {
        return false;
    }
    out.s_addr = address;
    return true;
#else
    return ::inet_pton(AF_INET, text.c_str(), &out) == 1;
#endif
}

// 连接是否在进行中（非阻塞 connect 的正常中间状态）。
bool inProgress() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS;
#endif
}

// 等 socket 变成可读（readable=true）或可写（readable=false）。
// 返回 1 就绪 / 0 超时 / -1 出错。
int waitFor(SocketFd fd, bool readable, int timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);

    timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    // Windows 下第一个参数被忽略，但必须传一个合法值；POSIX 下必须 > fd。
    const int result = ::select(static_cast<int>(fd) + 1, readable ? &set : nullptr,
                                readable ? nullptr : &set, nullptr, &timeout);
    if (result > 0) {
        return 1;
    }
    if (result == 0) {
        return 0;
    }
    return -1;
}

}  // namespace

NativeTcpTransport::NativeTcpTransport(const TransportConfig& config) : config_(config) {
#ifdef _WIN32
    winsockReady_ = acquireWinsock();
#endif
}

NativeTcpTransport::~NativeTcpTransport() {
    close();
#ifdef _WIN32
    if (winsockReady_) {
        releaseWinsock();
    }
#endif
}

bool NativeTcpTransport::open() {
    if (fd_ >= 0) {
        return true;  // 幂等：已经开着就直接成功
    }
#ifdef _WIN32
    if (!winsockReady_) {
        notifyMessage("WinSock 初始化失败，无法建立 TCP 连接");
        notifyState(TransportState::Error);
        return false;
    }
#endif

    notifyState(TransportState::Connecting);

    const SocketFd fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidSocket) {
        notifyMessage("创建 socket 失败：" + lastSocketError());
        notifyState(TransportState::Error);
        return false;
    }

    // Nagle 算法会把小的 Modbus 帧攒起来再发，对轮询式采集是纯延迟。
    // 一条 Modbus 请求也就 12 字节左右，攒包没有任何收益。
    const int noDelay = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                 sizeof(noDelay));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (!parseIpv4(config_.host, address.sin_addr)) {
        notifyMessage("主机地址不是合法的 IPv4：" + config_.host);
        notifyState(TransportState::Error);
        closeSocket(fd);
        return false;
    }

    // 带超时的连接：先切成非阻塞发起 connect，等可写事件，再查 SO_ERROR 确认结果。
    // 直接用阻塞 connect 的话，对端不存在时要等操作系统默认超时（Linux 约 2 分钟），
    // 期间整个采集进程会被卡死 —— 这是现场最不能接受的。
    if (!setBlocking(fd, false)) {
        notifyMessage("设置非阻塞模式失败：" + lastSocketError());
        notifyState(TransportState::Error);
        closeSocket(fd);
        return false;
    }

    const int timeoutMs = config_.connectTimeoutMs > 0 ? config_.connectTimeoutMs : 3000;
    const std::string endpoint = config_.host + ":" + std::to_string(config_.port);

    bool connected = false;
    // 失败原因只记一次：超时和「连接被拒绝」是两种不同的排查方向，
    // 先记下的那条更有信息量（后记的 lastSocketError() 往往已经是无关的残留错误码）。
    std::string failureReason;

    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        connected = true;
    } else if (inProgress()) {
        const int ready = waitFor(fd, false, timeoutMs);
        if (ready == 1) {
            int socketError = 0;
#ifdef _WIN32
            int length = sizeof(socketError);
#else
            socklen_t length = sizeof(socketError);
#endif
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &length);
            connected = (socketError == 0);
            if (!connected) {
                failureReason = "连接 " + endpoint + " 被拒绝：错误码 " +
                                std::to_string(socketError);
            }
        } else if (ready == 0) {
            failureReason =
                "连接 " + endpoint + " 超时（" + std::to_string(timeoutMs) + "ms）";
        } else {
            failureReason = "等待连接就绪失败：" + lastSocketError();
        }
    } else {
        failureReason = "连接 " + endpoint + " 失败：" + lastSocketError();
    }

    if (!connected) {
        notifyMessage(failureReason);
        notifyState(TransportState::Error);
        closeSocket(fd);
        return false;
    }

    // 连上后恢复阻塞：pump() 用 select 驱动读取，不需要非阻塞语义。
    // 阻塞模式下 send 的行为更好预测（要么写完，要么报错）。
    setBlocking(fd, true);

    fd_ = static_cast<int>(fd);
    notifyState(TransportState::Open);
    notifyMessage("已连接 " + describe());
    return true;
}

void NativeTcpTransport::close() {
    if (fd_ < 0) {
        return;
    }
    closeSocket(static_cast<SocketFd>(fd_));
    fd_ = -1;
    readBuffer_.clear();
    notifyState(TransportState::Closed);
}

bool NativeTcpTransport::isOpen() const { return fd_ >= 0; }

bool NativeTcpTransport::send(const uint8_t* data, std::size_t len) {
    if (fd_ < 0) {
        return false;
    }
    if (len == 0) {
        return true;
    }

    std::size_t sent = 0;
    while (sent < len) {
        const int chunk =
#ifdef _WIN32
            ::send(static_cast<SocketFd>(fd_), reinterpret_cast<const char*>(data + sent),
                   static_cast<int>(len - sent), 0);
#else
            static_cast<int>(::send(fd_, data + sent, len - sent, MSG_NOSIGNAL));
#endif
        if (chunk > 0) {
            sent += static_cast<std::size_t>(chunk);
            continue;
        }
        if (chunk < 0) {
            // 发送缓冲满了：等它可写再继续。Modbus 帧很小，几乎不会走到这里，
            // 但「几乎不会」不等于「不会」，现场网络拥塞时就会。
            if (waitFor(static_cast<SocketFd>(fd_), false, 1000) == 1) {
                continue;
            }
            notifyMessage("发送失败：" + lastSocketError());
            notifyState(TransportState::Error);
            close();
            return false;
        }
        // chunk == 0：对端已关闭。
        notifyMessage("发送失败：对端已关闭连接");
        notifyState(TransportState::Error);
        close();
        return false;
    }
    return true;
}

int NativeTcpTransport::pollOnce() {
    if (fd_ < 0) {
        return -1;
    }

    // select 超时 0 = 纯轮询，不阻塞调用方的主循环。
    const int ready = waitFor(static_cast<SocketFd>(fd_), true, 0);
    if (ready == 0) {
        return 0;  // 暂时没数据，正常情况
    }
    if (ready < 0) {
        notifyMessage("等待可读失败：" + lastSocketError());
        notifyState(TransportState::Error);
        close();
        return -1;
    }

    uint8_t buffer[4096];
    const int received =
#ifdef _WIN32
        ::recv(static_cast<SocketFd>(fd_), reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
#else
        static_cast<int>(::recv(fd_, buffer, sizeof(buffer), 0));
#endif

    if (received > 0) {
        // 立刻把字节转给上层，不在这里攒包：
        // 攒包会丢掉「这两帧之间隔了多久」的时间信息，而 RTU 的分帧正依赖帧间静默。
        // 粘包/半包的拼接由 DeviceSession 里的 ByteAccumulator 负责。
        notifyBytes(buffer, static_cast<std::size_t>(received));
        return received;
    }
    if (received == 0) {
        notifyMessage("对端已关闭连接");
        notifyState(TransportState::Closed);
        close();
        return -1;
    }

    // received < 0：非阻塞模式下「暂时无数据」不算错误。
#ifdef _WIN32
    const int code = WSAGetLastError();
    if (code == WSAEWOULDBLOCK) {
        return 0;
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    if (errno == EINTR) {
        return 0;
    }
#endif
    notifyMessage("接收失败：" + lastSocketError());
    notifyState(TransportState::Error);
    close();
    return -1;
}

void NativeTcpTransport::pump() { (void)pollOnce(); }

std::string NativeTcpTransport::describe() const {
    return "tcp://" + config_.host + ":" + std::to_string(config_.port);
}

}  // namespace gateway::transport
