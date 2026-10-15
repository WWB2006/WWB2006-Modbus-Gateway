#include "gateway/storage/MemoryStorage.h"

#include <algorithm>

namespace gateway::storage {

bool MemoryStorage::open(std::string& error) {
    error.clear();
    open_ = true;
    return true;
}

void MemoryStorage::close() { open_ = false; }

ArchiveResult MemoryStorage::write(const std::vector<Sample>& samples) {
    ArchiveResult result;
    if (!open_) {
        result.ok = false;
        result.error = "内存存储未打开";
        return result;
    }
    samples_.insert(samples_.end(), samples.begin(), samples.end());
    result.written = samples.size();
    // 内存实现没有批缓冲，写进即"落盘"，因此 archived 与 totalSamples 一致。
    result.archived = samples_.size();
    result.pending = 0;
    return result;
}

ArchiveResult MemoryStorage::flush() {
    ArchiveResult result;
    result.ok = open_;
    result.archived = samples_.size();
    result.pending = 0;
    if (!open_) {
        result.error = "内存存储未打开";
    }
    return result;
}

std::vector<Sample> MemoryStorage::query(const std::string& deviceName, uint64_t fromMs,
                                        uint64_t toMs, std::size_t limit) const {
    std::vector<Sample> out;
    for (const Sample& sample : samples_) {
        // 空设备名 = 不限设备；时间范围是闭区间。
        if (!deviceName.empty() && sample.deviceName != deviceName) {
            continue;
        }
        if (sample.timestampMs < fromMs || sample.timestampMs > toMs) {
            continue;
        }
        out.push_back(sample);
    }
    // 与 SQLite 实现的 ORDER BY ts_ms, device, address 语义保持一致。
    std::stable_sort(out.begin(), out.end(), [](const Sample& a, const Sample& b) {
        if (a.timestampMs != b.timestampMs) {
            return a.timestampMs < b.timestampMs;
        }
        if (a.deviceName != b.deviceName) {
            return a.deviceName < b.deviceName;
        }
        return a.address < b.address;
    });
    if (limit != 0 && out.size() > limit) {
        out.resize(limit);
    }
    return out;
}

std::vector<std::string> MemoryStorage::devices() const {
    std::vector<std::string> names;
    for (const Sample& sample : samples_) {
        if (std::find(names.begin(), names.end(), sample.deviceName) == names.end()) {
            names.push_back(sample.deviceName);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string MemoryStorage::describe() const {
    return "memory://" + std::to_string(samples_.size()) + " samples";
}

}  // namespace gateway::storage
