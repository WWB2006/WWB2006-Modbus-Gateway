#include "gateway/transport/TransportFactory.h"

#include "gateway/transport/TcpTransport.h"

#if defined(GATEWAY_HAS_SERIALPORT)
#include "gateway/transport/SerialTransport.h"
#endif

namespace gateway::transport {

// ---------------------------------------------------------------------------
// 静态注册：把 Qt 实现挂到无 Qt 的注册表上（见 TransportFactory.h 的说明）。
//
// 用一个文件级对象的构造函数完成注册 —— 它在 main() 之前执行，
// 因此任何能链到本翻译单元的进程一启动就已经具备 Qt 链路。
// 为什么不用显式 init 函数：那样每个入口（gateway_cli / daemon / 界面 / 测试）
// 都得记得调一次，漏掉一处就是「链路莫名不可用」，正是本项目反复踩过的坑。
// ---------------------------------------------------------------------------
namespace {

std::unique_ptr<TransportInterface> buildTcp(const TransportConfig& config,
                                             std::string& out_error) {
    out_error.clear();
    return std::make_unique<TcpTransport>(config);
}

bool tcpAvailable(TransportKind kind) { return kind == TransportKind::Tcp; }

#if defined(GATEWAY_HAS_SERIALPORT)
std::unique_ptr<TransportInterface> buildSerial(const TransportConfig& config,
                                                std::string& out_error) {
    out_error.clear();
    return std::make_unique<SerialTransport>(config);
}

bool serialAvailable(TransportKind kind) { return kind == TransportKind::Serial; }
#endif

struct QtTransportRegistrar {
    QtTransportRegistrar() {
        registerTransportBuilder(TransportKind::Tcp, &buildTcp, &tcpAvailable);
#if defined(GATEWAY_HAS_SERIALPORT)
        registerTransportBuilder(TransportKind::Serial, &buildSerial, &serialAvailable);
#endif
    }
};

// 局部静态对象：既保证「main 之前注册」，又避免跨翻译单元的静态初始化顺序问题
// （若这里改成全局非局部对象，注册表本身可能还没构造完 —— 局部静态没有这个风险）。
const QtTransportRegistrar& qtRegistrar() {
    static const QtTransportRegistrar registrar;
    return registrar;
}

}  // namespace

std::unique_ptr<TransportInterface> createTransport(const TransportConfig& config,
                                                    std::string& out_error) {
    out_error.clear();

    // 先确保注册已完成（首次调用时触发器构造）。
    (void)qtRegistrar();

    switch (config.kind) {
        case TransportKind::Tcp:
            return std::make_unique<TcpTransport>(config);

        case TransportKind::Serial:
#if defined(GATEWAY_HAS_SERIALPORT)
            return std::make_unique<SerialTransport>(config);
#else
            out_error =
                "当前构建未启用 Qt SerialPort 模块，串口链路不可用。"
                "Ubuntu 请安装 libqt6serialport6-dev 后重新 cmake；"
                "Windows 请在 Qt 安装器里勾选 SerialPort 后重新 cmake。";
            return nullptr;
#endif
    }

    out_error = "未知的链路类型";
    return nullptr;
}

bool isTransportAvailable(TransportKind kind) {
    // 与 createTransport 同样先确保注册表可用。
    (void)qtRegistrar();

    switch (kind) {
        case TransportKind::Tcp:
            return true;
        case TransportKind::Serial:
#if defined(GATEWAY_HAS_SERIALPORT)
            return true;
#else
            return false;
#endif
    }
    return false;
}

}  // namespace gateway::transport
