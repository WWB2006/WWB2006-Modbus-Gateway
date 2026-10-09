#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "gateway/protocol/ModbusCodec.h"
#include "gateway/transport/TransportInterface.h"

namespace test_support {

// 测试替身：一个可被脚本控制的 Modbus 从站。
//
// 注意区分它与阶段 3 的 SlaveSimulator：那个是项目的一部分（四区寄存器、故障注入配置、
// 可被 pytest 与其它工具调用）；这里只是测试内部的最小替身，够用就行。
//
// 它有意把响应放进发件箱，由测试显式调用 deliverNext() 才送达对端——
// 真实链路是异步的，同步回调会掩盖再入问题。
class FakeSlaveTransport : public gateway::transport::TransportInterface {
public:
    FakeSlaveTransport(uint8_t unitId, gateway::transport::TransportKind kind)
        : unitId_(unitId), kind_(kind), registers_(64, 0) {
        for (std::size_t i = 0; i < registers_.size(); ++i) {
            registers_[i] = static_cast<uint16_t>(i * 10);
        }
    }

    bool open() override {
        open_ = true;
        notifyState(gateway::transport::TransportState::Open);
        return true;
    }
    void close() override {
        open_ = false;
        notifyState(gateway::transport::TransportState::Closed);
    }
    bool isOpen() const override { return open_; }
    gateway::transport::TransportKind kind() const override { return kind_; }
    std::string describe() const override { return "fake-slave"; }

    bool send(const uint8_t* data, std::size_t len) override {
        ++sentFrames;
        if (!respond_) {
            return true;  // 模拟设备离线：收到请求但不回应
        }
        std::vector<uint8_t> response = buildResponse(data, len);
        if (response.empty()) {
            return true;
        }
        if (corruptCrc_) {
            response.back() ^= 0xFF;
        }
        if (fragment_) {
            const std::size_t half = response.size() / 2;
            outbox_.emplace_back(response.begin(), response.begin() + static_cast<std::ptrdiff_t>(half));
            outbox_.emplace_back(response.begin() + static_cast<std::ptrdiff_t>(half), response.end());
        } else {
            outbox_.push_back(response);
        }
        return true;
    }

    // 测试驱动：把发件箱里最早的一批字节交给对端（模拟数据到达）。
    bool deliverNext() {
        if (outbox_.empty()) {
            return false;
        }
        const std::vector<uint8_t> chunk = outbox_.front();
        outbox_.pop_front();
        notifyBytes(chunk.data(), chunk.size());
        return true;
    }

    std::size_t deliverAll() {
        std::size_t count = 0;
        while (deliverNext()) {
            ++count;
        }
        return count;
    }

    std::size_t pendingChunks() const { return outbox_.size(); }

    // ---- 脚本开关 ----
    void setResponding(bool value) { respond_ = value; }
    void setCorruptCrc(bool value) { corruptCrc_ = value; }
    void setFragment(bool value) { fragment_ = value; }
    void setExceptionCode(uint8_t code) { exceptionCode_ = code; }
    void setResponseUnitId(uint8_t unitId) { responseUnitId_ = unitId; }
    void setResponseTransactionId(uint16_t id) { transactionIdOverride_ = id; }
    void clearResponseTransactionId() { transactionIdOverride_ = -1; }
    void setRegister(std::size_t index, uint16_t value) {
        if (index < registers_.size()) {
            registers_[index] = value;
        }
    }
    uint16_t registerAt(std::size_t index) const {
        return index < registers_.size() ? registers_[index] : 0;
    }

    std::uint64_t sentFrames = 0;

private:
    static uint16_t readU16(const std::vector<uint8_t>& data, std::size_t offset) {
        return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8) |
                                     static_cast<uint16_t>(data[offset + 1]));
    }

    std::vector<uint8_t> buildResponse(const uint8_t* data, std::size_t len) {
        gateway::protocol::DecodeResult decoded;
        std::size_t consumed = 0;
        if (kind_ == gateway::transport::TransportKind::Tcp) {
            consumed = gateway::protocol::decodeTcp(data, len, decoded);
        } else {
            consumed = gateway::protocol::decodeRtuRequest(data, len, decoded);
        }
        if (consumed == 0) {
            return {};
        }

        const gateway::protocol::Frame& frame = decoded.frame;
        const uint8_t functionCode = frame.functionCode;
        std::vector<uint8_t> pdu;

        if (exceptionCode_ != 0) {
            pdu.push_back(static_cast<uint8_t>(functionCode | 0x80));
            pdu.push_back(exceptionCode_);
        } else if (functionCode == 0x03 || functionCode == 0x04) {
            const uint16_t address = readU16(frame.pdu, 0);
            const uint16_t count = readU16(frame.pdu, 2);
            pdu.push_back(functionCode);
            pdu.push_back(static_cast<uint8_t>(count * 2));
            for (uint16_t i = 0; i < count; ++i) {
                const uint16_t value =
                    registers_[(static_cast<std::size_t>(address) + i) % registers_.size()];
                pdu.push_back(static_cast<uint8_t>(value >> 8));
                pdu.push_back(static_cast<uint8_t>(value & 0x00FFu));
            }
        } else if (functionCode == 0x06) {
            const uint16_t address = readU16(frame.pdu, 0);
            const uint16_t value = readU16(frame.pdu, 2);
            registers_[static_cast<std::size_t>(address) % registers_.size()] = value;
            pdu.push_back(functionCode);
            pdu.insert(pdu.end(), frame.pdu.begin(), frame.pdu.begin() + 4);
        } else {
            return {};
        }

        const uint8_t unitId = responseUnitId_ != 0 ? responseUnitId_ : frame.unitId;
        if (kind_ == gateway::transport::TransportKind::Tcp) {
            const uint16_t transactionId =
                transactionIdOverride_ >= 0 ? static_cast<uint16_t>(transactionIdOverride_)
                                            : frame.transactionId;
            return gateway::protocol::encodeTcpRequest(transactionId, unitId, pdu);
        }
        return gateway::protocol::encodeRtuRequest(unitId, pdu);
    }

    uint8_t unitId_ = 1;
    gateway::transport::TransportKind kind_;
    std::vector<uint16_t> registers_;
    std::deque<std::vector<uint8_t>> outbox_;
    bool open_ = false;
    bool respond_ = true;
    bool corruptCrc_ = false;
    bool fragment_ = false;
    uint8_t exceptionCode_ = 0;
    uint8_t responseUnitId_ = 0;
    int transactionIdOverride_ = -1;
};

}  // namespace test_support
