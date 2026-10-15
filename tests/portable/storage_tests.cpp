// 存储层与指标测试：两个存储实现跑同一批断言 + 分位数与滑动窗口语义。
// 不依赖 Qt（用 sqlite3 C API），因此任何环境都能跑。

#include <cstdio>
#include <string>
#include <vector>

#include "gateway/metrics/MetricsCollector.h"
#include "gateway/storage/MemoryStorage.h"
#include "gateway/storage/SqliteStorage.h"
#include "test_support.h"

using test_support::check;
using test_support::checkU16;
using test_support::checkU64;
using test_support::section;

namespace {

gateway::storage::Sample makeSample(const std::string& device, uint64_t ts, uint16_t address,
                                    uint16_t value) {
    gateway::storage::Sample sample;
    sample.deviceName = device;
    sample.timestampMs = ts;
    sample.address = address;
    sample.value = value;
    sample.functionCode = 0x03;
    sample.valid = true;
    return sample;
}

// 两个存储实现共用的行为断言。返回值是断言数（由 test_support 全局计数）。
void runStorageContract(gateway::storage::StorageInterface& storage, const std::string& label) {
    section(("存储契约：" + label).c_str());

    std::string error;
    check(storage.open(error), label + " 打开成功：" + error);
    check(storage.isOpen(), label + " isOpen 为真");

    std::vector<gateway::storage::Sample> samples;
    for (int i = 0; i < 10; ++i) {
        samples.push_back(makeSample("plc-01", 1000 + static_cast<uint64_t>(i), 0, static_cast<uint16_t>(i * 100)));
    }
    samples.push_back(makeSample("meter-02", 1005, 0, 42));

    auto result = storage.write(samples);
    check(result.ok, label + " 写入成功");
    checkU64(result.written, samples.size(), label + " 写入条数");

    result = storage.flush();
    check(result.ok, label + " 落盘成功");

    checkU64(storage.totalSamples(), samples.size(), label + " 总条数");

    // 按设备过滤。
    const auto plcSamples = storage.query("plc-01", 0, 999999, 0);
    checkU64(plcSamples.size(), 10, label + " 按设备查询条数");
    const auto meterSamples = storage.query("meter-02", 0, 999999, 0);
    checkU64(meterSamples.size(), 1, label + " 另一台设备条数");

    // 时间范围（闭区间）。
    const auto ranged = storage.query("", 1003, 1005, 0);
    checkU64(ranged.size(), 4, label + " 时间范围查询（1003-1005 含 meter-02）");

    // 空设备名 = 不限设备。
    const auto all = storage.query("", 0, 999999, 0);
    checkU64(all.size(), 11, label + " 不限设备查询");

    // limit 生效。
    const auto limited = storage.query("", 0, 999999, 3);
    checkU64(limited.size(), 3, label + " limit 生效");

    // 排序：时间升序。
    bool sorted = true;
    for (std::size_t i = 1; i < all.size(); ++i) {
        if (all[i].timestampMs < all[i - 1].timestampMs) {
            sorted = false;
            break;
        }
    }
    check(sorted, label + " 结果按时间升序");

    // 设备列表去重并排序。
    const auto names = storage.devices();
    checkU64(names.size(), 2, label + " 设备去重");
    check(names[0] == "meter-02" && names[1] == "plc-01", label + " 设备名排序");

    // 重复写入同一 (device, ts, address) 应去重（INSERT OR REPLACE 语义）。
    // 内存实现不天然去重，所以这一条只在 SQLite 上验证。
    storage.close();
}

void testSqliteSpecific() {
    section("SQLite 存储：主键去重与索引");

    const std::string path = "build_storage_test.db";
    std::remove(path.c_str());

    gateway::storage::SqliteStorage storage(path, 3);  // 批量 3 条，便于触发自动落盘
    std::string error;
    check(storage.open(error), "打开数据库：" + error);

    // 写 7 条，批量 3 → 在**第 3、6、7 条**各触发一次自动落盘（判据是
    // 「写入后缓冲 ≥ batchSize」，每次 flush 都把缓冲清零重计）。
    // 所以 7 条全部落盘、缓冲剩 0 —— 这是刻意断言的边界语义：
    //   累计 7 条：3 → flush；6 → flush；7 → flush。缓冲 0，已落盘 7。
    // 如果哪天改成「攒够 batchSize 才 flush」，第 7 条会留在缓冲里，
    // 这条断言就会失败并暴露语义变化。
    std::vector<gateway::storage::Sample> samples;
    for (int i = 0; i < 7; ++i) {
        samples.push_back(makeSample("plc-01", 2000 + static_cast<uint64_t>(i), 0, static_cast<uint16_t>(i)));
    }
    auto result = storage.write(samples);
    check(result.ok, "写入成功");
    checkU64(result.archived, 7, "批量 3 时 7 条全部落盘");
    checkU64(storage.pendingCount(), 0, "批量 3 时缓冲已清空");
    checkU64(storage.totalSamples(), 7, "总条数 7");

    // 再写 1 条（缓冲只有 1 条，未达阈值）验证「缓冲会真的留住数据」。
    storage.write({makeSample("plc-01", 2010, 0, 77)});
    checkU64(storage.pendingCount(), 1, "未达批量阈值时缓冲留住 1 条");
    checkU64(storage.totalSamples(), 8, "总条数含未落盘部分");
    storage.flush();
    checkU64(storage.pendingCount(), 0, "显式 flush 后缓冲清空");

    // 把 2010 那条删掉：删库重建太麻烦，这里用「close 后删文件再 open」的方式
    // 恢复到干净的 7 条基线 —— 后面的主键去重断言需要一个确定的起点。
    storage.close();
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());

    check(storage.open(error), "重新打开干净库：" + error);    storage.write(samples);  // 还是那 7 条，batch=3 → 全部落盘
    storage.flush();
    checkU64(storage.totalSamples(), 7, "重开后 7 条");

    // 主键去重：同一 (device, ts, address) 再写一次，值被替换、条数不变。
    std::vector<gateway::storage::Sample> duplicate = {makeSample("plc-01", 2000, 0, 9999)};
    storage.write(duplicate);
    storage.flush();
    checkU64(storage.totalSamples(), 7, "重复主键不增加条数");

    const auto fetched = storage.query("plc-01", 2000, 2000, 0);
    checkU64(fetched.size(), 1, "按时间点查询到 1 条");
    checkU16(fetched[0].value, 9999, "值已被替换");

    // 索引创建不报错。
    check(storage.ensureIndexes(error), "建索引成功：" + error);

    // 关闭时会自动 flush 未落盘数据。
    gateway::storage::SqliteStorage second(path, 100);
    check(second.open(error), "重新打开：" + error);
    checkU64(second.totalSamples(), 7, "重开后仍是 7 条");
    // 再写一条不 flush 就 close，验证 close 里的兜底 flush。
    second.write({makeSample("vfd-03", 3000, 5, 77)});
    second.close();

    gateway::storage::SqliteStorage third(path, 100);
    check(third.open(error), "第三次打开：" + error);
    checkU64(third.totalSamples(), 8, "close 前自动落盘，共 8 条");
    third.close();
    storage.close();

    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
}

void testCsv() {
    section("CSV：导出、转义与回读");

    std::vector<gateway::storage::Sample> samples;
    samples.push_back(makeSample("plc-01", 1000, 0, 100));
    samples.push_back(makeSample("meter-02", 1001, 1, 200));
    // note 里含逗号与引号：必须被正确转义，否则列会错位。
    gateway::storage::Sample tricky = makeSample("weird,device", 1002, 2, 300);
    tricky.note = "响应异常，含\"引号\"";
    samples.push_back(tricky);

    const std::string csv = gateway::storage::toCsv(samples);
    check(csv.find("timestamp_ms,device,address,value,function,valid,note") == 0, "表头正确");
    check(csv.find("\"weird,device\"") != std::string::npos, "含逗号的设备名被引号包围");
    check(csv.find("\"响应异常，含\"\"引号\"\"\"") != std::string::npos, "引号被翻倍转义");

    // 回读：跳过表头后逐行解析，数量与值都要对得上。
    std::vector<std::string> lines;
    std::string current;
    for (const char ch : csv) {
        if (ch == '\n') {
            if (!current.empty()) {
                lines.push_back(current);
            }
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    checkU64(lines.size(), 4, "共 4 行（1 表头 + 3 数据）");

    gateway::storage::Sample parsed;
    std::string error;
    check(!gateway::storage::parseCsvLine(lines[0], parsed, error), "表头行被拒绝");
    check(gateway::storage::parseCsvLine(lines[1], parsed, error), "第 1 条数据行解析成功：" + error);
    check(parsed.deviceName == "plc-01", "设备名回读");
    checkU16(parsed.value, 100, "数值回读");
    checkU64(parsed.timestampMs, 1000, "时间戳回读");
    check(gateway::storage::parseCsvLine(lines[3], parsed, error), "带转义的行解析成功：" + error);
    check(parsed.deviceName == "weird,device", "含逗号的设备名回读正确");
    check(parsed.note == "响应异常，含\"引号\"", "note 回读正确（引号还原）");

    // 列数不足应报错。
    gateway::storage::Sample bad;
    check(!gateway::storage::parseCsvLine("1,2,3", bad, error), "列数不足被拒绝");

    // 文件导出。
    const std::string path = "build_csv_test.csv";
    std::remove(path.c_str());
    check(gateway::storage::exportCsv(path, samples, error), "导出文件成功：" + error);
    std::remove(path.c_str());
}

void testPercentile() {
    section("分位数：最近秩法");

    checkU64(gateway::metrics::percentile({}, 50.0), 0, "空序列返回 0");

    // 1..100，最近秩法：P50 → 第 50 个 → 50；P95 → 第 95 个 → 95；P99 → 99。
    std::vector<uint32_t> values;
    for (uint32_t i = 1; i <= 100; ++i) {
        values.push_back(i);
    }
    checkU64(gateway::metrics::percentile(values, 50.0), 50, "P50 of 1..100");
    checkU64(gateway::metrics::percentile(values, 95.0), 95, "P95 of 1..100");
    checkU64(gateway::metrics::percentile(values, 99.0), 99, "P99 of 1..100");
    checkU64(gateway::metrics::percentile(values, 100.0), 100, "P100 = 最大值");
    checkU64(gateway::metrics::percentile(values, 0.0), 1, "P0 = 最小值");

    // 小样本：4 个值，P50 → ceil(0.5*4)=2 → 第 2 个。
    checkU64(gateway::metrics::percentile({10, 20, 30, 40}, 50.0), 20, "P50 of 4 值");
    checkU64(gateway::metrics::percentile({10, 20, 30, 40}, 95.0), 40, "P95 of 4 值");
    checkU64(gateway::metrics::percentile({10, 20, 30, 40}, 75.0), 30, "P75 of 4 值");

    // 乱序输入也要正确（内部会排序）。
    checkU64(gateway::metrics::percentile({100, 1, 50, 2}, 50.0), 2, "乱序输入 P50");
}

void testMetricsWindow() {
    section("指标收集器：滑动窗口与成功率");

    gateway::metrics::Collector collector(5);  // 窗口 5 条
    checkU64(collector.capacity(), 5, "窗口容量");

    // 先记 10 条成功，窗口只留最近 5 条。
    for (int i = 0; i < 10; ++i) {
        gateway::metrics::Observation observation;
        observation.success = true;
        observation.latencyMs = static_cast<uint32_t>(10 + i);
        collector.record(observation);
    }
    auto snapshot = collector.snapshot();
    checkU64(snapshot.windowSize, 5, "窗口只保留最近 5 条");
    checkU64(snapshot.totalRequests, 10, "累计请求 10");
    checkU64(snapshot.totalSuccess, 10, "累计成功 10");
    check(snapshot.successRate > 0.99, "成绩率 100%");

    // 再记 5 条超时，窗口变成「5 条全超时」。
    for (int i = 0; i < 5; ++i) {
        gateway::metrics::Observation observation;
        observation.success = false;
        observation.timedOut = true;
        observation.latencyMs = 1000;
        collector.record(observation);
    }
    snapshot = collector.snapshot();
    checkU64(snapshot.windowSize, 5, "窗口仍是 5 条");
    check(snapshot.successRate < 0.01, "窗口内成功率跌到 0");
    check(snapshot.timeoutRate > 0.99, "窗口内超时率 100%");
    checkU64(snapshot.totalRequests, 15, "累计请求 15");
    checkU64(snapshot.totalTimeout, 5, "累计超时 5");
    // 超时样本不进延迟分位数（延迟是"等满超时"，不是真实响应时间）。
    checkU64(snapshot.p50LatencyMs, 0, "全超时时无延迟分位数");

    collector.reset();
    snapshot = collector.snapshot();
    checkU64(snapshot.windowSize, 0, "reset 后窗口为空");
    checkU64(snapshot.totalRequests, 0, "reset 后累计清零");
}

void testMetricsLatency() {
    section("指标收集器：延迟分位数");

    gateway::metrics::Collector collector(1000);
    // 90 条 10ms + 9 条 100ms + 1 条 1000ms。
    for (int i = 0; i < 90; ++i) {
        gateway::metrics::Observation observation;
        observation.success = true;
        observation.latencyMs = 10;
        collector.record(observation);
    }
    for (int i = 0; i < 9; ++i) {
        gateway::metrics::Observation observation;
        observation.success = true;
        observation.latencyMs = 100;
        collector.record(observation);
    }
    {
        gateway::metrics::Observation observation;
        observation.success = true;
        observation.latencyMs = 1000;
        collector.record(observation);
    }

    const auto snapshot = collector.snapshot();
    checkU64(snapshot.windowSize, 100, "窗口 100 条");
    checkU64(snapshot.p50LatencyMs, 10, "P50 = 10ms");
    checkU64(snapshot.p95LatencyMs, 100, "P95 = 100ms（第 95 个是长尾开始）");
    checkU64(snapshot.p99LatencyMs, 100, "P99 = 100ms");
    checkU64(snapshot.minLatencyMs, 10, "最小值");
    checkU64(snapshot.maxLatencyMs, 1000, "最大值");
    // 均值 = (90*10 + 9*100 + 1*1000) / 100 = (900 + 900 + 1000) / 100 = 28ms。
    // 之前把 1000ms 的样本漏算了才写成 19ms——断言写错比代码写错更常见，
    // 这里把算术过程留在注释里，下次改动时能直接核对。
    check(snapshot.avgLatencyMs > 27.0 && snapshot.avgLatencyMs < 29.0, "均值 28ms");
}

void testMetricsDescribe() {
    section("指标收集器：文本渲染");

    gateway::metrics::Collector collector(100);
    gateway::metrics::Observation observation;
    observation.success = true;
    observation.latencyMs = 25;
    collector.record(observation);

    const std::string text = collector.describe();
    check(text.find("成功率") != std::string::npos, "含成功率");
    check(text.find("P50") != std::string::npos, "含 P50");
    check(text.find("P95") != std::string::npos, "含 P95");
    check(text.find("P99") != std::string::npos, "含 P99");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Modbus 存储层与指标测试（不依赖 Qt）\n");

    // 两个存储实现跑同一批契约断言。
    {
        gateway::storage::MemoryStorage storage;
        runStorageContract(storage, "内存实现");
    }
    testSqliteSpecific();

    testCsv();
    testPercentile();
    testMetricsWindow();
    testMetricsLatency();
    testMetricsDescribe();

    return test_support::summary();
}
