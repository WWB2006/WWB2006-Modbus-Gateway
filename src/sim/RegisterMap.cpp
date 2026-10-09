#include "gateway/sim/RegisterMap.h"

#include <cstdio>

namespace gateway::sim {
namespace {

using protocol::ExceptionCode;

// 位区的打包上限来自规范：一次最多读 2000 位、写 1968 位。
// 超出返回 IllegalDataValue，而不是截断 —— 截断会让主站拿到「少了一半」的数据却不报错。
constexpr uint32_t kMaxReadBits = 2000;
constexpr uint32_t kMaxWriteBits = 1968;
constexpr uint32_t kMaxReadRegisters = 125;
constexpr uint32_t kMaxWriteRegisters = 123;

// 地址 + 数量是否整段落在 [0, size) 内。用 64 位算，避免 start + count 在
// 16 位边界上回绕（例如 start=0xFFFF、count=2）被误判为合法。
bool rangeFits(uint32_t start, uint32_t count, std::size_t size) {
    if (count == 0) {
        return false;
    }
    const uint64_t end = static_cast<uint64_t>(start) + static_cast<uint64_t>(count);
    return end <= static_cast<uint64_t>(size);
}

}  // namespace

// --------------------------------------------------------------------- 分配

void RegisterMap::resizeHoldingRegisters(std::size_t count, uint16_t fill) {
    holding_.assign(count, fill);
}

void RegisterMap::resizeInputRegisters(std::size_t count, uint16_t fill) {
    input_.assign(count, fill);
}

void RegisterMap::resizeCoils(std::size_t count, bool fill) {
    coils_.assign(count, fill);
}

void RegisterMap::resizeDiscreteInputs(std::size_t count, bool fill) {
    discreteInputs_.assign(count, fill);
}

// ----------------------------------------------------------------- 直接访问

void RegisterMap::setHoldingRegister(uint16_t address, uint16_t value) {
    if (address < holding_.size()) {
        holding_[address] = value;
    }
}

void RegisterMap::setInputRegister(uint16_t address, uint16_t value) {
    if (address < input_.size()) {
        input_[address] = value;
    }
}

void RegisterMap::setCoil(uint16_t address, bool value) {
    if (address < coils_.size()) {
        coils_[address] = value;
    }
}

void RegisterMap::setDiscreteInput(uint16_t address, bool value) {
    if (address < discreteInputs_.size()) {
        discreteInputs_[address] = value;
    }
}

uint16_t RegisterMap::holdingRegister(uint16_t address) const {
    return address < holding_.size() ? holding_[address] : static_cast<uint16_t>(0);
}

uint16_t RegisterMap::inputRegister(uint16_t address) const {
    return address < input_.size() ? input_[address] : static_cast<uint16_t>(0);
}

bool RegisterMap::coil(uint16_t address) const {
    return address < coils_.size() ? coils_[address] : false;
}

bool RegisterMap::discreteInput(uint16_t address) const {
    return address < discreteInputs_.size() ? discreteInputs_[address] : false;
}

// ------------------------------------------------------------------- 读取

bool RegisterMap::readHoldingRegisters(uint16_t startAddress, uint16_t count,
                                       std::vector<uint16_t>& out,
                                       ExceptionCode& out_exception) const {
    out.clear();
    if (!rangeFits(startAddress, count, holding_.size())) {
        out_exception = ExceptionCode::IllegalDataAddress;
        return false;
    }
    if (count > kMaxReadRegisters) {
        out_exception = ExceptionCode::IllegalDataValue;
        return false;
    }
    out.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        out.push_back(holding_[startAddress + i]);
    }
    out_exception = ExceptionCode::None;
    return true;
}

bool RegisterMap::readInputRegisters(uint16_t startAddress, uint16_t count,
                                     std::vector<uint16_t>& out,
                                     ExceptionCode& out_exception) const {
    out.clear();
    if (!rangeFits(startAddress, count, input_.size())) {
        out_exception = ExceptionCode::IllegalDataAddress;
        return false;
    }
    if (count > kMaxReadRegisters) {
        out_exception = ExceptionCode::IllegalDataValue;
        return false;
    }
    out.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        out.push_back(input_[startAddress + i]);
    }
    out_exception = ExceptionCode::None;
    return true;
}

bool RegisterMap::readCoils(uint16_t startAddress, uint16_t count, std::vector<bool>& out,
                            ExceptionCode& out_exception) const {
    out.clear();
    if (!rangeFits(startAddress, count, coils_.size())) {
        out_exception = ExceptionCode::IllegalDataAddress;
        return false;
    }
    if (count > kMaxReadBits) {
        out_exception = ExceptionCode::IllegalDataValue;
        return false;
    }
    out.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        out.push_back(coils_[startAddress + i]);
    }
    out_exception = ExceptionCode::None;
    return true;
}

bool RegisterMap::readDiscreteInputs(uint16_t startAddress, uint16_t count,
                                     std::vector<bool>& out, ExceptionCode& out_exception) const {
    out.clear();
    if (!rangeFits(startAddress, count, discreteInputs_.size())) {
        out_exception = ExceptionCode::IllegalDataAddress;
        return false;
    }
    if (count > kMaxReadBits) {
        out_exception = ExceptionCode::IllegalDataValue;
        return false;
    }
    out.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        out.push_back(discreteInputs_[startAddress + i]);
    }
    out_exception = ExceptionCode::None;
    return true;
}

// ------------------------------------------------------------------- 写入

bool RegisterMap::writeSingleRegister(uint16_t address, uint16_t value,
                                      ExceptionCode& out_exception) {
    if (address >= holding_.size()) {
        out_exception = ExceptionCode::IllegalDataAddress;
        return false;
    }
    holding_[address] = value;
    out_exception = ExceptionCode::None;
    return true;
}

bool RegisterMap::writeMultipleRegisters(uint16_t startAddress, const std::vector<uint16_t>& values,
                                         ExceptionCode& out_exception) {
    if (values.empty()) {
        out_exception = ExceptionCode::IllegalDataValue;
        return false;
    }
    // 先判「数量本身是否合法」再判地址：
    // 数量超过规范上限（123）时，无论地址落在哪里都是非法请求，
    // 先报 0x03 才能让主站知道问题出在数量上，而不是让它去查地址。
    if (values.size() > kMaxWriteRegisters) {
        out_exception = ExceptionCode::IllegalDataValue;
        return false;
    }
    if (!rangeFits(startAddress, static_cast<uint16_t>(values.size()), holding_.size())) {
        out_exception = ExceptionCode::IllegalDataAddress;
        return false;
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        holding_[startAddress + i] = values[i];
    }
    out_exception = ExceptionCode::None;
    return true;
}

bool RegisterMap::writeSingleCoil(uint16_t address, bool value, ExceptionCode& out_exception) {
    if (address >= coils_.size()) {
        out_exception = ExceptionCode::IllegalDataAddress;
        return false;
    }
    coils_[address] = value;
    out_exception = ExceptionCode::None;
    return true;
}

bool RegisterMap::writeMultipleCoils(uint16_t startAddress, const std::vector<bool>& values,
                                     ExceptionCode& out_exception) {
    if (values.empty()) {
        out_exception = ExceptionCode::IllegalDataValue;
        return false;
    }
    // 与 writeMultipleRegisters 同理：数量本身非法时先报 0x03。
    if (values.size() > kMaxWriteBits) {
        out_exception = ExceptionCode::IllegalDataValue;
        return false;
    }
    if (!rangeFits(startAddress, static_cast<uint16_t>(values.size()), coils_.size())) {
        out_exception = ExceptionCode::IllegalDataAddress;
        return false;
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        coils_[startAddress + i] = values[i];
    }
    out_exception = ExceptionCode::None;
    return true;
}

// ------------------------------------------------------------------- 输出

std::string RegisterMap::describe() const {
    std::string text;
    char line[128];

    std::snprintf(line, sizeof(line), "保持寄存器 %zu 个 / 输入寄存器 %zu 个 / 线圈 %zu 个 / 离散输入 %zu 个\n",
                  holding_.size(), input_.size(), coils_.size(), discreteInputs_.size());
    text += line;

    const std::size_t show = holding_.size() < 16 ? holding_.size() : 16;
    for (std::size_t i = 0; i < show; ++i) {
        std::snprintf(line, sizeof(line), "  HR[%03zu] = %5u (0x%04X)\n", i,
                      static_cast<unsigned>(holding_[i]), static_cast<unsigned>(holding_[i]));
        text += line;
    }
    if (holding_.size() > show) {
        std::snprintf(line, sizeof(line), "  … 其余 %zu 个略\n", holding_.size() - show);
        text += line;
    }
    return text;
}

}  // namespace gateway::sim
