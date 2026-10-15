#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "gateway/storage/StorageInterface.h"

namespace gateway::storage {

// MemoryStorage：纯内存实现。
//
// 存在的意义：阶段 10 的端到端测试、CI 与「只想跑一下看看」的场景
// 不需要落地到磁盘。它同时是 SQLite 实现的对照物 ——
// 两个实现跑同一批断言，能暴露「查询语义不一致」这类实现细节泄漏。
class MemoryStorage : public StorageInterface {
public:
    MemoryStorage() = default;
    ~MemoryStorage() override = default;

    bool open(std::string& error) override;
    void close() override;
    bool isOpen() const override { return open_; }
    ArchiveResult write(const std::vector<Sample>& samples) override;
    ArchiveResult flush() override;
    std::vector<Sample> query(const std::string& deviceName, uint64_t fromMs, uint64_t toMs,
                              std::size_t limit = 0) const override;
    std::vector<std::string> devices() const override;
    std::size_t totalSamples() const override { return samples_.size(); }
    std::string describe() const override;

private:
    std::vector<Sample> samples_;
    bool open_ = false;
};

}  // namespace gateway::storage
