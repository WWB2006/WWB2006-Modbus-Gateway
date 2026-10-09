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

    transport::TransportInterface* rawTransport = entry.transport.get();
    // 用 shared_ptr 保存状态：Entry 会被 push_back 移动，捕获引用会悬空。
    const auto statePtr = entry.state;
    rawTransport->onState([statePtr](transport::TransportState state) { *statePtr = state; });

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
        entry.transport->close();
    }
}

void DeviceManager::tick(uint64_t nowMs) {
    for (Entry& entry : entries_) {
        entry.session->tick(nowMs);
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
            return *entry.state;
        }
    }
    return transport::TransportState::Closed;
}

}  // namespace gateway::device
