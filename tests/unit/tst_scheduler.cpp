#include "gateway/device/PollScheduler.h"
#include "gateway/device/DeviceManager.h"
#include "gateway/storage/SqliteStorage.h"
#include "gateway/sim/SlaveSimulator.h"
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdio>

using namespace gateway;

int main() {
    std::cout << "[TEST] Running tst_scheduler..." << std::endl;

    std::string testDb = "test_sched.db";
    remove(testDb.c_str());

    // 1. Start Slave Simulator on port 5120
    sim::SlaveSimulator simulator(1, 5120);
    simulator.setHoldingRegister(0, 1234);
    simulator.setHoldingRegister(1, 5678);
    assert(simulator.startTcpServer());

    // 2. Setup Storage & Manager
    storage::SqliteStorage storage(testDb, 10);
    assert(storage.init());

    device::DeviceManager devManager;
    device::PollScheduler scheduler(devManager, storage);

    std::atomic<int> callbackCount{0};
    scheduler.setRecordCallback([&](const storage::CommunicationRecord& rec) {
        callbackCount++;
        std::cout << "  Polled: " << rec.deviceName << " status=" << rec.status
                  << " vals=[" << rec.valuesStr << "]" << std::endl;
        assert(rec.status == "OK");
    });

    // 3. Configure 2 test devices polling local simulator
    device::DeviceConfig dev1;
    dev1.name = "test-sim-01";
    dev1.transport = "tcp";
    dev1.host = "127.0.0.1";
    dev1.port = 5120;
    dev1.unitId = 1;
    dev1.pollIntervalMs = 150;
    dev1.timeoutMs = 1000;
    dev1.points = {{3, 0, 2}}; // Read 2 holding registers

    std::vector<device::DeviceConfig> configs = {dev1};
    assert(scheduler.start(configs));

    // Wait for at least 5 poll cycles
    std::this_thread::sleep_for(std::chrono::milliseconds(900));

    // Stop scheduler
    scheduler.stop();
    simulator.stopTcpServer();

    std::cout << "  Total callbacks received: " << callbackCount.load() << std::endl;
    assert(callbackCount.load() >= 4);

    // Verify records in DB
    storage::QueryFilter qf;
    int64_t count = storage.countRecords(qf);
    std::cout << "  Records stored in SQLite: " << count << std::endl;
    assert(count >= 4);

    storage.close();
    remove(testDb.c_str());

    std::cout << "[PASS] tst_scheduler passed all tests!" << std::endl;
    return 0;
}
