#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gateway::transport {

// 接收缓冲区：TCP 是字节流，一次 receive 可能拿到半帧或多帧，必须先攒起来再切帧。
//
// [与项目一对应] MyselfWebServer 里的 Buffer 用的是同一套思路：
//   - 项目一：readIndex_ / writeIndex_ + retrieve / retrieveAll，供 HTTP 状态机反复解析；
//   - 项目二：readIndex_ + consume，供 Modbus 解码器反复切片。
// 差别只在协议：HTTP 靠 \r\n\r\n 分界，Modbus TCP 靠 MBAP 长度字段，Modbus RTU 靠静默间隔。
class ByteAccumulator {
public:
    ByteAccumulator() = default;

    void append(const uint8_t* data, std::size_t len);
    void append(const std::vector<uint8_t>& data) { append(data.data(), data.size()); }

    const uint8_t* data() const noexcept { return buffer_.data() + readIndex_; }
    std::size_t size() const noexcept { return buffer_.size() - readIndex_; }
    bool empty() const noexcept { return size() == 0; }
    std::size_t capacity() const noexcept { return buffer_.size(); }

    // 丢弃前 len 字节；越界时清空。
    void consume(std::size_t len);
    void clear() noexcept;

private:
    std::vector<uint8_t> buffer_;
    std::size_t readIndex_ = 0;
};

}  // namespace gateway::transport
