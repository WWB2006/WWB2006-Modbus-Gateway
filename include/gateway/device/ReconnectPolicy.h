#pragma once

#include <cstdint>
#include <random>

namespace gateway::device {

/**
 * @brief Exponential backoff reconnect policy with jitter.
 * Prevents "thundering herd" reconnection storms when multiple devices disconnect simultaneously.
 */
class ReconnectPolicy {
public:
    ReconnectPolicy(int baseDelayMs = 1000, int maxDelayMs = 30000, int maxRetries = 10)
        : m_baseDelayMs(baseDelayMs), m_maxDelayMs(maxDelayMs), m_maxRetries(maxRetries),
          m_attempt(0), m_rng(std::random_device{}()) {}

    int nextDelayMs() {
        if (m_maxRetries > 0 && m_attempt >= m_maxRetries) {
            return -1; // Max retries exceeded
        }

        // Exponential backoff: base * 2^attempt (capped at maxDelay)
        int shift = (m_attempt > 10) ? 10 : m_attempt;
        int delay = m_baseDelayMs * (1 << shift);
        if (delay > m_maxDelayMs || delay < 0) {
            delay = m_maxDelayMs;
        }

        // Add 0% to 25% randomized jitter
        std::uniform_int_distribution<int> dist(0, delay / 4);
        int jitter = dist(m_rng);
        delay += jitter;

        m_attempt++;
        return delay;
    }

    void reset() {
        m_attempt = 0;
    }

    int currentAttempt() const {
        return m_attempt;
    }

    bool hasExceededMaxRetries() const {
        return m_maxRetries > 0 && m_attempt >= m_maxRetries;
    }

private:
    int m_baseDelayMs;
    int m_maxDelayMs;
    int m_maxRetries;
    int m_attempt;
    std::mt19937 m_rng;
};

} // namespace gateway::device
