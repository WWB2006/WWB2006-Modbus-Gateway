#pragma once

// 极简测试框架：不依赖 Qt 与任何第三方库，任何 C++17 编译器都能跑。
// Qt Test 用例（tests/unit）覆盖同一批向量，两者互为备份。

#include <cstdio>
#include <string>
#include <vector>

#include "gateway/protocol/ModbusCodec.h"

namespace test_support {

inline int g_checks = 0;
inline int g_failures = 0;
inline std::string g_section;

inline void section(const char* name) {
    g_section = name;
    std::printf("\n-- %s\n", name);
}

inline void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) {
        return;
    }
    ++g_failures;
    std::printf("  [FAIL] %s :: %s\n", g_section.c_str(), what.c_str());
}

inline std::string hexOf(const std::vector<uint8_t>& bytes) {
    return gateway::protocol::toHex(bytes);
}

inline void checkBytes(const std::vector<uint8_t>& actual, const std::string& expect,
                       const std::string& what) {
    const std::string got = hexOf(actual);
    check(got == expect, what + " 期望 [" + expect + "] 实际 [" + got + "]");
}

inline void checkU16(uint16_t actual, uint16_t expect, const std::string& what) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "0x%04X", actual);
    char expected[64];
    std::snprintf(expected, sizeof(expected), "0x%04X", expect);
    check(actual == expect, what + " 期望 " + expected + " 实际 " + buffer);
}

inline void checkU64(uint64_t actual, uint64_t expect, const std::string& what) {
    check(actual == expect, what + " 期望 " + std::to_string(expect) + " 实际 " +
                                std::to_string(actual));
}

// 越界时返回 0，避免断言在没有取到数据时触发未定义行为。
inline uint16_t valueAt(const std::vector<uint16_t>& values, std::size_t index) {
    return index < values.size() ? values[index] : static_cast<uint16_t>(0);
}

inline int summary() {
    std::printf("\n断言 %d 项，失败 %d 项\n", g_checks, g_failures);
    if (g_failures != 0) {
        std::printf("结果：失败\n");
        return 1;
    }
    std::printf("结果：全部通过\n");
    return 0;
}

}  // namespace test_support
