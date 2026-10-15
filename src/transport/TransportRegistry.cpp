// 链路注册表的实现。
//
// 刻意**不包含任何 Qt 头文件**：这个翻译单元属于「核心层」，
// 任何构建（含无 Qt 的机器）都会编译它。
// 依赖 Qt 的 createTransport / isTransportAvailable 在 TransportFactory.cpp 里，
// 那份文件才包含 TcpTransport.h。

#include "gateway/transport/TransportFactory.h"

#include <array>

namespace gateway::transport {

namespace {

// 只有两种链路类型，用定长数组比 map 更快也更容易看出「注册的是哪几类」。
constexpr std::size_t kKindCount = 2;

constexpr std::size_t indexOf(TransportKind kind) noexcept {
    switch (kind) {
        case TransportKind::Tcp:
            return 0;
        case TransportKind::Serial:
            return 1;
    }
    return 0;
}

struct Registration {
    TransportBuilder builder = nullptr;
    TransportAvailability availability = nullptr;
};

std::array<Registration, kKindCount>& registry() {
    static std::array<Registration, kKindCount> table{};
    return table;
}

// 每种链路类型的默认拒绝原因，未注册时给调用方一个可操作的提示。
const char* describeMissing(TransportKind kind) {
    switch (kind) {
        case TransportKind::Tcp:
            return "当前构建未注册 TCP 链路（通常是没装 Qt6::Network）。"
                   "如需 TCP 采集请安装 Qt 后重新 cmake，"
                   "或在纯无 Qt 场景下改用测试/模拟链路。";
        case TransportKind::Serial:
            return "当前构建未注册串口链路（通常是没装 Qt6::SerialPort）。"
                   "Ubuntu 请安装 libqt6serialport6-dev 后重新 cmake；"
                   "Windows 请在 Qt 安装器里勾选 SerialPort 后重新 cmake。";
    }
    return "未知的链路类型";
}

}  // namespace

void registerTransportBuilder(TransportKind kind, TransportBuilder builder,
                              TransportAvailability availability) {
    Registration& slot = registry()[indexOf(kind)];
    slot.builder = builder;
    slot.availability = availability;
}

void unregisterTransportBuilder(TransportKind kind) {
    Registration& slot = registry()[indexOf(kind)];
    slot.builder = nullptr;
    slot.availability = nullptr;
}

std::unique_ptr<TransportInterface> createRegisteredTransport(const TransportConfig& config,
                                                             std::string& out_error) {
    out_error.clear();
    const Registration& slot = registry()[indexOf(config.kind)];
    if (slot.builder == nullptr) {
        out_error = describeMissing(config.kind);
        return nullptr;
    }
    return slot.builder(config, out_error);
}

bool isRegisteredTransportAvailable(TransportKind kind) {
    const Registration& slot = registry()[indexOf(kind)];
    if (slot.availability == nullptr) {
        return false;
    }
    return slot.availability(kind);
}

}  // namespace gateway::transport
