#include "gateway/device/DeviceManager.h"

namespace gateway::device {

DeviceManager::~DeviceManager() = default;

DeviceSession* DeviceManager::addDevice(
    DeviceConfig config, std::unique_ptr<transport::TransportInterface> transport) {
    if (transport == nullptr) {
        return nullptr;
    }

    Entry entry;
    entry.session = std::make_unique<DeviceSession>(std::move(config));
    entry.transport = std::move(transport);

    const std::string name = entry.session->config().name;
    DeviceSession* session = entry.session.get();
    session->attach(*entry.transport);
    session->setAutoPoll(true);
    session->setResultCallback([this, name](const TransactionResult& result) {
        if (resultCallback_) {
            resultCallback_(name, result);
        }
    });

    // 注意：**不在这里注册 transport->onState**。
    // 链路状态由 DeviceSession::attach 统一接管（它需要状态来触发重连），
    // 这里再注册一次会把会话的回调覆盖掉，导致重连失效。
    // 查状态走 session->linkState()（见 DeviceManager::state）。
    if (linkCallback_) {
        session->setLinkCallback(
            [this, name](DeviceSession::LinkEvent event, const std::string& detail) {
                if (linkCallback_) {
                    linkCallback_(name, event, detail);
                }
            });
    }

    entries_.push_back(std::move(entry));
    return session;
}

bool DeviceManager::startAll() {
    bool allStarted = true;
    for (Entry& entry : entries_) {
        if (!entry.transport->open()) {
            allStarted = false;
        }
    }
    return allStarted;
}

void DeviceManager::stopAll() {
    for (Entry& entry : entries_) {
        // 先告诉会话「是我主动要停」（否则它会把 close 当成掉线而启动重连），
        // 再关闭链路。
        entry.session->stop();
        entry.transport->close();
    }
}

void DeviceManager::tick(uint64_t nowMs) {
    for (Entry& entry : entries_) {
        entry.session->tick(nowMs);
    }
}

void DeviceManager::pump() {
    for (Entry& entry : entries_) {
        entry.session->pump();
    }
}

std::vector<std::string> DeviceManager::deviceNames() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const Entry& entry : entries_) {
        names.push_back(entry.session->config().name);
    }
    return names;
}

DeviceSession* DeviceManager::session(const std::string& name) {
    for (Entry& entry : entries_) {
        if (entry.session->config().name == name) {
            return entry.session.get();
        }
    }
    return nullptr;
}

transport::TransportState DeviceManager::state(const std::string& name) const {
    for (const Entry& entry : entries_) {
        if (entry.session->config().name == name) {
            // 直接问会话要链路状态（唯一所有者），不再自己维护一份。
            return entry.session->linkState();
        }
    }
    return transport::TransportState::Closed;
}

}  // namespace gateway::device
