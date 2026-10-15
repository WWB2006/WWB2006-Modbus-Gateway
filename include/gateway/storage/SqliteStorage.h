#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "gateway/storage/StorageInterface.h"

struct sqlite3;

namespace gateway::storage {

// SqliteStorage：SQLite 实现（阶段 6 的主力）。
//
// 为什么用 sqlite3 的 C API 而不是 Qt SQL：
//   1. 项目卖点之一是「没装 Qt 也能构建」——用 QtSql 会把存储层变成 Qt 专属，
//      而存储层恰恰是阶段 11 无界面网关最需要的部分；
//   2. sqlite3 是单文件 amalgamation，vendored 在 third_party/sqlite/ 下，
//      构建时直接编译，无外部依赖、无版本漂移。
class SqliteStorage : public StorageInterface {
public:
    explicit SqliteStorage(std::string path, std::size_t batchSize = 500);
    ~SqliteStorage() override;

    SqliteStorage(const SqliteStorage&) = delete;
    SqliteStorage& operator=(const SqliteStorage&) = delete;

    bool open(std::string& error) override;
    void close() override;
    bool isOpen() const override;

    // 写入时先入内存批缓冲，攒到 batchSize 才在一个事务里落盘。
    // 这是「百万级写入」的关键：逐条 INSERT 每次都要 fsync，同样数据量下
    // 事务批量提交比逐条快两个数量级（见阶段 6 验收记录）。
    ArchiveResult write(const std::vector<Sample>& samples) override;
    ArchiveResult flush() override;

    std::vector<Sample> query(const std::string& deviceName, uint64_t fromMs, uint64_t toMs,
                              std::size_t limit = 0) const override;
    std::vector<std::string> devices() const override;
    std::size_t totalSamples() const override;
    std::string describe() const override;

    // 待落盘样本数（仅内存缓冲，未计入已完成 flush 的部分）。
    std::size_t pendingCount() const { return pending_.size(); }

    // 建索引。大数据量下「按设备 + 时间」查询没有索引会全表扫描。
    bool ensureIndexes(std::string& error);

private:
    bool createSchema(std::string& error);
    bool flushLocked(std::string& error);
    std::string lastError() const;

    std::string path_;
    std::size_t batchSize_;
    sqlite3* db_ = nullptr;
    std::vector<Sample> pending_;
    std::size_t archived_ = 0;  // 累计已提交事务的样本数
    mutable std::string lastError_;
};

}  // namespace gateway::storage
