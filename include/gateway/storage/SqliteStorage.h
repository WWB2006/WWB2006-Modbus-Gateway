#pragma once

#include "gateway/storage/RecordRepository.h"
#include <string>
#include <vector>
#include <mutex>

struct sqlite3;
struct sqlite3_stmt;

namespace gateway::storage {

class SqliteStorage : public RecordRepository {
public:
    SqliteStorage(std::string dbPath, size_t batchSize = 500);
    ~SqliteStorage() override;

    bool init() override;
    bool insertRecord(const CommunicationRecord& record) override;
    bool insertBatch(const std::vector<CommunicationRecord>& records) override;
    std::vector<CommunicationRecord> queryRecords(const QueryFilter& filter) override;
    int64_t countRecords(const QueryFilter& filter) override;
    bool exportCsv(const std::string& filePath, const QueryFilter& filter) override;
    void flush() override;

    void close();

private:
    bool executePragmas();
    bool createSchema();

    std::string m_dbPath;
    size_t m_batchSize;
    sqlite3* m_db;
    sqlite3_stmt* m_insertStmt;

    std::vector<CommunicationRecord> m_batchBuffer;
    std::mutex m_mutex;
    bool m_initialized;
};

} // namespace gateway::storage
