#pragma once

#include <memory>
#include <string>

#include "gateway/transport/TransportInterface.h"

namespace gateway::transport {

// 链路工厂：按 TransportConfig 造出对应实现（TCP 或串口）。
//
// 为什么需要它：界面层与阶段 11 的无界面网关都要「给一份配置、拿一条链路」，
// 若各自 new 一次具体类型，等于把「用哪种链路」的知识复制了两份，
// 将来加第三条链路（比如串口服务器 UDP 模式）要改两处，必然漏一处。
//
// ⚠️ 实现位于 src/transport/TransportFactory.cpp，依赖 Qt（QObject）。
// 因此**不依赖 Qt 的核心测试不能调用它** —— 测试里请直接用 FakeSlaveTransport。
// 这个约束与项目「协议层 / 设备层不依赖 Qt」的分层原则一致：
// 工厂属于「装配层」，装配层可以依赖 Qt，核心层不行。
//
// 返回 nullptr 表示该链路类型在当前构建里不可用（例如没装 Qt SerialPort
// 却请求串口链路），错误原因由 out_error 给出，避免调用方只看到空指针。
std::unique_ptr<TransportInterface> createTransport(const TransportConfig& config,
                                                     std::string& out_error);

// 检查某类链路在当前构建里是否可用，供界面层提前禁用按钮而不是等 open() 失败。
bool isTransportAvailable(TransportKind kind);

// ---------------------------------------------------------------------------
// 无 Qt 时的链路注册表（阶段 11 用）。
//
// 问题：createTransport / isTransportAvailable 的实现文件包含 TcpTransport.h
// （一个 QObject 子类），所以**整个 TransportFactory.cpp 都依赖 Qt**。
//   - 有 Qt 的构建：工厂直接用 Qt 实现，不需要注册表；
//   - 无 Qt 的构建：工厂这个翻译单元根本编不进来，于是 gateway_daemon
//     这类「不想依赖 Qt 的可执行文件」在没装 Qt 的机器上就没有链路可用。
//
// 解法：把「链路从哪来」抽象成一层可注册的工厂函数。
// 依赖 Qt 的构建在启动时注册 Qt 工厂；测试或无 Qt 场景可以注册假链路工厂。
// 纯逻辑（协议 / 设备 / 存储 / 主循环）因此完全不认识 Qt，
// 与项目「协议层与设备层不依赖 Qt」的分层原则一致。
// ---------------------------------------------------------------------------
using TransportBuilder = std::unique_ptr<TransportInterface> (*)(const TransportConfig& config,
                                                                std::string& out_error);
using TransportAvailability = bool (*)(TransportKind kind);

// 注册 / 覆盖某类链路的构造器。传 nullptr 表示注销。
void registerTransportBuilder(TransportKind kind, TransportBuilder builder,
                              TransportAvailability availability);
void unregisterTransportBuilder(TransportKind kind);

// 通过注册表造链路。未注册时返回 nullptr 并给出可读原因。
std::unique_ptr<TransportInterface> createRegisteredTransport(const TransportConfig& config,
                                                             std::string& out_error);
// 通过注册表查询可用性。未注册 = 不可用。
bool isRegisteredTransportAvailable(TransportKind kind);

}  // namespace gateway::transport
