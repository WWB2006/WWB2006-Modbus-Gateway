#pragma once

#include "gateway/protocol/ModbusCodec.h"
#include "gateway/sim/FaultInjector.h"
#include <vector>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <thread>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
typedef SOCKET sim_socket_t;
#define SIM_INVALID_SOCKET INVALID_SOCKET
#else
typedef int sim_socket_t;
#define SIM_INVALID_SOCKET (-1)
#endif

namespace gateway::sim {

class SlaveSimulator {
public:
    explicit SlaveSimulator(uint8_t unitId = 1, uint16_t tcpPort = 5020);
    ~SlaveSimulator();

    void setUnitId(uint8_t unitId) { m_unitId = unitId; }
    uint8_t getUnitId() const { return m_unitId; }

    void setPort(uint16_t port) { m_tcpPort = port; }
    uint16_t getPort() const { return m_tcpPort; }

    FaultInjector& getFaultInjector() { return m_faultInjector; }

    // Register access
    void setHoldingRegister(uint16_t address, uint16_t value);
    uint16_t getHoldingRegister(uint16_t address);

    void setInputRegister(uint16_t address, uint16_t value);
    uint16_t getInputRegister(uint16_t address);

    void setCoil(uint16_t address, bool value);
    bool getCoil(uint16_t address);

    void setDiscreteInput(uint16_t address, bool value);
    bool getDiscreteInput(uint16_t address);

    void initDefaultRegisters();

    // Process raw Modbus TCP/RTU requests
    std::vector<uint8_t> processTcpRequest(const uint8_t* data, size_t len);
    std::vector<uint8_t> processRtuRequest(const uint8_t* data, size_t len);

    // Network server
    bool startTcpServer();
    void stopTcpServer();
    bool isRunning() const { return m_running; }

    uint64_t getProcessedRequests() const { return m_requestCount.load(); }

private:
    void serverLoop();
    void clientHandler(sim_socket_t clientSock);

    std::vector<uint8_t> handlePdu(const gateway::protocol::ModbusRequest& req);

    uint8_t m_unitId;
    uint16_t m_tcpPort;
    FaultInjector m_faultInjector;

    std::vector<bool> m_coils;
    std::vector<bool> m_discreteInputs;
    std::vector<uint16_t> m_holdingRegisters;
    std::vector<uint16_t> m_inputRegisters;
    std::mutex m_regMutex;

    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_requestCount{0};
    std::thread m_serverThread;
    sim_socket_t m_serverSocket{SIM_INVALID_SOCKET};
};

} // namespace gateway::sim
