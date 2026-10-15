#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace gateway::storage {

struct CommunicationRecord {
    int64_t id = 0;
    int64_t timestampMs = 0; // Epoch milliseconds
    std::string deviceName;
    uint8_t unitId = 1;
    uint8_t functionCode = 3;
    uint16_t address = 0;
    uint16_t count = 1;
    std::string valuesStr;   // e.g. "10,20,30"
    double latencyMs = 0.0;
    std::string status;      // "OK", "TIMEOUT", "CRC_ERROR", "EXCEPTION_01", etc.

    static int64_t currentTimestampMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

struct QueryFilter {
    std::string deviceName;
    int64_t startTimestampMs = 0;
    int64_t endTimestampMs = 0;
    std::string status;
    int limit = 100;
    int offset = 0;
};

class RecordRepository {
public:
    virtual ~RecordRepository() = default;

    virtual bool init() = 0;
    virtual bool insertRecord(const CommunicationRecord& record) = 0;
    virtual bool insertBatch(const std::vector<CommunicationRecord>& records) = 0;
    virtual std::vector<CommunicationRecord> queryRecords(const QueryFilter& filter) = 0;
    virtual int64_t countRecords(const QueryFilter& filter) = 0;
    virtual bool exportCsv(const std::string& filePath, const QueryFilter& filter) = 0;
    virtual void flush() = 0;
};

} // namespace gateway::storage
