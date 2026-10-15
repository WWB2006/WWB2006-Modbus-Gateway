#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace gateway::metrics {

// 一次请求的观测结果。
struct Observation {
    uint64_t timestampMs = 0;
    bool success = false;      // 收到有效响应
    bool timedOut = false;
    bool exception = false;    // 设备回了异常码（链路是通的，但业务失败）
    uint32_t latencyMs = 0;    // 从发出到收到响应的耗时
    uint16_t address = 0;
    uint8_t functionCode = 0;
};

// 指标的不可变快照。界面与报告只读这个结构，不直接碰收集器内部。
//
// [与项目一对应] 项目一用 Prometheus 暴露 counter/histogram，
// 由外部抓取、由 Grafana 计算分位数；这里不引入时序数据库，
// 而是在进程内维护一个滑动窗口并**自己算分位数** ——
// 因为本项目要做的是「现场一台机器的自诊断」，没有 Prometheus 那一套基础设施。
struct Snapshot {
    std::size_t windowSize = 0;  // 当前窗口里的样本数
    uint64_t totalRequests = 0;
    uint64_t totalSuccess = 0;
    uint64_t totalTimeout = 0;
    uint64_t totalException = 0;
    double successRate = 0.0;      // 窗口内成功率（0-1）
    double timeoutRate = 0.0;
    uint32_t p50LatencyMs = 0;
    uint32_t p95LatencyMs = 0;
    uint32_t p99LatencyMs = 0;
    uint32_t minLatencyMs = 0;
    uint32_t maxLatencyMs = 0;
    double avgLatencyMs = 0.0;
};

// 指标收集器：维护一个固定容量的滑动窗口，计算成功率与延迟分位数。
//
// 为什么用滑动窗口而不是全量累计：
//   - 全量分位数会「记住」开机时的慢响应，跑一天后当前的分位数完全没有意义；
//   - 工业现场关心的是「现在稳不稳」，窗口取最近 N 次（默认 1000）最合适；
//   - 内存占用固定，不会随时长增长（阶段 7 要求「长稳刷新内存不增长」）。
class Collector {
public:
    explicit Collector(std::size_t windowCapacity = 1000);

    void record(const Observation& observation);
    void reset();

    Snapshot snapshot() const;

    // 分位数计算用最近一次记录的延迟序列。
    const std::deque<Observation>& window() const noexcept { return window_; }

    std::size_t capacity() const noexcept { return capacity_; }

    // 渲染成多行文本，供日志与命令行使用。
    std::string describe() const;

private:
    std::size_t capacity_;
    std::deque<Observation> window_;
    uint64_t totalRequests_ = 0;
    uint64_t totalSuccess_ = 0;
    uint64_t totalTimeout_ = 0;
    uint64_t totalException_ = 0;
};

// 分位数计算：输入未排序的延迟序列，返回指定百分位（0-100）的值。
//
// 用「最近秩」法（nearest-rank）：排序后取 ceil(p/100 * N) - 1 位置。
// 不用插值法，因为延迟本身就是整数毫秒，插值出来的小数没有物理意义，
// 而且现场对「P95 = 47ms」这种整数更容易沟通。空序列返回 0。
uint32_t percentile(std::vector<uint32_t> samples, double percent);

}  // namespace gateway::metrics
