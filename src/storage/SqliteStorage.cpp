#include "gateway/storage/SqliteStorage.h"
#include "gateway/util/Logger.h"
#include "sqlite3.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(d) mkdir(d, 0755)
#endif

namespace gateway::storage {

static void ensureParentDir(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        std::string dir = path.substr(0, pos);
        MKDIR(dir.c_str());
    }
}

SqliteStorage::SqliteStorage(std::string dbPath, size_t batchSize)
    : m_dbPath(std::move(dbPath)), m_batchSize(batchSize),
      m_db(nullptr), m_insertStmt(nullptr), m_initialized(false) {
}

SqliteStorage::~SqliteStorage() {
    close();
}

void SqliteStorage::close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_batchBuffer.empty() && m_db) {
        insertBatch(m_batchBuffer);
        m_batchBuffer.clear();
    }
    if (m_insertStmt) {
        sqlite3_finalize(m_insertStmt);
        m_insertStmt = nullptr;
    }
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
    m_initialized = false;
}

bool SqliteStorage::init() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return true;

    ensureParentDir(m_dbPath);

    int rc = sqlite3_open(m_dbPath.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Cannot open SQLite database " << m_dbPath << ": " << sqlite3_errmsg(m_db));
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        return false;
    }

    if (!executePragmas() || !createSchema()) {
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }

    const char* insertSql =
        "INSERT INTO records (timestamp, device_name, unit_id, function_code, address, count, values_str, latency_ms, status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    rc = sqlite3_prepare_v2(m_db, insertSql, -1, &m_insertStmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Failed to prepare insert statement: " << sqlite3_errmsg(m_db));
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }

    m_initialized = true;
    LOG_INFO("SQLite storage initialized at " << m_dbPath << " with WAL mode");
    return true;
}

bool SqliteStorage::executePragmas() {
    char* errMsg = nullptr;
    // Enable WAL mode, NORMAL sync, memory temp store, 64MB cache
    const char* pragmaSql =
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA cache_size = -64000;"
        "PRAGMA temp_store = MEMORY;";

    int rc = sqlite3_exec(m_db, pragmaSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        LOG_WARN("SQLite PRAGMA warning: " << (errMsg ? errMsg : ""));
        sqlite3_free(errMsg);
    }
    return true;
}

bool SqliteStorage::createSchema() {
    char* errMsg = nullptr;
    const char* schemaSql =
        "CREATE TABLE IF NOT EXISTS records ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp INTEGER NOT NULL,"
        "  device_name TEXT NOT NULL,"
        "  unit_id INTEGER NOT NULL,"
        "  function_code INTEGER NOT NULL,"
        "  address INTEGER NOT NULL,"
        "  count INTEGER NOT NULL,"
        "  values_str TEXT,"
        "  latency_ms REAL NOT NULL,"
        "  status TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_records_dev_time ON records (device_name, timestamp);"
        "CREATE INDEX IF NOT EXISTS idx_records_time ON records (timestamp);";

    int rc = sqlite3_exec(m_db, schemaSql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Failed to create schema: " << (errMsg ? errMsg : ""));
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool SqliteStorage::insertRecord(const CommunicationRecord& record) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return false;

    m_batchBuffer.push_back(record);
    if (m_batchBuffer.size() >= m_batchSize) {
        bool ok = insertBatch(m_batchBuffer);
        m_batchBuffer.clear();
        return ok;
    }
    return true;
}

bool SqliteStorage::insertBatch(const std::vector<CommunicationRecord>& records) {
    if (records.empty() || !m_db || !m_insertStmt) return true;

    char* errMsg = nullptr;
    sqlite3_exec(m_db, "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg);

    for (const auto& rec : records) {
        sqlite3_reset(m_insertStmt);
        sqlite3_clear_bindings(m_insertStmt);

        sqlite3_bind_int64(m_insertStmt, 1, rec.timestampMs);
        sqlite3_bind_text(m_insertStmt, 2, rec.deviceName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(m_insertStmt, 3, rec.unitId);
        sqlite3_bind_int(m_insertStmt, 4, rec.functionCode);
        sqlite3_bind_int(m_insertStmt, 5, rec.address);
        sqlite3_bind_int(m_insertStmt, 6, rec.count);
        sqlite3_bind_text(m_insertStmt, 7, rec.valuesStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(m_insertStmt, 8, rec.latencyMs);
        sqlite3_bind_text(m_insertStmt, 9, rec.status.c_str(), -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(m_insertStmt);
        if (rc != SQLITE_DONE) {
            LOG_ERROR("Failed to step insert: " << sqlite3_errmsg(m_db));
        }
    }

    sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &errMsg);
    return true;
}

void SqliteStorage::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_batchBuffer.empty() && m_db) {
        insertBatch(m_batchBuffer);
        m_batchBuffer.clear();
    }
}

std::vector<CommunicationRecord> SqliteStorage::queryRecords(const QueryFilter& filter) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CommunicationRecord> results;
    if (!m_initialized || !m_db) return results;

    std::ostringstream sql;
    sql << "SELECT id, timestamp, device_name, unit_id, function_code, address, count, values_str, latency_ms, status "
        << "FROM records WHERE 1=1 ";

    if (!filter.deviceName.empty()) {
        sql << "AND device_name = ? ";
    }
    if (filter.startTimestampMs > 0) {
        sql << "AND timestamp >= ? ";
    }
    if (filter.endTimestampMs > 0) {
        sql << "AND timestamp <= ? ";
    }
    if (!filter.status.empty()) {
        sql << "AND status = ? ";
    }

    sql << "ORDER BY timestamp DESC LIMIT ? OFFSET ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Query prepare failed: " << sqlite3_errmsg(m_db));
        return results;
    }

    int bindIdx = 1;
    if (!filter.deviceName.empty()) {
        sqlite3_bind_text(stmt, bindIdx++, filter.deviceName.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (filter.startTimestampMs > 0) {
        sqlite3_bind_int64(stmt, bindIdx++, filter.startTimestampMs);
    }
    if (filter.endTimestampMs > 0) {
        sqlite3_bind_int64(stmt, bindIdx++, filter.endTimestampMs);
    }
    if (!filter.status.empty()) {
        sqlite3_bind_text(stmt, bindIdx++, filter.status.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, bindIdx++, filter.limit);
    sqlite3_bind_int(stmt, bindIdx++, filter.offset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        CommunicationRecord rec;
        rec.id = sqlite3_column_int64(stmt, 0);
        rec.timestampMs = sqlite3_column_int64(stmt, 1);
        rec.deviceName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        rec.unitId = static_cast<uint8_t>(sqlite3_column_int(stmt, 3));
        rec.functionCode = static_cast<uint8_t>(sqlite3_column_int(stmt, 4));
        rec.address = static_cast<uint16_t>(sqlite3_column_int(stmt, 5));
        rec.count = static_cast<uint16_t>(sqlite3_column_int(stmt, 6));
        const char* valText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        rec.valuesStr = valText ? valText : "";
        rec.latencyMs = sqlite3_column_double(stmt, 8);
        const char* stText = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
        rec.status = stText ? stText : "";
        results.push_back(rec);
    }

    sqlite3_finalize(stmt);
    return results;
}

int64_t SqliteStorage::countRecords(const QueryFilter& filter) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized || !m_db) return 0;

    std::ostringstream sql;
    sql << "SELECT COUNT(*) FROM records WHERE 1=1 ";

    if (!filter.deviceName.empty()) sql << "AND device_name = ? ";
    if (filter.startTimestampMs > 0) sql << "AND timestamp >= ? ";
    if (filter.endTimestampMs > 0) sql << "AND timestamp <= ? ";
    if (!filter.status.empty()) sql << "AND status = ? ";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    int bindIdx = 1;
    if (!filter.deviceName.empty()) sqlite3_bind_text(stmt, bindIdx++, filter.deviceName.c_str(), -1, SQLITE_TRANSIENT);
    if (filter.startTimestampMs > 0) sqlite3_bind_int64(stmt, bindIdx++, filter.startTimestampMs);
    if (filter.endTimestampMs > 0) sqlite3_bind_int64(stmt, bindIdx++, filter.endTimestampMs);
    if (!filter.status.empty()) sqlite3_bind_text(stmt, bindIdx++, filter.status.c_str(), -1, SQLITE_TRANSIENT);

    int64_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

bool SqliteStorage::exportCsv(const std::string& filePath, const QueryFilter& filter) {
    flush();

    std::ofstream out(filePath);
    if (!out.is_open()) {
        LOG_ERROR("Cannot open CSV file for writing: " << filePath);
        return false;
    }

    // CSV Header
    out << "id,timestamp,device_name,unit_id,function_code,address,count,values,latency_ms,status\n";

    QueryFilter streamingFilter = filter;
    streamingFilter.limit = 500;
    streamingFilter.offset = 0;

    while (true) {
        auto chunk = queryRecords(streamingFilter);
        if (chunk.empty()) break;

        for (const auto& rec : chunk) {
            out << rec.id << ","
                << rec.timestampMs << ","
                << rec.deviceName << ","
                << static_cast<int>(rec.unitId) << ","
                << static_cast<int>(rec.functionCode) << ","
                << rec.address << ","
                << rec.count << ","
                << "\"" << rec.valuesStr << "\","
                << rec.latencyMs << ","
                << rec.status << "\n";
        }

        if (chunk.size() < static_cast<size_t>(streamingFilter.limit)) break;
        streamingFilter.offset += streamingFilter.limit;
    }

    out.close();
    LOG_INFO("Exported records to CSV: " << filePath);
    return true;
}

} // namespace gateway::storage
