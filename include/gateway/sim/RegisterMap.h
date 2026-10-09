#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "gateway/protocol/ExceptionCode.h"

namespace gateway::sim {

// ---------------------------------------------------------------------------
// 从站数据区：Modbus 的四区寄存器模型。
//
// 四个区在协议上是彼此独立的地址空间（同一地址 0 在四个区里是四个不同的格子），
// 这是现场最常见的误解来源：手册写「40001」指的是保持寄存器第 0 个，
// 而「10001」是离散输入第 0 个。模拟器必须把四个区分开存，否则无法复现这类问题。
//
//   线圈      Coil            读 0x01 / 写 0x05、0x0F   可读写，1 位
//   离散输入  DiscreteInput   读 0x02                 只读，1 位
//   保持寄存器 HoldingRegister 读 0x03 / 写 0x06、0x10  可读写，16 位
//   输入寄存器 InputRegister   读 0x04                 只读，16 位
//
// 位区与字区都按 16 位地址空间（0x0000-0xFFFF）建模，但只分配实际配置的长度，
// 越界访问返回 IllegalDataAddress —— 这正是模拟器要复现的第一类现场故障。
// ---------------------------------------------------------------------------

class RegisterMap {
public:
    // 只读区（离散输入、输入寄存器）写入时返回 IllegalDataAddress，
    // 与真实设备一致：不存在的可写区域不会被「默默接受」。
    void resizeHoldingRegisters(std::size_t count, uint16_t fill = 0);
    void resizeInputRegisters(std::size_t count, uint16_t fill = 0);
    void resizeCoils(std::size_t count, bool fill = false);
    void resizeDiscreteInputs(std::size_t count, bool fill = false);

    std::size_t holdingSize() const noexcept { return holding_.size(); }
    std::size_t inputSize() const noexcept { return input_.size(); }
    std::size_t coilSize() const noexcept { return coils_.size(); }
    std::size_t discreteInputSize() const noexcept { return discreteInputs_.size(); }

    // 直接读写（供配置加载与测试断言使用，不做地址校验）。
    void setHoldingRegister(uint16_t address, uint16_t value);
    void setInputRegister(uint16_t address, uint16_t value);
    void setCoil(uint16_t address, bool value);
    void setDiscreteInput(uint16_t address, bool value);

    uint16_t holdingRegister(uint16_t address) const;
    uint16_t inputRegister(uint16_t address) const;
    bool coil(uint16_t address) const;
    bool discreteInput(uint16_t address) const;

    // 带越界校验的批量操作，返回 false 时 out_exception 给出应返回的异常码。
    //
    // 校验顺序与规范一致：先判地址范围（0x02），再判数量（0x03）。
    // 反过来的话「地址越界 + 数量超限」会报成 0x03，主站拿到的提示就指向了错误的方向。
    bool readHoldingRegisters(uint16_t startAddress, uint16_t count,
                              std::vector<uint16_t>& out,
                              protocol::ExceptionCode& out_exception) const;
    bool readInputRegisters(uint16_t startAddress, uint16_t count,
                            std::vector<uint16_t>& out,
                            protocol::ExceptionCode& out_exception) const;
    bool readCoils(uint16_t startAddress, uint16_t count, std::vector<bool>& out,
                   protocol::ExceptionCode& out_exception) const;
    bool readDiscreteInputs(uint16_t startAddress, uint16_t count, std::vector<bool>& out,
                            protocol::ExceptionCode& out_exception) const;

    bool writeSingleRegister(uint16_t address, uint16_t value,
                             protocol::ExceptionCode& out_exception);
    bool writeMultipleRegisters(uint16_t startAddress, const std::vector<uint16_t>& values,
                                protocol::ExceptionCode& out_exception);
    bool writeSingleCoil(uint16_t address, bool value, protocol::ExceptionCode& out_exception);
    bool writeMultipleCoils(uint16_t startAddress, const std::vector<bool>& values,
                            protocol::ExceptionCode& out_exception);

    // 供日志与界面用：把当前数据区渲染成多行文本。
    std::string describe() const;

private:
    std::vector<uint16_t> holding_;
    std::vector<uint16_t> input_;
    std::vector<bool> coils_;
    std::vector<bool> discreteInputs_;
};

}  // namespace gateway::sim
