#pragma once

#include "gateway/sim/FaultProfile.h"
#include <random>
#include <mutex>
#include <string>

namespace gateway::sim {

class FaultInjector {
public:
    FaultInjector();
    explicit FaultInjector(FaultProfile profile);

    void setProfile(FaultProfile profile);
    bool loadProfileFromFile(const std::string& path);

    const FaultProfile& getProfile() const { return m_profile; }

    /**
     * @brief Evaluates delay fault and sleeps if triggered.
     * @return Number of milliseconds delayed (0 if no delay).
     */
    int applyDelay();

    /**
     * @brief Checks if an exception code should be injected instead of normal response.
     */
    bool shouldInjectException(uint8_t& outCode);

    /**
     * @brief Checks if a CRC error should be injected into RTU frame.
     */
    bool shouldInjectCrcError();

    /**
     * @brief Checks if connection should be dropped.
     */
    bool shouldDisconnect(uint64_t currentRequestCount, int& outDurationMs);

    void reset();

private:
    FaultProfile m_profile;
    std::mt19937 m_rng;
    std::uniform_real_distribution<double> m_probDist;
    std::mutex m_mutex;
};

} // namespace gateway::sim
