#include "gateway/storage/SqliteStorage.h"
#include <iostream>
#include <cassert>
#include <chrono>
#include <fstream>
#include <cstdio>

using namespace gateway::storage;

int main() {
    std::cout << "[TEST] Running tst_storage..." << std::endl;

    std::string testDb = "test_records.db";
    std::string testCsv = "test_export.csv";
    remove(testDb.c_str());
    remove(testCsv.c_str());

    SqliteStorage storage(testDb, 500);
    assert(storage.init());

    // Test 1: Insert 2,000 records and benchmark ingestion speed
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 2000; ++i) {
        CommunicationRecord rec;
        rec.timestampMs = CommunicationRecord::currentTimestampMs() + i;
        rec.deviceName = (i % 2 == 0) ? "plc-01" : "meter-02";
        rec.unitId = (i % 2 == 0) ? 1 : 2;
        rec.functionCode = 3;
        rec.address = static_cast<uint16_t>(i % 100);
        rec.count = 5;
        rec.valuesStr = "100,200,300,400,500";
        rec.latencyMs = 12.5 + (i % 10);
        rec.status = (i % 50 == 0) ? "TIMEOUT" : "OK";
        assert(storage.insertRecord(rec));
    }
    storage.flush();
    auto end = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();
    double recordsPerSec = (2000.0 / elapsedMs) * 1000.0;
    std::cout << "  Inserted 2000 records in " << elapsedMs << "ms ("
              << static_cast<int>(recordsPerSec) << " recs/sec)" << std::endl;

    // Test 2: Count records
    QueryFilter fTotal;
    int64_t totalCount = storage.countRecords(fTotal);
    std::cout << "  Total records in DB: " << totalCount << std::endl;
    assert(totalCount == 2000);

    // Test 3: Filter by deviceName
    QueryFilter fPlc;
    fPlc.deviceName = "plc-01";
    int64_t plcCount = storage.countRecords(fPlc);
    std::cout << "  plc-01 count: " << plcCount << std::endl;
    assert(plcCount == 1000);

    // Test 4: Filter by status
    QueryFilter fTimeout;
    fTimeout.status = "TIMEOUT";
    int64_t timeoutCount = storage.countRecords(fTimeout);
    std::cout << "  TIMEOUT count: " << timeoutCount << std::endl;
    assert(timeoutCount == 40); // 2000 / 50 = 40

    // Test 5: Query with pagination
    QueryFilter fPage;
    fPage.limit = 10;
    fPage.offset = 0;
    auto page0 = storage.queryRecords(fPage);
    assert(page0.size() == 10);

    // Test 6: CSV Export
    QueryFilter fExport;
    assert(storage.exportCsv(testCsv, fExport));

    // Verify CSV exists and count lines
    std::ifstream csvIn(testCsv);
    assert(csvIn.is_open());
    std::string line;
    int lineCount = 0;
    while (std::getline(csvIn, line)) {
        if (!line.empty()) lineCount++;
    }
    csvIn.close();
    std::cout << "  Exported CSV line count (including header): " << lineCount << std::endl;
    assert(lineCount == 2001); // 2000 data rows + 1 header row

    storage.close();

    // Clean up test files
    remove(testDb.c_str());
    remove(testCsv.c_str());

    std::cout << "[PASS] tst_storage passed all tests!" << std::endl;
    return 0;
}
