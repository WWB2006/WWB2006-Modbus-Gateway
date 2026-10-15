#include "gateway/storage/StorageInterface.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gateway::storage {
namespace {

// CSV 字段转义：含逗号、引号、换行时必须用双引号包住，内部引号翻倍。
// 中文标点（，）是全角，不会与分隔符冲突，但引号与换行必须处理，
// 否则 note 里的一句「设备返回异常」被截成两列，后面所有列都错位。
std::string escapeCsv(const std::string& text) {
    bool needsQuote = false;
    for (const char ch : text) {
        if (ch == ',' || ch == '"' || ch == '\n' || ch == '\r') {
            needsQuote = true;
            break;
        }
    }
    if (!needsQuote) {
        return text;
    }
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char ch : text) {
        if (ch == '"') {
            out.push_back('"');
        }
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

// 把一行 CSV 按「引号规则」切成字段。
std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool inQuotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inQuotes) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    current.push_back('"');
                    ++i;  // 跳过转义用的第二个引号
                } else {
                    inQuotes = false;
                }
            } else {
                current.push_back(ch);
            }
        } else if (ch == '"') {
            inQuotes = true;
        } else if (ch == ',') {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    fields.push_back(current);
    return fields;
}

}  // namespace

std::string toCsv(const std::vector<Sample>& samples) {
    std::ostringstream out;
    out << "timestamp_ms,device,address,value,function,valid,note\n";
    for (const Sample& sample : samples) {
        out << sample.timestampMs << ',' << escapeCsv(sample.deviceName) << ',' << sample.address
            << ',' << sample.value << ',' << static_cast<int>(sample.functionCode) << ','
            << (sample.valid ? 1 : 0) << ',' << escapeCsv(sample.note) << '\n';
    }
    return out.str();
}

bool exportCsv(const std::string& path, const std::vector<Sample>& samples, std::string& error) {
    error.clear();
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        error = "打不开导出文件：" + path;
        return false;
    }
    stream << toCsv(samples);
    if (!stream) {
        error = "写入导出文件失败：" + path;
        return false;
    }
    return true;
}

bool parseCsvLine(const std::string& line, Sample& out, std::string& error) {
    error.clear();
    const std::vector<std::string> fields = splitCsvLine(line);
    if (fields.size() < 7) {
        error = "CSV 列数不足（需要 7 列，实际 " + std::to_string(fields.size()) + "）";
        return false;
    }
    // 表头行不解析，直接报错让调用方跳过。
    if (fields[0] == "timestamp_ms") {
        error = "这是表头行";
        return false;
    }
    try {
        out.timestampMs = std::stoull(fields[0]);
        out.deviceName = fields[1];
        const unsigned long address = std::stoul(fields[2]);
        const unsigned long value = std::stoul(fields[3]);
        if (address > 65535 || value > 65535) {
            error = "地址或数值超出 0-65535";
            return false;
        }
        out.address = static_cast<uint16_t>(address);
        out.value = static_cast<uint16_t>(value);
        out.functionCode = static_cast<uint8_t>(std::stoul(fields[4]));
        out.valid = std::stoul(fields[5]) != 0;
        out.note = fields[6];
    } catch (const std::exception&) {
        error = "数值字段解析失败";
        return false;
    }
    return true;
}

}  // namespace gateway::storage
