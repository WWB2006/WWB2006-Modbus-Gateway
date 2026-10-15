#pragma once

#include "gateway/device/DeviceManager.h"
#include "gateway/storage/RecordRepository.h"
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>

namespace gateway::device {

using RecordCallback = std::function<void(const storage::CommunicationRecord&)>;
using StateCallback = std::function<void(const std::string&, DeviceState)>;

class PollScheduler {
public:
    PollScheduler(DeviceManager& devManager, storage::RecordRepository& repository);
    ~PollScheduler();

    void setRecordCallback(RecordCallback cb) { m_recordCallback = std::move(cb); }
    void setStateCallback(StateCallback cb) { m_stateCallback = std::move(cb); }

    bool start(const std::vector<DeviceConfig>& configs);
    void stop();
    bool isRunning() const { return m_running; }

    size_t getActiveWorkerCount() const { return m_workerThreads.size(); }

private:
    void devicePollLoop(DeviceConfig cfg);

    DeviceManager& m_devManager;
    storage::RecordRepository& m_repository;

    std::atomic<bool> m_running{false};
    std::vector<std::thread> m_workerThreads;
    RecordCallback m_recordCallback;
    StateCallback m_stateCallback;
    std::mutex m_schedulerMutex;
};

} // namespace gateway::device
