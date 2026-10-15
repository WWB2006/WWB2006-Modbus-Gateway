#include "gateway/sim/SlaveSimulator.h"
#include "gateway/protocol/ModbusCodec.h"
#include "gateway/protocol/Crc16.h"
#include "gateway/util/Logger.h"
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace gateway::sim {

SlaveSimulator::SlaveSimulator(uint8_t unitId, uint16_t tcpPort)
    : m_unitId(unitId), m_tcpPort(tcpPort),
      m_coils(10000, false), m_discreteInputs(10000, false),
      m_holdingRegisters(10000, 0), m_inputRegisters(10000, 0) {
    initDefaultRegisters();
}

SlaveSimulator::~SlaveSimulator() {
    stopTcpServer();
}

void SlaveSimulator::initDefaultRegisters() {
    std::lock_guard<std::mutex> lock(m_regMutex);
    // Initialize holding registers with realistic factory test values
    for (size_t i = 0; i < 100; ++i) {
        m_holdingRegisters[i] = static_cast<uint16_t>((i + 1) * 10); // 10, 20, 30...
    }
    m_holdingRegisters[0] = 235;  // e.g. Voltage 23.5V
    m_holdingRegisters[1] = 485;  // e.g. Current 4.85A
    m_holdingRegisters[2] = 5002; // e.g. Frequency 50.02 Hz
    m_holdingRegisters[3] = 1140; // e.g. Active Power 1.14 kW
    m_holdingRegisters[4] = 68;   // Temperature 68 deg C

    for (size_t i = 0; i < 100; ++i) {
        m_inputRegisters[i] = static_cast<uint16_t>(500 + i);
    }

    m_coils[0] = true;   // Pump running
    m_coils[1] = false;  // Valve closed
    m_coils[2] = true;   // Auto mode enabled
    m_discreteInputs[0] = true; // Emergency switch OK
}

void SlaveSimulator::setHoldingRegister(uint16_t address, uint16_t value) {
    std::lock_guard<std::mutex> lock(m_regMutex);
    if (address < m_holdingRegisters.size()) {
        m_holdingRegisters[address] = value;
    }
}

uint16_t SlaveSimulator::getHoldingRegister(uint16_t address) {
    std::lock_guard<std::mutex> lock(m_regMutex);
    if (address < m_holdingRegisters.size()) {
        return m_holdingRegisters[address];
    }
    return 0;
}

void SlaveSimulator::setInputRegister(uint16_t address, uint16_t value) {
    std::lock_guard<std::mutex> lock(m_regMutex);
    if (address < m_inputRegisters.size()) {
        m_inputRegisters[address] = value;
    }
}

uint16_t SlaveSimulator::getInputRegister(uint16_t address) {
    std::lock_guard<std::mutex> lock(m_regMutex);
    if (address < m_inputRegisters.size()) {
        return m_inputRegisters[address];
    }
    return 0;
}

void SlaveSimulator::setCoil(uint16_t address, bool value) {
    std::lock_guard<std::mutex> lock(m_regMutex);
    if (address < m_coils.size()) {
        m_coils[address] = value;
    }
}

bool SlaveSimulator::getCoil(uint16_t address) {
    std::lock_guard<std::mutex> lock(m_regMutex);
    if (address < m_coils.size()) {
        return m_coils[address];
    }
    return false;
}

void SlaveSimulator::setDiscreteInput(uint16_t address, bool value) {
    std::lock_guard<std::mutex> lock(m_regMutex);
    if (address < m_discreteInputs.size()) {
        m_discreteInputs[address] = value;
    }
}

bool SlaveSimulator::getDiscreteInput(uint16_t address) {
    std::lock_guard<std::mutex> lock(m_regMutex);
    if (address < m_discreteInputs.size()) {
        return m_discreteInputs[address];
    }
    return false;
}

std::vector<uint8_t> SlaveSimulator::handlePdu(const gateway::protocol::ModbusRequest& req) {
    using namespace gateway::protocol;
    std::lock_guard<std::mutex> lock(m_regMutex);

    switch (static_cast<FunctionCode>(req.functionCode)) {
        case FunctionCode::READ_COILS: {
            if (req.count == 0 || req.count > 2000) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_VALUE);
            }
            if (static_cast<size_t>(req.address + req.count) > m_coils.size()) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_ADDRESS);
            }
            std::vector<bool> vals(req.count);
            for (size_t i = 0; i < req.count; ++i) {
                vals[i] = m_coils[req.address + i];
            }
            return ModbusCodec::buildReadCoilsResponsePdu(vals);
        }
        case FunctionCode::READ_DISCRETE_INPUTS: {
            if (req.count == 0 || req.count > 2000) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_VALUE);
            }
            if (static_cast<size_t>(req.address + req.count) > m_discreteInputs.size()) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_ADDRESS);
            }
            std::vector<bool> vals(req.count);
            for (size_t i = 0; i < req.count; ++i) {
                vals[i] = m_discreteInputs[req.address + i];
            }
            return ModbusCodec::buildReadDiscreteInputsResponsePdu(vals);
        }
        case FunctionCode::READ_HOLDING_REGISTERS: {
            if (req.count == 0 || req.count > 125) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_VALUE);
            }
            if (static_cast<size_t>(req.address + req.count) > m_holdingRegisters.size()) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_ADDRESS);
            }
            std::vector<uint16_t> vals(req.count);
            for (size_t i = 0; i < req.count; ++i) {
                vals[i] = m_holdingRegisters[req.address + i];
            }
            return ModbusCodec::buildReadHoldingRegistersResponsePdu(vals);
        }
        case FunctionCode::READ_INPUT_REGISTERS: {
            if (req.count == 0 || req.count > 125) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_VALUE);
            }
            if (static_cast<size_t>(req.address + req.count) > m_inputRegisters.size()) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_ADDRESS);
            }
            std::vector<uint16_t> vals(req.count);
            for (size_t i = 0; i < req.count; ++i) {
                vals[i] = m_inputRegisters[req.address + i];
            }
            return ModbusCodec::buildReadInputRegistersResponsePdu(vals);
        }
        case FunctionCode::WRITE_SINGLE_COIL: {
            if (req.address >= m_coils.size()) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_ADDRESS);
            }
            bool val = !req.coilValues.empty() ? req.coilValues[0] : false;
            m_coils[req.address] = val;
            return ModbusCodec::buildWriteSingleCoilResponsePdu(req.address, val);
        }
        case FunctionCode::WRITE_SINGLE_REGISTER: {
            if (req.address >= m_holdingRegisters.size()) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_ADDRESS);
            }
            uint16_t val = !req.values.empty() ? req.values[0] : 0;
            m_holdingRegisters[req.address] = val;
            return ModbusCodec::buildWriteSingleRegisterResponsePdu(req.address, val);
        }
        case FunctionCode::WRITE_MULTIPLE_COILS: {
            if (req.count == 0 || req.count > 1968) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_VALUE);
            }
            if (static_cast<size_t>(req.address + req.count) > m_coils.size()) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_ADDRESS);
            }
            for (size_t i = 0; i < req.count && i < req.coilValues.size(); ++i) {
                m_coils[req.address + i] = req.coilValues[i];
            }
            return ModbusCodec::buildWriteMultipleCoilsResponsePdu(req.address, req.count);
        }
        case FunctionCode::WRITE_MULTIPLE_REGISTERS: {
            if (req.count == 0 || req.count > 123) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_VALUE);
            }
            if (static_cast<size_t>(req.address + req.count) > m_holdingRegisters.size()) {
                return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_DATA_ADDRESS);
            }
            for (size_t i = 0; i < req.count && i < req.values.size(); ++i) {
                m_holdingRegisters[req.address + i] = req.values[i];
            }
            return ModbusCodec::buildWriteMultipleRegistersResponsePdu(req.address, req.count);
        }
        default:
            return ModbusCodec::buildExceptionPdu(req.functionCode, ModbusException::ILLEGAL_FUNCTION);
    }
}

std::vector<uint8_t> SlaveSimulator::processTcpRequest(const uint8_t* data, size_t len) {
    using namespace gateway::protocol;
    ModbusRequest req;
    size_t consumed = 0;
    CodecStatus status = ModbusCodec::parseTcpRequest(data, len, req, consumed);
    if (status != CodecStatus::OK) {
        return {};
    }

    uint64_t reqIndex = m_requestCount.fetch_add(1);

    // Apply delay fault injection
    m_faultInjector.applyDelay();

    // Check disconnect fault injection
    int disconnectDurationMs = 0;
    if (m_faultInjector.shouldDisconnect(reqIndex, disconnectDurationMs)) {
        LOG_WARN("[SIM] Injected disconnect fault for " << disconnectDurationMs << "ms");
        std::this_thread::sleep_for(std::chrono::milliseconds(disconnectDurationMs));
        return {}; // Drop packet
    }

    // Check exception fault injection
    uint8_t exCode = 0;
    if (m_faultInjector.shouldInjectException(exCode)) {
        std::vector<uint8_t> exPdu = ModbusCodec::buildExceptionPdu(req.functionCode, static_cast<ModbusException>(exCode));
        return ModbusCodec::buildTcpResponseFrame(req.transactionId, req.unitId, exPdu);
    }

    std::vector<uint8_t> respPdu = handlePdu(req);
    return ModbusCodec::buildTcpResponseFrame(req.transactionId, req.unitId, respPdu);
}

std::vector<uint8_t> SlaveSimulator::processRtuRequest(const uint8_t* data, size_t len) {
    using namespace gateway::protocol;
    ModbusRequest req;
    size_t consumed = 0;
    CodecStatus status = ModbusCodec::parseRtuRequest(data, len, req, consumed);
    if (status != CodecStatus::OK) {
        return {};
    }

    uint64_t reqIndex = m_requestCount.fetch_add(1);

    // Apply delay fault injection
    m_faultInjector.applyDelay();

    // Check disconnect fault injection
    int disconnectDurationMs = 0;
    if (m_faultInjector.shouldDisconnect(reqIndex, disconnectDurationMs)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(disconnectDurationMs));
        return {};
    }

    // Check exception fault injection
    uint8_t exCode = 0;
    std::vector<uint8_t> respPdu;
    if (m_faultInjector.shouldInjectException(exCode)) {
        respPdu = ModbusCodec::buildExceptionPdu(req.functionCode, static_cast<ModbusException>(exCode));
    } else {
        respPdu = handlePdu(req);
    }

    std::vector<uint8_t> rtuFrame = ModbusCodec::buildRtuResponseFrame(req.unitId, respPdu);

    // Check CRC corruption fault injection
    if (m_faultInjector.shouldInjectCrcError() && !rtuFrame.empty()) {
        rtuFrame.back() ^= 0xFF;
    }

    return rtuFrame;
}

bool SlaveSimulator::startTcpServer() {
    if (m_running) return true;

#if defined(_WIN32)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    m_serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_serverSocket == SIM_INVALID_SOCKET) {
        LOG_ERROR("SlaveSimulator: failed to create socket");
        return false;
    }

    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_tcpPort);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_serverSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR("SlaveSimulator: failed to bind port " << m_tcpPort);
#if defined(_WIN32)
        closesocket(m_serverSocket);
#else
        close(m_serverSocket);
#endif
        m_serverSocket = SIM_INVALID_SOCKET;
        return false;
    }

    if (listen(m_serverSocket, 16) != 0) {
        LOG_ERROR("SlaveSimulator: failed to listen");
#if defined(_WIN32)
        closesocket(m_serverSocket);
#else
        close(m_serverSocket);
#endif
        m_serverSocket = SIM_INVALID_SOCKET;
        return false;
    }

    m_running = true;
    m_serverThread = std::thread(&SlaveSimulator::serverLoop, this);
    LOG_INFO("SlaveSimulator started listening on TCP port " << m_tcpPort << " (Unit=" << static_cast<int>(m_unitId) << ")");
    return true;
}

void SlaveSimulator::stopTcpServer() {
    if (!m_running) return;
    m_running = false;

    if (m_serverSocket != SIM_INVALID_SOCKET) {
#if defined(_WIN32)
        closesocket(m_serverSocket);
#else
        close(m_serverSocket);
#endif
        m_serverSocket = SIM_INVALID_SOCKET;
    }

    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
    LOG_INFO("SlaveSimulator stopped");
}

void SlaveSimulator::serverLoop() {
    while (m_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(m_serverSocket, &readSet);

        timeval tv{0, 200000}; // 200ms timeout to allow checking m_running
        int sel = select(static_cast<int>(m_serverSocket + 1), &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        sim_socket_t clientSock = accept(m_serverSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientSock == SIM_INVALID_SOCKET) continue;

        // Spawn client thread
        std::thread([this, clientSock]() {
            this->clientHandler(clientSock);
        }).detach();
    }
}

void SlaveSimulator::clientHandler(sim_socket_t clientSock) {
    int flag = 1;
    setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));

    uint8_t buffer[2048];
    while (m_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(clientSock, &readSet);

        timeval tv{1, 0}; // 1 sec select
        int sel = select(static_cast<int>(clientSock + 1), &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) {
            if (sel < 0) break;
            continue;
        }

        int n = ::recv(clientSock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (n <= 0) break;

        std::vector<uint8_t> resp = processTcpRequest(buffer, static_cast<size_t>(n));
        if (!resp.empty()) {
            ::send(clientSock, reinterpret_cast<const char*>(resp.data()), static_cast<int>(resp.size()), 0);
        }
    }

#if defined(_WIN32)
    closesocket(clientSock);
#else
    close(clientSock);
#endif
}

} // namespace gateway::sim
