#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace gateway::json {

// 通用极简 JSON 值。
//
// 只覆盖本项目配置文件用到的子集：对象、数组、字符串、数字、true/false/null。
// 不做 \u 转义（配置文件里不会出现），与其写半套转义逻辑不如明确不支持 ——
// 避免「看起来支持、实际有坑」。
//
// 为什么不用第三方库（nlohmann/json 之类）：
// 项目卖点之一是「没装 Qt 也能构建」，引入一个头文件库会把这条卖点变成
// 「再下一个头文件也能构建」。而两份配置结构都很简单，不值得付这个依赖成本。
class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    // 查对象成员；不存在返回 nullptr。这是最常用的访问方式。
    const Value* member(const std::string& key) const {
        for (const auto& entry : object) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    bool isObject() const noexcept { return type == Type::Object; }
    bool isArray() const noexcept { return type == Type::Array; }
    bool isNumber() const noexcept { return type == Type::Number; }
    bool isString() const noexcept { return type == Type::String; }
    bool isBool() const noexcept { return type == Type::Bool; }
};

// 解析一段 JSON 文本。失败时返回 false 并填写 error（含行列号）。
bool parse(const std::string& text, Value& out, std::string& error);

}  // namespace gateway::json
