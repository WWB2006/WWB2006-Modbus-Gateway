#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gateway::storage {

// 一条采集记录。字段刻意保持扁平：工业现场要按「设备 + 地址 + 时间」查历史值，
// 过深的嵌套会让查询与导出都变复杂。
struct Sample {
    uint64_t timestampMs = 0;  // 采集时刻（Unix 毫秒级，由调用方统一时基）
    std::string deviceName;
    uint16_t address = 0;
    uint16_t value = 0;
    uint8_t functionCode = 0x03;
    bool valid = true;
    std::string note;  // 异常/失败原因；正常样本为空
};

// 归档结果的统计，供日志与界面显示。
struct ArchiveResult {
    std::size_t written = 0;   // 本次调用接收的样本条数
    std::size_t archived = 0;  // 累计已真正落盘的样本条数（本实现内）
    std::size_t pending = 0;   // 仍在批缓冲里、尚未落盘的条数
    std::string error;
    bool ok = true;
};

// 存储接口。
//
// 为什么抽象成接口：阶段 6 要 SQLite，但阶段 10 的端到端测试与 CI 里
// 一个纯内存实现能跑得更快、且不需要临时文件。上层（DeviceManager 侧）
// 与测试都只依赖这个接口，换实现不动调用方。
//
// [与项目一对应] 项目一用 Prometheus 暴露指标、日志走 spdlog；
// 这里把「采集到的数据」与「运行指标」分成两条路：
//   - 数据（Sample）走 StorageInterface，要能查历史、能导出 CSV；
//   - 指标（Metrics）走阶段 7 的 MetricsCollector，只保留最近窗口。
// 两者的生命周期完全不同：数据要长期留，指标只需要看近期。
class StorageInterface {
public:
    virtual ~StorageInterface() = default;

    virtual bool open(std::string& error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // 写入一批样本。批量写入是强制的：工业现场一秒几十上百个点，
    // 逐条 fsync 会把磁盘打满。实现内部按 batchSize 攒够再落盘。
    virtual ArchiveResult write(const std::vector<Sample>& samples) = 0;

    // 把缓冲里剩下的样本强制落盘（程序退出、测试断言前调用）。
    virtual ArchiveResult flush() = 0;

    // 按设备名 + 时间范围查询。count 为 0 表示不限条数。
    virtual std::vector<Sample> query(const std::string& deviceName, uint64_t fromMs, uint64_t toMs,
                                      std::size_t limit = 0) const = 0;

    // 已知的所有设备名（去重）。
    virtual std::vector<std::string> devices() const = 0;

    virtual std::size_t totalSamples() const = 0;
    virtual std::string describe() const = 0;
};

// 导出 CSV。字段顺序固定为：timestamp_ms,device,address,value,function,valid,note。
// 冒号/引号/逗号都做转义，避免 note 里的中文标点把列数搞乱。
std::string toCsv(const std::vector<Sample>& samples);
bool exportCsv(const std::string& path, const std::vector<Sample>& samples, std::string& error);

// 从 CSV 行解析回样本（供测试与导入使用）。仅解析本模块导出的格式。
bool parseCsvLine(const std::string& line, Sample& out, std::string& error);

}  // namespace gateway::storage
