#pragma once

#include <cstdint>
#include <atomic>
#include <vector>
#include <mutex>
#include <algorithm>
#include <numeric>
#include <string>
#include <sstream>
#include <iomanip>

namespace gateway::util {

struct MetricsSnapshot {
    uint64_t totalRequests = 0;
    uint64_t successfulRequests = 0;
    uint64_t failedRequests = 0;
    uint64_t timeoutCount = 0;
    uint64_t crcErrorCount = 0;
    uint64_t exceptionCount = 0;
    uint64_t reconnectCount = 0;

    double p50LatencyMs = 0.0;
    double p90LatencyMs = 0.0;
    double p95LatencyMs = 0.0;
    double p99LatencyMs = 0.0;
    double avgLatencyMs = 0.0;

    double successRate() const {
        if (totalRequests == 0) return 100.0;
        return (static_cast<double>(successfulRequests) / totalRequests) * 100.0;
    }

    std::string summary() const {
        std::ostringstream oss;
        oss << "Reqs: " << totalRequests
            << " (OK: " << successfulRequests
            << ", Fail: " << failedRequests
            << ", Timeout: " << timeoutCount
            << ", CRC: " << crcErrorCount
            << ", Ex: " << exceptionCount << ")"
            << " | SuccessRate: " << std::fixed << std::setprecision(1) << successRate() << "%"
            << " | Latency: P50=" << std::setprecision(1) << p50LatencyMs << "ms"
            << " P95=" << p95LatencyMs << "ms"
            << " P99=" << p99LatencyMs << "ms";
        return oss.str();
    }
};

class Metrics {
public:
    static Metrics& instance() {
        static Metrics s_instance;
        return s_instance;
    }

    void recordSuccess(double latencyMs) {
        m_totalRequests.fetch_add(1, std::memory_order_relaxed);
        m_successfulRequests.fetch_add(1, std::memory_order_relaxed);
        recordLatency(latencyMs);
    }

    void recordTimeout() {
        m_totalRequests.fetch_add(1, std::memory_order_relaxed);
        m_failedRequests.fetch_add(1, std::memory_order_relaxed);
        m_timeoutCount.fetch_add(1, std::memory_order_relaxed);
    }

    void recordCrcError() {
        m_totalRequests.fetch_add(1, std::memory_order_relaxed);
        m_failedRequests.fetch_add(1, std::memory_order_relaxed);
        m_crcErrorCount.fetch_add(1, std::memory_order_relaxed);
    }

    void recordException(double latencyMs) {
        m_totalRequests.fetch_add(1, std::memory_order_relaxed);
        m_failedRequests.fetch_add(1, std::memory_order_relaxed);
        m_exceptionCount.fetch_add(1, std::memory_order_relaxed);
        recordLatency(latencyMs);
    }

    void recordReconnect() {
        m_reconnectCount.fetch_add(1, std::memory_order_relaxed);
    }

    void reset() {
        m_totalRequests = 0;
        m_successfulRequests = 0;
        m_failedRequests = 0;
        m_timeoutCount = 0;
        m_crcErrorCount = 0;
        m_exceptionCount = 0;
        m_reconnectCount = 0;
        std::lock_guard<std::mutex> lock(m_latencyMutex);
        m_latencies.clear();
    }

    MetricsSnapshot snapshot() {
        MetricsSnapshot s;
        s.totalRequests = m_totalRequests.load(std::memory_order_relaxed);
        s.successfulRequests = m_successfulRequests.load(std::memory_order_relaxed);
        s.failedRequests = m_failedRequests.load(std::memory_order_relaxed);
        s.timeoutCount = m_timeoutCount.load(std::memory_order_relaxed);
        s.crcErrorCount = m_crcErrorCount.load(std::memory_order_relaxed);
        s.exceptionCount = m_exceptionCount.load(std::memory_order_relaxed);
        s.reconnectCount = m_reconnectCount.load(std::memory_order_relaxed);

        std::vector<double> copy;
        {
            std::lock_guard<std::mutex> lock(m_latencyMutex);
            copy = m_latencies;
        }

        if (!copy.empty()) {
            std::sort(copy.begin(), copy.end());
            double sum = std::accumulate(copy.begin(), copy.end(), 0.0);
            s.avgLatencyMs = sum / copy.size();
            s.p50LatencyMs = getPercentile(copy, 0.50);
            s.p90LatencyMs = getPercentile(copy, 0.90);
            s.p95LatencyMs = getPercentile(copy, 0.95);
            s.p99LatencyMs = getPercentile(copy, 0.99);
        }

        return s;
    }

private:
    Metrics() : m_totalRequests(0), m_successfulRequests(0), m_failedRequests(0),
                m_timeoutCount(0), m_crcErrorCount(0), m_exceptionCount(0), m_reconnectCount(0) {
        m_latencies.reserve(MAX_LATENCY_SAMPLES);
    }

    void recordLatency(double ms) {
        std::lock_guard<std::mutex> lock(m_latencyMutex);
        if (m_latencies.size() >= MAX_LATENCY_SAMPLES) {
            m_latencies.erase(m_latencies.begin());
        }
        m_latencies.push_back(ms);
    }

    static double getPercentile(const std::vector<double>& sorted, double p) {
        if (sorted.empty()) return 0.0;
        size_t idx = static_cast<size_t>(p * sorted.size());
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    }

    static constexpr size_t MAX_LATENCY_SAMPLES = 2000;

    std::atomic<uint64_t> m_totalRequests;
    std::atomic<uint64_t> m_successfulRequests;
    std::atomic<uint64_t> m_failedRequests;
    std::atomic<uint64_t> m_timeoutCount;
    std::atomic<uint64_t> m_crcErrorCount;
    std::atomic<uint64_t> m_exceptionCount;
    std::atomic<uint64_t> m_reconnectCount;

    std::vector<double> m_latencies;
    std::mutex m_latencyMutex;
};

} // namespace gateway::util
