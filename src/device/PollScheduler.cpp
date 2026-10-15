#include "gateway/device/PollScheduler.h"
#include "gateway/util/Logger.h"
#include <chrono>

namespace gateway::device {

PollScheduler::PollScheduler(DeviceManager& devManager, storage::RecordRepository& repository)
    : m_devManager(devManager), m_repository(repository) {
}

PollScheduler::~PollScheduler() {
    stop();
}

bool PollScheduler::start(const std::vector<DeviceConfig>& configs) {
    std::lock_guard<std::mutex> lock(m_schedulerMutex);
    if (m_running) return true;

    m_running = true;
    m_workerThreads.clear();

    for (const auto& cfg : configs) {
        m_devManager.addDevice(cfg);
        m_workerThreads.emplace_back(&PollScheduler::devicePollLoop, this, cfg);
    }

    LOG_INFO("PollScheduler started with " << configs.size() << " concurrent device workers");
    return true;
}

void PollScheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(m_schedulerMutex);
        if (!m_running) return;
        m_running = false;
    }

    for (auto& t : m_workerThreads) {
        if (t.joinable()) {
            t.join();
        }
    }
    m_workerThreads.clear();

    m_repository.flush();
    LOG_INFO("PollScheduler stopped and repository flushed");
}

void PollScheduler::devicePollLoop(DeviceConfig cfg) {
    LOG_INFO("Worker thread started for device '" << cfg.name << "' (Interval: " << cfg.pollIntervalMs << "ms)");

    DeviceState lastState = DeviceState::DISCONNECTED;

    while (m_running) {
        auto loopStart = std::chrono::steady_clock::now();

        for (const auto& pt : cfg.points) {
            if (!m_running) break;

            storage::CommunicationRecord rec;
            m_devManager.pollPoint(cfg.name, pt, rec);

            m_repository.insertRecord(rec);

            if (m_recordCallback) {
                m_recordCallback(rec);
            }

            DeviceState curState = m_devManager.getDeviceState(cfg.name);
            if (curState != lastState) {
                lastState = curState;
                if (m_stateCallback) {
                    m_stateCallback(cfg.name, curState);
                }
            }
        }

        if (!m_running) break;

        auto loopEnd = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(loopEnd - loopStart).count();
        int sleepMs = cfg.pollIntervalMs - static_cast<int>(elapsedMs);
        if (sleepMs < 10) sleepMs = 10;

        // Sleep in small increments to respond quickly to stop signal
        int slept = 0;
        while (m_running && slept < sleepMs) {
            int chunk = std::min(50, sleepMs - slept);
            std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
            slept += chunk;
        }
    }

    LOG_INFO("Worker thread finished for device '" << cfg.name << "'");
}

} // namespace gateway::device
