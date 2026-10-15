#include "gateway/storage/SqliteStorage.h"

#include <sqlite3.h>

#include <sstream>

namespace gateway::storage {

// ======================================================== SqliteStorage

namespace {

// sqlite3 的错误码 -> 可读文案。避免把裸错误码抛给上层。
std::string describeSqliteError(sqlite3* db, int code) {
    std::ostringstream out;
    out << sqlite3_errstr(code);
    if (db != nullptr) {
        const char* message = sqlite3_errmsg(db);
        if (message != nullptr) {
            out << "：" << message;
        }
    }
    return out.str();
}

}  // namespace

SqliteStorage::SqliteStorage(std::string path, std::size_t batchSize)
    : path_(std::move(path)), batchSize_(batchSize == 0 ? 1 : batchSize) {}

SqliteStorage::~SqliteStorage() { close(); }

bool SqliteStorage::open(std::string& error) {
    error.clear();
    if (db_ != nullptr) {
        return true;
    }

    // SQLITE_OPEN_FULLMUTEX：允许跨线程使用同一连接（阶段 9 的并发调度会用到）。
    // 我们在应用层已经串行化写入，这里的互斥只是兜底，不作为并发策略。
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    const int rc = sqlite3_open_v2(path_.c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        error = "打开数据库失败（" + path_ + "）：" + describeSqliteError(db_, rc);
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    // WAL 模式：读写不互相阻塞。工业现场是「持续写入 + 偶尔查询」，
    // 默认的 rollback journal 会让查询卡住写入，WAL 下两者并行。
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    // NORMAL 比 FULL 快很多，WAL 下断电最多丢最后一个事务 —— 对采集数据可接受。
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    if (!createSchema(error)) {
        close();
        return false;
    }
    return true;
}

bool SqliteStorage::createSchema(std::string& error) {
    // samples 表的设计要点：
    //   - 主键 (device, ts_ms, address)：同一设备同一毫秒同一地址只会有一条，
    //     重复写入自动去重（INSERT OR REPLACE），避免重传导致的重复样本；
    //   - ts_ms 用 INTEGER（Unix 毫秒），比 TEXT 快且范围查询友好；
    //   - note 允许为空字符串，不设 NOT NULL 以便老数据迁移。
    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS samples (
            device      TEXT    NOT NULL,
            ts_ms       INTEGER NOT NULL,
            address     INTEGER NOT NULL,
            value       INTEGER NOT NULL,
            function    INTEGER NOT NULL DEFAULT 3,
            valid       INTEGER NOT NULL DEFAULT 1,
            note        TEXT    NOT NULL DEFAULT '',
            PRIMARY KEY (device, ts_ms, address)
        );
        CREATE INDEX IF NOT EXISTS idx_samples_device_ts ON samples (device, ts_ms);
        CREATE INDEX IF NOT EXISTS idx_samples_ts ON samples (ts_ms);
    )SQL";

    char* message = nullptr;
    const int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        error = std::string("建表失败：") + (message != nullptr ? message : sqlite3_errstr(rc));
        if (message != nullptr) {
            sqlite3_free(message);
        }
        return false;
    }
    return true;
}

bool SqliteStorage::ensureIndexes(std::string& error) {
    error.clear();
    if (db_ == nullptr) {
        error = "数据库未打开";
        return false;
    }
    const char* sql = "CREATE INDEX IF NOT EXISTS idx_samples_device_ts ON samples (device, ts_ms);";
    char* message = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        error = std::string("建索引失败：") + (message != nullptr ? message : "");
        if (message != nullptr) {
            sqlite3_free(message);
        }
        return false;
    }
    return true;
}

void SqliteStorage::close() {
    if (db_ == nullptr) {
        return;
    }
    // 关闭前先把批缓冲落盘，否则最近一批数据会丢 —— 这是最容易漏的一步。
    if (!pending_.empty()) {
        std::string error;
        flushLocked(error);
    }
    sqlite3_close(db_);
    db_ = nullptr;
}

bool SqliteStorage::isOpen() const { return db_ != nullptr; }

ArchiveResult SqliteStorage::write(const std::vector<Sample>& samples) {
    ArchiveResult result;
    if (db_ == nullptr) {
        result.ok = false;
        result.error = "数据库未打开";
        return result;
    }

    pending_.insert(pending_.end(), samples.begin(), samples.end());

    if (pending_.size() >= batchSize_) {
        const std::size_t before = pending_.size();
        std::string error;
        if (!flushLocked(error)) {
            result.ok = false;
            result.error = error;
            result.pending = pending_.size();
            return result;
        }
        archived_ += before;
    }
    result.written = samples.size();
    result.archived = archived_;
    result.pending = pending_.size();
    return result;
}

ArchiveResult SqliteStorage::flush() {
    ArchiveResult result;
    if (db_ == nullptr) {
        result.ok = false;
        result.error = "数据库未打开";
        return result;
    }
    const std::size_t before = pending_.size();
    std::string error;
    if (!flushLocked(error)) {
        result.ok = false;
        result.error = error;
        result.pending = pending_.size();
        return result;
    }
    archived_ += before;
    result.written = before;
    result.archived = archived_;
    result.pending = pending_.size();
    return result;
}

bool SqliteStorage::flushLocked(std::string& error) {
    error.clear();
    if (pending_.empty()) {
        return true;
    }

    // 整个批次一个事务 + 一条预编译语句：这是性能的关键。
    char* message = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, &message) != SQLITE_OK) {
        error = std::string("开启事务失败：") + (message != nullptr ? message : "");
        if (message != nullptr) {
            sqlite3_free(message);
        }
        return false;
    }

    sqlite3_stmt* statement = nullptr;
    const char* sql =
        "INSERT OR REPLACE INTO samples (device, ts_ms, address, value, function, valid, note) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK) {
        error = std::string("预编译失败：") + sqlite3_errmsg(db_);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    for (const Sample& sample : pending_) {
        sqlite3_bind_text(statement, 1, sample.deviceName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(sample.timestampMs));
        sqlite3_bind_int(statement, 3, static_cast<int>(sample.address));
        sqlite3_bind_int(statement, 4, static_cast<int>(sample.value));
        sqlite3_bind_int(statement, 5, static_cast<int>(sample.functionCode));
        sqlite3_bind_int(statement, 6, sample.valid ? 1 : 0);
        sqlite3_bind_text(statement, 7, sample.note.c_str(), -1, SQLITE_TRANSIENT);

        const int rc = sqlite3_step(statement);
        if (rc != SQLITE_DONE) {
            error = std::string("写入失败：") + sqlite3_errmsg(db_);
            sqlite3_finalize(statement);
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        // 复位语句以便复用（复用预编译语句是批量写入快的第二个原因：
        // 只编译一次 SQL，之后只换绑定值）。
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    }
    sqlite3_finalize(statement);

    if (sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &message) != SQLITE_OK) {
        error = std::string("提交事务失败：") + (message != nullptr ? message : "");
        if (message != nullptr) {
            sqlite3_free(message);
        }
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    pending_.clear();
    return true;
}

std::vector<Sample> SqliteStorage::query(const std::string& deviceName, uint64_t fromMs,
                                         uint64_t toMs, std::size_t limit) const {
    std::vector<Sample> out;
    if (db_ == nullptr) {
        return out;
    }

    std::string sql =
        "SELECT device, ts_ms, address, value, function, valid, note FROM samples "
        "WHERE ts_ms >= ? AND ts_ms <= ?";
    if (!deviceName.empty()) {
        sql += " AND device = ?";
    }
    sql += " ORDER BY ts_ms, device, address";
    if (limit != 0) {
        sql += " LIMIT ?";
    }
    sql += ";";

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db_);
        return out;
    }

    int index = 1;
    sqlite3_bind_int64(statement, index++, static_cast<sqlite3_int64>(fromMs));
    sqlite3_bind_int64(statement, index++, static_cast<sqlite3_int64>(toMs));
    if (!deviceName.empty()) {
        sqlite3_bind_text(statement, index++, deviceName.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (limit != 0) {
        sqlite3_bind_int64(statement, index++, static_cast<sqlite3_int64>(limit));
    }

    while (sqlite3_step(statement) == SQLITE_ROW) {
        Sample sample;
        const unsigned char* text = sqlite3_column_text(statement, 0);
        sample.deviceName = text != nullptr ? reinterpret_cast<const char*>(text) : "";
        sample.timestampMs = static_cast<uint64_t>(sqlite3_column_int64(statement, 1));
        sample.address = static_cast<uint16_t>(sqlite3_column_int(statement, 2));
        sample.value = static_cast<uint16_t>(sqlite3_column_int(statement, 3));
        sample.functionCode = static_cast<uint8_t>(sqlite3_column_int(statement, 4));
        sample.valid = sqlite3_column_int(statement, 5) != 0;
        const unsigned char* note = sqlite3_column_text(statement, 6);
        sample.note = note != nullptr ? reinterpret_cast<const char*>(note) : "";
        out.push_back(std::move(sample));
    }
    sqlite3_finalize(statement);
    return out;
}

std::vector<std::string> SqliteStorage::devices() const {
    std::vector<std::string> names;
    if (db_ == nullptr) {
        return names;
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT DISTINCT device FROM samples ORDER BY device;", -1,
                           &statement, nullptr) != SQLITE_OK) {
        return names;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(statement, 0);
        if (text != nullptr) {
            names.emplace_back(reinterpret_cast<const char*>(text));
        }
    }
    sqlite3_finalize(statement);
    return names;
}

std::size_t SqliteStorage::totalSamples() const {
    if (db_ == nullptr) {
        return 0;
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM samples;", -1, &statement, nullptr) !=
        SQLITE_OK) {
        return 0;
    }
    std::size_t count = 0;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        count = static_cast<std::size_t>(sqlite3_column_int64(statement, 0));
    }
    sqlite3_finalize(statement);
    return count + pending_.size();
}

std::string SqliteStorage::lastError() const { return lastError_; }

std::string SqliteStorage::describe() const {
    std::ostringstream out;
    out << "sqlite://" << path_ << " (" << totalSamples() << " samples, batch " << batchSize_
        << ", pending " << pending_.size() << ")";
    return out.str();
}

}  // namespace gateway::storage
