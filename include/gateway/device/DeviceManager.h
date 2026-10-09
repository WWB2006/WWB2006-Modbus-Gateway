#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gateway/device/DeviceSession.h"
#include "gateway/transport/TransportInterface.h"

namespace gateway::device {

// 设备管理：注册多台设备、维护链路状态、把字节与时钟分发给各自的会话。
//
// 职责边界：
//   - 不解析协议（交给 DeviceSession 与 protocol 层）；
//   - 不自己产生时间（由外部每轮调用 tick）；
//   - 不关心界面（结果通过 ResultCallback 抛给上层）。
//
// [与项目一对应] 项目一的 EventLoopThreadPool 负责把连接分派给多个 IO 线程，
// 每个连接绑定固定线程以保证无锁访问；这里把「一台设备一条链路一个会话」作为隔离单位，
// 思路一致：**状态只在归属者手里改，跨组件只传数据**。
class DeviceManager {
public:
    using ResultCallback =
        std::function<void(const std::string& deviceName, const TransactionResult& result)>;

    DeviceManager() = default;
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    void setResultCallback(ResultCallback callback) { resultCallback_ = std::move(callback); }

    // 接管 transport 的所有权。返回的会话指针在 manager 生命周期内保持有效。
    DeviceSession* addDevice(DeviceConfig config,
                             std::unique_ptr<transport::TransportInterface> transport);

    // TCP 的 open() 是异步连接，返回 true 只表示开始连接；真正的连通状态看 state()。
    bool startAll();
    void stopAll();
    void tick(uint64_t nowMs);

    std::size_t deviceCount() const { return entries_.size(); }
    std::vector<std::string> deviceNames() const;
    DeviceSession* session(const std::string& name);
    transport::TransportState state(const std::string& name) const;

private:
    // 成员顺序即析构顺序（逆序）：state -> transport -> session，
    // 保证链路先拆掉，回调不会再打到一个已经销毁的会话上。
    struct Entry {
        std::unique_ptr<DeviceSession> session;
        std::unique_ptr<transport::TransportInterface> transport;
        std::shared_ptr<transport::TransportState> state =
            std::make_shared<transport::TransportState>(transport::TransportState::Closed);
    };

    std::vector<Entry> entries_;
    ResultCallback resultCallback_;
};

}  // namespace gateway::device
