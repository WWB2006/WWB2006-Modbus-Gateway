#include "gateway/transport/ByteAccumulator.h"

#include <algorithm>

namespace gateway::transport {

void ByteAccumulator::append(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len == 0) {
        return;
    }
    // 前半段已被消费时，先把剩余数据搬回起点，避免缓冲区无界增长。
    if (readIndex_ > 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(readIndex_));
        readIndex_ = 0;
    }
    buffer_.insert(buffer_.end(), data, data + len);
}

void ByteAccumulator::consume(std::size_t len) {
    if (len >= size()) {
        clear();
        return;
    }
    readIndex_ += len;
}

void ByteAccumulator::clear() noexcept {
    buffer_.clear();
    readIndex_ = 0;
}

}  // namespace gateway::transport
