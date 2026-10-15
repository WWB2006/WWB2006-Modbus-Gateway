#include "gateway/metrics/MetricsCollector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <sstream>

namespace gateway::metrics {

Collector::Collector(std::size_t windowCapacity)
    : capacity_(windowCapacity == 0 ? 1 : windowCapacity) {}

void Collector::record(const Observation& observation) {
    totalRequests_ += 1;
    if (observation.success) {
        totalSuccess_ += 1;
    }
    if (observation.timedOut) {
        totalTimeout_ += 1;
    }
    if (observation.exception) {
        totalException_ += 1;
    }

    window_.push_back(observation);
    // 超过容量就丢最旧的：固定内存，且指标始终反映「最近这一段」。
    while (window_.size() > capacity_) {
        window_.pop_front();
    }
}

void Collector::reset() {
    window_.clear();
    totalRequests_ = 0;
    totalSuccess_ = 0;
    totalTimeout_ = 0;
    totalException_ = 0;
}

uint32_t percentile(std::vector<uint32_t> samples, double percent) {
    if (samples.empty()) {
        return 0;
    }
    if (percent <= 0.0) {
        percent = 0.0;
    }
    if (percent >= 100.0) {
        percent = 100.0;
    }
    std::sort(samples.begin(), samples.end());

    // 最近秩法：排名 = ceil(p/100 * N)，取值 = 第 rank 小的元素（1 基）。
    const double rank = std::ceil(percent / 100.0 * static_cast<double>(samples.size()));
    std::size_t index = static_cast<std::size_t>(rank);
    if (index == 0) {
        index = 1;
    }
    if (index > samples.size()) {
        index = samples.size();
    }
    return samples[index - 1];
}

Snapshot Collector::snapshot() const {
    Snapshot result;
    result.windowSize = window_.size();
    result.totalRequests = totalRequests_;
    result.totalSuccess = totalSuccess_;
    result.totalTimeout = totalTimeout_;
    result.totalException = totalException_;

    if (window_.empty()) {
        return result;
    }

    // 成功率的分母用窗口内样本数，而不是 totalSuccess/totalRequests ——
    // 后者会把「一小时前成功的请求」算进当前的健康度，掩盖正在发生的故障。
    std::size_t success = 0;
    std::size_t timeout = 0;
    std::vector<uint32_t> latencies;
    latencies.reserve(window_.size());
    uint32_t minLatency = 0xFFFFFFFFu;
    uint32_t maxLatency = 0;
    uint64_t sumLatency = 0;

    for (const Observation& observation : window_) {
        if (observation.success) {
            ++success;
        }
        if (observation.timedOut) {
            ++timeout;
        }
        // 延迟只统计有响应的（超时样本的延迟是「等满超时」，不是设备真实响应时间，
        // 把它算进分位数会让 P95 永远等于超时阈值，看不出真实的性能分布）。
        if (observation.success && !observation.timedOut) {
            latencies.push_back(observation.latencyMs);
            minLatency = std::min(minLatency, observation.latencyMs);
            maxLatency = std::max(maxLatency, observation.latencyMs);
            sumLatency += observation.latencyMs;
        }
    }

    const double total = static_cast<double>(window_.size());
    result.successRate = static_cast<double>(success) / total;
    result.timeoutRate = static_cast<double>(timeout) / total;

    if (!latencies.empty()) {
        result.minLatencyMs = minLatency;
        result.maxLatencyMs = maxLatency;
        result.avgLatencyMs =
            static_cast<double>(sumLatency) / static_cast<double>(latencies.size());
        result.p50LatencyMs = percentile(latencies, 50.0);
        result.p95LatencyMs = percentile(latencies, 95.0);
        result.p99LatencyMs = percentile(latencies, 99.0);
    }
    return result;
}

std::string Collector::describe() const {
    const Snapshot snap = snapshot();
    std::ostringstream out;
    char buffer[128];

    std::snprintf(buffer, sizeof(buffer), "窗口 %zu 次（上限 %zu）\n", snap.windowSize, capacity_);
    out << buffer;
    std::snprintf(buffer, sizeof(buffer), "累计请求 %llu / 成功 %llu / 超时 %llu / 异常 %llu\n",
                  static_cast<unsigned long long>(snap.totalRequests),
                  static_cast<unsigned long long>(snap.totalSuccess),
                  static_cast<unsigned long long>(snap.totalTimeout),
                  static_cast<unsigned long long>(snap.totalException));
    out << buffer;
    std::snprintf(buffer, sizeof(buffer), "成功率 %.1f%% / 超时率 %.1f%%\n", snap.successRate * 100.0,
                  snap.timeoutRate * 100.0);
    out << buffer;
    std::snprintf(buffer, sizeof(buffer), "延迟 ms：P50 %u / P95 %u / P99 %u / 均值 %.1f / 范围 %u-%u",
                  snap.p50LatencyMs, snap.p95LatencyMs, snap.p99LatencyMs, snap.avgLatencyMs,
                  snap.minLatencyMs, snap.maxLatencyMs);
    out << buffer;
    return out.str();
}

}  // namespace gateway::metrics
