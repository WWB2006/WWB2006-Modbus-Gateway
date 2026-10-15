#include "gateway/sim/FaultInjector.h"
#include "gateway/util/Logger.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <thread>
#include <chrono>

using json = nlohmann::json;

namespace gateway::sim {

FaultInjector::FaultInjector()
    : m_rng(std::random_device{}()), m_probDist(0.0, 1.0) {
}

FaultInjector::FaultInjector(FaultProfile profile)
    : m_profile(std::move(profile)), m_rng(std::random_device{}()), m_probDist(0.0, 1.0) {
}

void FaultInjector::setProfile(FaultProfile profile) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_profile = std::move(profile);
}

bool FaultInjector::loadProfileFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        LOG_WARN("Could not open fault profile file " << path);
        return false;
    }

    try {
        json j;
        f >> j;

        std::lock_guard<std::mutex> lock(m_mutex);
        m_profile.name = j.value("name", "custom");

        if (j.contains("delayMs")) {
            m_profile.delay.minMs = j["delayMs"].value("min", 0);
            m_profile.delay.maxMs = j["delayMs"].value("max", 0);
            m_profile.delay.ratio = j["delayMs"].value("ratio", 0.0);
        } else {
            m_profile.delay = {};
        }

        if (j.contains("crcError")) {
            m_profile.crcError.ratio = j["crcError"].value("ratio", 0.0);
        } else {
            m_profile.crcError = {};
        }

        if (j.contains("exception")) {
            m_profile.exception.ratio = j["exception"].value("ratio", 0.0);
            m_profile.exception.code = static_cast<uint8_t>(j["exception"].value("code", 6));
        } else {
            m_profile.exception = {};
        }

        if (j.contains("disconnect")) {
            m_profile.disconnect.afterRequests = j["disconnect"].value("afterRequests", 0);
            m_profile.disconnect.durationMs = j["disconnect"].value("durationMs", 0);
        } else {
            m_profile.disconnect = {};
        }

        LOG_INFO("Loaded fault profile '" << m_profile.name << "' from " << path);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse fault profile JSON: " << e.what());
        return false;
    }
}

int FaultInjector::applyDelay() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_profile.delay.ratio <= 0.0 || m_profile.delay.maxMs <= 0) {
        return 0;
    }

    if (m_probDist(m_rng) <= m_profile.delay.ratio) {
        std::uniform_int_distribution<int> dist(m_profile.delay.minMs, m_profile.delay.maxMs);
        int delay = dist(m_rng);
        if (delay > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            return delay;
        }
    }
    return 0;
}

bool FaultInjector::shouldInjectException(uint8_t& outCode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_profile.exception.ratio <= 0.0) return false;

    if (m_probDist(m_rng) <= m_profile.exception.ratio) {
        outCode = m_profile.exception.code;
        return true;
    }
    return false;
}

bool FaultInjector::shouldInjectCrcError() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_profile.crcError.ratio <= 0.0) return false;
    return m_probDist(m_rng) <= m_profile.crcError.ratio;
}

bool FaultInjector::shouldDisconnect(uint64_t currentRequestCount, int& outDurationMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_profile.disconnect.afterRequests == 0 || m_profile.disconnect.durationMs == 0) {
        return false;
    }

    if (currentRequestCount > 0 && (currentRequestCount % m_profile.disconnect.afterRequests) == 0) {
        outDurationMs = m_profile.disconnect.durationMs;
        return true;
    }
    return false;
}

void FaultInjector::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_profile = {};
}

} // namespace gateway::sim
